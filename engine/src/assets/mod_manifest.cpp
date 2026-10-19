#include "litholattice/assets/mod_manifest.h"
#include "litholattice/assets/mod_signature.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include "litholattice/core/json.h"

namespace litholattice {

std::optional<SemVer> SemVer::parse(const std::string& s) {
  SemVer v;
  int* parts[3] = {&v.major, &v.minor, &v.patch};
  const char* p = s.data();
  const char* end = s.data() + s.size();
  for (int i = 0; i < 3; ++i) {
    if (p == end || *p < '0' || *p > '9') return std::nullopt;
    const auto r = std::from_chars(p, end, *parts[i]);
    if (r.ec != std::errc()) return std::nullopt;
    p = r.ptr;
    if (i < 2) { if (p == end || *p != '.') return std::nullopt; ++p; }
  }
  if (p != end) return std::nullopt;
  return v;
}
std::string SemVer::str() const { return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch); }

const std::vector<std::string>& knownModApis() {
  static const std::vector<std::string> apis = {"world.read", "world.write", "ui", "audio", "input", "filesystem.save",
                                                "filesystem.mod", "network", "native"};
  return apis;
}

/// Shape check for a dependency version range (P1b, 2026-10-13, Appendix E
/// M1). Valid: "*", a bare SemVer, or one of the prefixes ">=", "=", "^",
/// "~" followed by a SemVer. EVERYTHING else — the empty string, a lone
/// prefix (">", ">=", "^", ...), a prefix with garbage after it — is an
/// invalid range. This function never indexes or substrs an unchecked
/// string: the old inline check in ModManifest::parse indexed range[0] and
/// called substr(1)/substr(2) on possibly-empty/short ranges, throwing
/// std::out_of_range out of the parser and aborting boot on a crafted
/// mod.json (present-but-empty "version" beats the "*" default).
bool isValidVersionRange(const std::string& range) {
  if (range == "*") return true;
  if (range.empty()) return false;
  size_t prefix = 0;
  if (range.rfind(">=", 0) == 0) prefix = 2;
  else if (range[0] == '=' || range[0] == '^' || range[0] == '~') prefix = 1;
  // prefix < range.size() is guaranteed SemVer-parseable input length-wise:
  // substr(prefix) is only taken when something follows the prefix (a bare
  // ">" is not a recognized prefix, so it falls through as prefix 0 and the
  // whole-string parse rejects it).
  if (prefix >= range.size()) return false;
  return SemVer::parse(range.substr(prefix)).has_value();
}

bool versionSatisfies(const SemVer& v, const std::string& range) {
  if (range.empty() || range == "*") return true;
  auto at = [&](size_t n) { return SemVer::parse(range.substr(n)); };
  if (range.rfind(">=", 0) == 0) { const auto b = at(2); return b && v >= *b; }
  if (range[0] == '=') { const auto b = at(1); return b && v == *b; }
  if (range[0] == '^') {
    const auto b = at(1);
    return b && v >= *b && (b->major ? v.major == b->major : v.minor == b->minor);
  }
  const auto b = SemVer::parse(range);
  return b && v == *b;
}

std::optional<ModManifest> ModManifest::parse(const std::string& text, std::string* err) {
  auto fail = [&](const std::string& why) -> std::optional<ModManifest> { if (err) *err = "mod.json: " + why; return std::nullopt; };
  Json j;
  if (!Json::parse(text, j, err)) return std::nullopt;
  if (j.type != Json::Type::Object) return fail("must be an object");
  ModManifest m;
  // Clamp in double before converting (P3-3, Appendix E L4): int(1e30) on a
  // hostile "schema" number is UB before the != 1 check below can run.
  m.schema = int(std::clamp(j.getNum("schema", 1), -1e6, 1e6));
  if (m.schema != 1) return fail("unsupported schema " + std::to_string(m.schema));
  m.id = j.getStr("id");
  const bool idOk = !m.id.empty() && m.id.size() <= 64 &&
                    std::all_of(m.id.begin(), m.id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
  if (!idOk) return fail("id must be 1-64 chars of [a-z0-9_]");
  m.name = j.getStr("name", m.id);
  const auto ver = SemVer::parse(j.getStr("version"));
  if (!ver) return fail("version must be MAJOR.MINOR.PATCH");
  m.version = *ver;
  const std::string type = j.getStr("type", "content");
  if (type == "content") m.type = ModType::Content;
  else if (type == "script") m.type = ModType::Script;
  else if (type == "native") m.type = ModType::Native;
  else return fail("type must be content|script|native");
  m.hotSwap = j.getBool("hot_swap", false);
  m.sealed = j.getBool("sealed", false);
  if (const Json* deps = j.get("deps")) {
    if (deps->type != Json::Type::Array) return fail("deps must be an array");
    for (const Json& d : deps->arr) {
      Dep dep{d.getStr("id"), d.getStr("version", "*")};
      if (dep.id.empty() || dep.id == m.id) return fail("dep needs an id other than the mod's own");
      // Range shape is validated BEFORE any parse/index (see
      // isValidVersionRange): an empty or malformed range is a manifest
      // error returned via fail(), never an exception.
      if (!isValidVersionRange(dep.range))
        return fail("dep '" + dep.id + "': bad version range '" + dep.range + "'");
      m.deps.push_back(std::move(dep));
    }
  }
  if (const Json* perms = j.get("permissions")) {
    if (perms->type != Json::Type::Array) return fail("permissions must be an array");
    for (const Json& p : perms->arr) {
      Permission perm{p.getStr("api"), p.getStr("reason")};
      const auto& apis = knownModApis();
      if (std::find(apis.begin(), apis.end(), perm.api) == apis.end()) return fail("unknown permission api '" + perm.api + "'");
      if (perm.reason.empty()) return fail("permission '" + perm.api + "' needs a reason");
      m.permissions.push_back(std::move(perm));
    }
  }
  if (m.type == ModType::Content && !m.permissions.empty()) return fail("content mods cannot request permissions");
  return m;
}

ModSet resolveMods(std::vector<ModManifest> mods, const std::vector<ModManifest>& provided) {
  ModSet out;
  std::map<std::string, const ModManifest*> byId;
  for (const ModManifest& p : provided) byId[p.id] = &p;
  std::vector<ModManifest> unique;
  for (ModManifest& m : mods) {
    if (byId.contains(m.id) || std::any_of(unique.begin(), unique.end(), [&](const ModManifest& u) { return u.id == m.id; })) {
      out.errors.push_back("mod '" + m.id + "': duplicate id, skipped");
      continue;
    }
    unique.push_back(std::move(m));
  }
  std::map<std::string, size_t> idx;
  for (size_t i = 0; i < unique.size(); ++i) idx[unique[i].id] = i;
  std::vector<int> state(unique.size(), 0);  // 0 new, 1 visiting, 2 done ok, 3 failed
  // Depth cap (P3-3, Appendix E L5): the walk is recursive per dependency
  // edge; a directory of N chained mods would otherwise recurse N deep.
  // 64 matches the alias-cycle precedent in the world module.
  constexpr int kMaxDepDepth = 64;
  int depth = 0;
  std::function<bool(size_t)> visit = [&](size_t i) -> bool {
    if (depth >= kMaxDepDepth) {
      out.errors.push_back("mod '" + unique[i].id + "': dependency chain deeper than " +
                           std::to_string(kMaxDepDepth) + " (refused)");
      state[i] = 3;
      return false;
    }
    ++depth;
    struct DepthGuard { int& d; ~DepthGuard() { --d; } } guard{depth};
    if (state[i] == 2) return true;
    if (state[i] == 3) return false;
    if (state[i] == 1) { out.errors.push_back("mod '" + unique[i].id + "': dependency cycle"); state[i] = 3; return false; }
    state[i] = 1;
    for (const auto& d : unique[i].deps) {
      const ModManifest* dm = nullptr;
      bool ok = true;
      if (const auto p = std::find_if(provided.begin(), provided.end(), [&](const ModManifest& x) { return x.id == d.id; }); p != provided.end()) dm = &*p;
      else if (const auto it = idx.find(d.id); it != idx.end()) { ok = visit(it->second); dm = &unique[it->second]; }
      if (!dm || !ok) { out.errors.push_back("mod '" + unique[i].id + "': needs '" + d.id + "', which is missing or failed"); state[i] = 3; return false; }
      if (!versionSatisfies(dm->version, d.range)) {
        out.errors.push_back("mod '" + unique[i].id + "': needs '" + d.id + "' " + d.range + ", found " + dm->version.str());
        state[i] = 3;
        return false;
      }
    }
    if (state[i] == 3) return false;
    state[i] = 2;
    out.ordered.push_back(unique[i]);
    return true;
  };
  for (size_t i = 0; i < unique.size(); ++i) visit(i);
  return out;
}

std::vector<std::filesystem::path> contentModPacks(const ModSet& set) {
  std::vector<std::filesystem::path> out;
  for (const ModManifest& m : set.ordered)
    if (m.type == ModType::Content && !m.dir.empty()) out.push_back(m.dir);
  return out;
}

ModSet loadModDirectory(const std::filesystem::path& dir, const std::vector<ModManifest>& provided) {
  std::vector<ModManifest> mods;
  std::vector<std::string> errors;
  std::error_code ec;
  std::vector<std::filesystem::path> folders;
  // Containment (P1b, 2026-10-13, Appendix E M1, layer 2): one hostile mod
  // folder must NEVER abort boot. Directory enumeration, each mod's
  // manifest parse, and its signature verification are each wrapped so an
  // exception skips ONLY that mod (recorded in errors, which the caller
  // logs loudly) and the scan continues with the remaining mods. The
  // parser itself is also fixed to validate before indexing (layer 1,
  // isValidVersionRange) — this boundary is defence in depth for any
  // future parser throw, not a substitute for it.
  try {
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
      if (e.is_directory() && std::filesystem::exists(e.path() / "mod.json")) folders.push_back(e.path());
  } catch (const std::exception& ex) {
    errors.push_back("mod scan of '" + dir.string() + "' aborted during enumeration: " + ex.what());
    ModSet set = resolveMods(std::move(mods), provided);
    set.errors.insert(set.errors.begin(), errors.begin(), errors.end());
    return set;
  }
  std::sort(folders.begin(), folders.end());
  for (const auto& f : folders) {
    try {
      std::ifstream in(f / "mod.json", std::ios::binary);
      std::stringstream ss;
      ss << in.rdbuf();
      std::string err;
      if (auto m = ModManifest::parse(ss.str(), &err)) {
        m->dir = f;
        // Verify the mod signature (fail closed: Invalid refuses the mod).
        std::string sigErr;
        m->signatureStatus = assets::verifyModSignature(f, &sigErr);
        if (m->signatureStatus == SignatureStatus::Invalid) {
          errors.push_back(f.filename().string() + ": signature invalid (" + sigErr + ") - refused");
          continue;
        }
        mods.push_back(std::move(*m));
      }
      else errors.push_back(f.filename().string() + ": " + err);
    } catch (const std::exception& ex) {
      errors.push_back(f.filename().string() + ": manifest parse threw (" + ex.what() + ") - mod skipped");
    } catch (...) {
      errors.push_back(f.filename().string() + ": manifest parse threw an unknown exception - mod skipped");
    }
  }
  ModSet set = resolveMods(std::move(mods), provided);
  set.errors.insert(set.errors.begin(), errors.begin(), errors.end());
  return set;
}

}  // namespace litholattice
