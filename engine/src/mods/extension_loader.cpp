// ExtensionLoader: recursive extension loading, scoped contexts, privilege
// default-deny, cascade unload. See extension_loader.h for the contracts.
#include "litholattice/mods/extension_loader.h"

#include <algorithm>
#include <cctype>

#include "litholattice/core/log.h"
#include "litholattice/mods/extension_budget.h"

namespace litholattice {
namespace {

// ---- semver range matching (same syntax as mod.json "deps"/"version":
// "*", "=x.y.z", ">=x.y.z", "^x.y.z", or a bare "x.y.z" meaning exact) ----

struct Semver {
  uint32_t major = 0, minor = 0, patch = 0;
};

std::string trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return {};
  const size_t b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}

// Fail-closed digits -> uint32: std::stoul throws std::out_of_range on huge
// inputs (uncaught = process crash), and silently narrows values in
// (2^32, 2^64) on 64-bit. A bad version refuses the match, never crashes.
bool parseUint32(const std::string& tok, uint32_t& out) {
  if (tok.empty() || tok.find_first_not_of("0123456789") != std::string::npos)
    return false;
  if (tok.size() > 10) return false;  // 10 digits max for a uint32
  unsigned long v = 0;
  try {
    v = std::stoul(tok);
  } catch (...) {
    return false;
  }
  if (v > 0xFFFFFFFFul) return false;
  out = static_cast<uint32_t>(v);
  return true;
}

bool parseSemver(const std::string& s, Semver& out) {
  uint32_t parts[3] = {0, 0, 0};
  size_t pos = 0;
  for (int i = 0; i < 3; ++i) {
    const size_t dot = s.find('.', pos);
    const std::string tok =
        (dot == std::string::npos) ? s.substr(pos) : s.substr(pos, dot - pos);
    if (!parseUint32(tok, parts[i])) return false;
    if (dot == std::string::npos) {
      if (i < 2) {
        // "1" or "1.2": pad with zeros.
        for (int j = i + 1; j < 3; ++j) parts[j] = 0;
      }
      break;
    }
    pos = dot + 1;
  }
  out = {parts[0], parts[1], parts[2]};
  return true;
}

int cmpSemver(const Semver& a, const Semver& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  return 0;
}

// Fail-closed: an unparseable range or version never matches.
bool versionSatisfies(const char* range, const std::string& version) {
  if (!range) return false;
  const std::string r = trim(range);
  if (r.empty() || r == "*") return true;
  Semver v;
  if (!parseSemver(version, v)) return false;
  Semver bound;
  if (r.rfind(">=", 0) == 0)
    return parseSemver(r.substr(2), bound) && cmpSemver(v, bound) >= 0;
  if (r.rfind("^", 0) == 0)
    return parseSemver(r.substr(1), bound) && v.major == bound.major &&
           cmpSemver(v, bound) >= 0;
  if (r.rfind("=", 0) == 0)
    return parseSemver(r.substr(1), bound) && cmpSemver(v, bound) == 0;
  return parseSemver(r, bound) && cmpSemver(v, bound) == 0;  // bare "x.y.z"
}

}  // namespace

void ExtensionLoader::registerStaticModule(StaticModule module) {
  if (!ExtensionRegistry::isValidId(module.id)) {
    logError("mods: refusing static module with invalid id '%s'", module.id.c_str());
    return;
  }
  if (staticModules_.count(module.id)) {
    logError("mods: duplicate static module id '%s' refused (first registration wins)",
             module.id.c_str());
    return;
  }
  staticModules_.emplace(module.id, std::move(module));
}

bool ExtensionLoader::validateFacet(const LithoLatticeExtensible* facet,
                                    std::string& err) const {
  if (!facet) {
    err = "null facet";
    return false;
  }
  if (facet->size == 0) {
    err = "facet has zero size (not a size-first struct)";
    return false;
  }
  if (facet->extensionCount > 0 && !facet->extensions) {
    err = "facet declares extensions but the decl array is null";
    return false;
  }
  if (!facet->childApi) {
    err = "facet has no childApi: an extensible parent must expose an API surface";
    return false;
  }
  if (facet->childApi->size == 0) {
    err = "facet childApi has zero size (not a size-first struct)";
    return false;
  }
  return true;
}

uint32_t ExtensionLoader::effectiveShare(const std::string& parentPath,
                                          uint32_t declIndex,
                                          uint32_t declared) const {
  const auto it = sharePlan_.find(parentPath);
  if (it == sharePlan_.end() || declIndex >= it->second.size()) return declared;
  return it->second[declIndex];
}

bool ExtensionLoader::loadExtension(const std::string& parentPath,
                                    const LithoLatticeExtensionDecl& decl,
                                    uint32_t declIndex,
                                    const LithoLatticeExtensible* parentFacet,
                                    std::vector<std::string>& ancestors,
                                    std::vector<std::string>& added,
                                    std::string& err) {
  if (!decl.id || !ExtensionRegistry::isValidId(decl.id)) {
    err = "parent '" + parentPath + "': extension decl has an invalid id";
    return false;
  }
  const std::string id = decl.id;
  // Cycle detection: the extension id must not already appear on the current
  // recursion path. This covers self-declaration (parent declares itself)
  // and mutual/ancestor cycles (A -> B -> A).
  if (std::find(ancestors.begin(), ancestors.end(), id) != ancestors.end()) {
    err = "extension cycle rejected: '" + id + "' is already an ancestor of '" +
          parentPath + "' (a mod may not host itself or its ancestors)";
    return false;
  }
  const auto mit = staticModules_.find(id);
  if (mit == staticModules_.end()) {
    err = "parent '" + parentPath + "': extension '" + id +
          "' not found (v1 static linking: no module registered under that id)";
    return false;
  }
  const StaticModule& child = mit->second;
  if (!versionSatisfies(decl.versionRange, child.version)) {
    err = "parent '" + parentPath + "': extension '" + id + "' version " +
          child.version + " does not satisfy range '" +
          (decl.versionRange ? decl.versionRange : "(null)") + "'";
    return false;
  }
  if (decl.budgetShare > 100) {
    err = "parent '" + parentPath + "': extension '" + id + "' budgetShare " +
          std::to_string(decl.budgetShare) + " > 100";
    return false;
  }
  const std::string path = registry_.registerExtension(parentPath, id);
  if (path.empty()) {
    // Distinguish the two refusal reasons for a clear error.
    uint32_t parentDepth = 0;
    if (registry_.depthOf(parentPath, parentDepth) &&
        parentDepth + 1 > registry_.maxExtensionDepth()) {
      err = "depth cap: '" + ExtensionRegistry::joinPath(parentPath, id) +
            "' would sit at depth " + std::to_string(parentDepth + 1) +
            ", over the max of " + std::to_string(registry_.maxExtensionDepth()) +
            " (fail-closed: refused, not truncated)";
    } else {
      err = "parent '" + parentPath + "': duplicate extension id '" + id + "'";
    }
    return false;
  }

  uint32_t depth = 0;
  registry_.depthOf(path, depth);
  ExtensionContext ctx;
  ctx.path = path;
  ctx.parentPath = parentPath;
  ctx.depth = depth;
  // Privilege default-deny: the parent's isFirstParty NEVER propagates; only
  // an explicit facet grant elevates this extension.
  ctx.isFirstParty = LithoLatticeExtensibleGrantsFirstParty(parentFacet, id.c_str()) != 0;
  // Sibling-share validation (worker 3): the share plan was warn-and-clamped
  // from the parent facet, so the context always carries an effective share.
  ctx.budgetShare = effectiveShare(parentPath, declIndex, decl.budgetShare);
  // Capability scoping: the extension sees ONLY the parent's filtered
  // childApi — never the full LithoLatticeModApi.
  ctx.childApi = parentFacet->childApi;
  contexts_.emplace(path, std::move(ctx));
  added.push_back(path);
  lastLoadOrder_.push_back(path);

  // Recurse: the child may itself be an extensible parent.
  const LithoLatticeExtensible* childFacet =
      LithoLatticeExtensibleQuery(child.extensibleQuery);
  if (childFacet) {
    if (!validateFacet(childFacet, err)) return false;
    // Warn-and-clamp this level's sibling shares before recursing.
    sharePlan_[path] = ExtensionBudget::clampShares(childFacet);
    ancestors.push_back(id);
    for (uint32_t i = 0; i < childFacet->extensionCount; ++i) {
      if (!loadExtension(path, childFacet->extensions[i], i, childFacet, ancestors,
                         added, err))
        return false;
    }
    ancestors.pop_back();
  }
  return true;
}

bool ExtensionLoader::loadMod(const std::string& id, std::string& err) {
  if (loadedMods_.count(id)) return true;  // idempotent
  const auto it = staticModules_.find(id);
  if (it == staticModules_.end()) {
    err = "unknown mod id '" + id +
          "' (v1 static linking: the id must be registered first)";
    return false;
  }
  const StaticModule& mod = it->second;
  if (!registry_.registerMod(id)) {
    err = "mod '" + id + "' is already registered or has an invalid id";
    return false;
  }

  lastLoadOrder_.clear();
  lastLoadOrder_.push_back(id);
  std::vector<std::string> added;  // dotted paths, for rollback on failure

  const LithoLatticeExtensible* facet =
      LithoLatticeExtensibleQuery(mod.extensibleQuery);
  bool ok = true;
  if (facet) {
    if (validateFacet(facet, err)) {
      // Sibling-share validation (worker 3): warn-and-clamp this level's
      // shares so every context carries an effective share summing <= 100.
      sharePlan_[id] = ExtensionBudget::clampShares(facet);
      std::vector<std::string> ancestors{id};
      for (uint32_t i = 0; i < facet->extensionCount; ++i) {
        if (!loadExtension(id, facet->extensions[i], i, facet, ancestors, added, err)) {
          ok = false;
          break;
        }
      }
    } else {
      ok = false;
    }
  }

  // The share plan is per-load scratch: never outlive the call, on either path.
  sharePlan_.clear();

  if (!ok) {
    // Fail-closed: roll back every node this call registered. Partially
    // loaded extensions never completed loading, so no onUnload hooks run
    // during rollback — there is nothing coherent to tear down yet.
    for (auto rit = added.rbegin(); rit != added.rend(); ++rit) {
      contexts_.erase(*rit);
      registry_.unregister(*rit);
    }
    registry_.unregister(id);
    lastLoadOrder_.clear();
    logError("mods: load of '%s' failed, rolled back: %s", id.c_str(), err.c_str());
    return false;
  }

  loadedMods_.insert(id);
  logInfo("mods: loaded '%s' (+%zu extension(s))", id.c_str(), added.size());
  return true;
}

void ExtensionLoader::collectSubtree(const std::string& path,
                                     std::vector<std::string>& out) const {
  out.push_back(path);
  for (const std::string& child : registry_.childrenOf(path))
    collectSubtree(child, out);
}

void ExtensionLoader::unloadMod(const std::string& id) {
  lastUnloadOrder_.clear();
  if (!registry_.contains(id)) return;  // unknown: no-op
  std::vector<std::string> subtree;
  collectSubtree(id, subtree);
  // Children before parent: reverse depth order. stable_sort keeps sibling
  // order deterministic (registration order).
  std::stable_sort(subtree.begin(), subtree.end(),
                   [this](const std::string& a, const std::string& b) {
                     uint32_t da = 0, db = 0;
                     registry_.depthOf(a, da);
                     registry_.depthOf(b, db);
                     return da > db;
                   });
  for (const std::string& path : subtree) {
    const ExtensionNode* node = registry_.find(path);
    if (node) {
      const auto mit = staticModules_.find(node->id);
      if (mit != staticModules_.end() && mit->second.imod &&
          mit->second.imod->onUnload) {
        mit->second.imod->onUnload(nullptr);  // v1: no live handles in the loader
      }
    }
    contexts_.erase(path);
    registry_.unregister(path);
    lastUnloadOrder_.push_back(path);
  }
  loadedMods_.erase(id);
  logInfo("mods: unloaded '%s' (+%zu extension(s)), children-first", id.c_str(),
          subtree.size() > 0 ? subtree.size() - 1 : 0);
}

const ExtensionContext* ExtensionLoader::contextFor(const std::string& path) const {
  const auto it = contexts_.find(path);
  return it == contexts_.end() ? nullptr : &it->second;
}

}  // namespace litholattice
