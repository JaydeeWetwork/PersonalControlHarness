// Permission manifest schema + install/enable approval flow (implementation).
#include "litholattice/mods/mod_permissions.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "litholattice/core/json.h"
#include "litholattice/core/log.h"
#include "litholattice/mods/mod_loader.h"  // ModInfo (resolve's input)
#include "litholattice/storage/file_contract.h"  // writeFileAtomic

namespace litholattice::mods {
namespace {

// Canonical name table: the single source of truth for manifest permission
// names. ModLoader::parsePermissions routes through permissionBitForName so
// a name can never parse to a bit the manifest schema does not know.
struct NameBit {
  const char* name;
  uint32_t bit;
};
constexpr NameBit kNameBits[] = {
    {"blocks", kPermBlocks},   {"world_read", kPermWorldRead},
    {"world_write", kPermWorldWrite}, {"tick", kPermTick},
    {"events", kPermEvents},   {"ui", kPermUI},
    {"input", kPermInput},     {"assets", kPermAssets},
    {"net", kPermNet},         {"console", kPermConsole},
    {"player_control", kPermPlayerControl},
    {"pilot_senses", kPermPilotSenses},
};

void appendNames(uint32_t bits, std::string& out) {
  bool first = true;
  for (const NameBit& nb : kNameBits)
    if (bits & nb.bit) {
      if (!first) out += ", ";
      out += nb.name;
      first = false;
    }
  if (first) out += "(none)";
}

// On-disk format version of the grants file.
constexpr uint32_t kGrantsFileFormat = 1;

}  // namespace

uint32_t permissionBitForName(std::string_view name) {
  for (const NameBit& nb : kNameBits)
    if (name == nb.name) return nb.bit;
  return 0;
}

const char* permissionNameForBit(uint32_t bit) {
  for (const NameBit& nb : kNameBits)
    if (nb.bit == bit) return nb.name;
  return nullptr;
}

uint32_t denyAllPermissions(const PermissionRequest&) {
  return 0;  // headless-safe default: DENY unless approved
}

// ---- PermissionStore ----

PermissionStore::PermissionStore(std::string path) : path_(std::move(path)) {
  if (!path_.empty()) load();
}

bool PermissionStore::load() {
  grants_.clear();
  dirty_ = false;
  if (path_.empty()) return true;
  std::ifstream in(path_, std::ios::binary);
  if (!in) return true;  // missing file: fresh store, not an error
  std::stringstream ss;
  ss << in.rdbuf();
  Json j;
  std::string err;
  if (!Json::parse(ss.str(), j, &err) || j.type != Json::Type::Object) {
    logError("permission grants: %s: %s (treated as empty)",
             path_.c_str(), err.empty() ? "not an object" : err.c_str());
    return false;  // corrupt file: treated as empty, never fatal
  }
  const Json* grants = j.get("grants");
  if (!grants || grants->type != Json::Type::Object) return true;
  for (const auto& [id, g] : grants->obj) {
    if (g.type != Json::Type::Object) continue;
    StoredGrant sg;
    sg.modVersion = g.getStr("version");
    sg.permissionSetVersion = static_cast<uint32_t>(g.getNum("permissionSet", 0));
    auto readBits = [&](const char* key) {
      uint32_t bits = 0;
      const Json* a = g.get(key);
      if (a && a->type == Json::Type::Array)
        for (const Json& e : a->arr)
          if (e.type == Json::Type::String) bits |= permissionBitForName(e.str);
      return bits;
    };
    sg.requested = readBits("requested");
    sg.granted = readBits("granted");
    sg.denied = readBits("denied");
    grants_[id] = sg;
  }
  return true;
}

bool PermissionStore::save() {
  if (path_.empty() || !dirty_) return true;
  Json root = Json::object();
  root.add("format", Json::number(kGrantsFileFormat));
  Json& grants = root.add("grants", Json::object());
  for (const auto& [id, sg] : grants_) {
    Json& g = grants.add(id, Json::object());
    g.add("version", Json::string(sg.modVersion));
    g.add("permissionSet", Json::number(sg.permissionSetVersion));
    auto writeBits = [&](const char* key, uint32_t bits) {
      Json& a = g.add(key, Json::array());
      for (const NameBit& nb : kNameBits)
        if (bits & nb.bit) a.arr.push_back(Json::string(nb.name));
    };
    writeBits("requested", sg.requested);
    writeBits("granted", sg.granted);
    writeBits("denied", sg.denied);
  }
  const std::string text = root.dump(2);
  // Grants are derived state (re-promptable), so no backup rotation: a torn
  // write is worse than a lost prompt history.
  storage::FileWriteOptions opts;
  opts.keepBackups = 0;
  std::string err;
  if (!storage::writeFileAtomic(path_, reinterpret_cast<const uint8_t*>(text.data()),
                                text.size(), &err, opts)) {
    logError("permission grants: cannot save %s: %s", path_.c_str(), err.c_str());
    return false;
  }
  dirty_ = false;
  return true;
}

const StoredGrant* PermissionStore::lookup(const std::string& modId) const {
  const auto it = grants_.find(modId);
  return it == grants_.end() ? nullptr : &it->second;
}

void PermissionStore::record(const std::string& modId, StoredGrant grant) {
  grants_[modId] = std::move(grant);
  dirty_ = true;
}

// ---- ModPermissionGate ----

bool ModPermissionGate::isTrustedFirstParty(const std::string& modId) const {
  return std::find(firstPartyIds_.begin(), firstPartyIds_.end(), modId) !=
         firstPartyIds_.end();
}

PermissionResolution ModPermissionGate::resolve(const ModInfo& mod, PermissionStore& store,
                                                PermissionPromptFn prompt) {
  PermissionResolution out;
  const uint32_t requested = mod.permissions & kAllPermissionBits;

  // 1. Nothing requested: loads clean with zero permissions, no prompt.
  if (requested == 0) {
    out.action = ModLoadAction::kLoad;
    out.reason = "mod '" + mod.id + "': no permissions requested";
    logInfo("permissions: %s", out.reason.c_str());
    return out;
  }

  // 2. First-party verification. The manifest claim alone never grants:
  // the id must be on the engine's trusted list. Verified first-party mods
  // (shipped with the game) skip the prompt — prompting for the game's own
  // mods would be theater — but the grant is still logged.
  if (mod.firstPartyClaim && !isTrustedFirstParty(mod.id)) {
    logError("permissions: mod '%s' claims firstParty but is not on the trusted list: "
             "treated as third-party",
             mod.id.c_str());
  }
  if (mod.firstPartyClaim && isTrustedFirstParty(mod.id)) {
    out.action = ModLoadAction::kLoad;
    out.granted = requested;
    out.reason = "mod '" + mod.id + "': first-party (verified claim), all permissions granted";
    logInfo("permissions: %s", out.reason.c_str());
    StoredGrant sg;
    sg.modVersion = mod.version;
    sg.permissionSetVersion = mod.permissionSetVersion;
    sg.requested = requested;
    sg.granted = requested;
    store.record(mod.id, sg);
    return out;
  }

  // 3. Store lookup + diff. Re-prompt ONLY for bits the manifest asks for
  // that the store has not seen before (mod update requesting NEW
  // permissions), or — when the permission set moved — for bits not already
  // approved. An explicit denial persists across updates: the user is never
  // re-prompted for a bit they already denied.
  //
  // Review F-M7 (grant binding): a stored grant is honored ONLY for the
  // exact (id, version) it was recorded under. The store's on-disk format
  // already carries the version per record (StoredGrant::modVersion), so
  // binding to id+version needs no format break; a signer hash cannot be
  // bound because neither ModInfo nor the store carries one (documented
  // limitation in mod_permissions.h). A version mismatch therefore treats
  // the record as absent: grants AND denials are re-decided for the new
  // version, so a different mod (or a changed mod) reusing an approved id
  // inherits nothing silently.
  const StoredGrant* stored = store.lookup(mod.id);
  const bool versionMatch = stored && stored->modVersion == mod.version;
  if (stored && !versionMatch)
    logError("permissions: mod '%s': stored grants are for version '%s', manifest is '%s' — "
             "grants are bound to id+version and do not carry over; re-deciding",
             mod.id.c_str(), stored->modVersion.c_str(), mod.version.c_str());
  const StoredGrant* prev = versionMatch ? stored : nullptr;
  const uint32_t prevRequested = prev ? prev->requested : 0;
  const uint32_t prevGranted = prev ? prev->granted : 0;
  const uint32_t prevDenied = prev ? prev->denied : 0;
  const bool setChanged = !prev || prev->permissionSetVersion != kPermissionSetVersion;
  uint32_t newBits = setChanged ? (requested & ~prevGranted)
                                : (requested & ~prevRequested);

  // Content mods auto-approve the content-safe subset (blocks, assets) with
  // no prompt; every other bit — for any mod type — goes through the prompt
  // path, where the headless default denies it.
  uint32_t granted = prevGranted & requested;
  uint32_t denied = prevDenied & requested;
  if (mod.type == "content") {
    const uint32_t safe = requested & kContentSafePermissions & ~prevRequested;
    granted |= safe;
    newBits &= ~kContentSafePermissions;
  }

  if (newBits != 0) {
    PermissionRequest req;
    req.modId = mod.id;
    req.modVersion = mod.version;
    req.modType = mod.type;
    req.permissionSetVersion = mod.permissionSetVersion;
    req.firstPartyClaim = mod.firstPartyClaim;
    req.requested = requested;
    req.alreadyGranted = granted;
    for (const NameBit& nb : kNameBits)
      if (newBits & nb.bit) {
        PermissionRequestItem item;
        item.bit = nb.bit;
        item.name = nb.name;
        const auto it = mod.permissionRationales.find(nb.name);
        item.rationale = (it != mod.permissionRationales.end()) ? it->second : "";
        item.previouslyDenied = (prevDenied & nb.bit) != 0;
        req.items.push_back(std::move(item));
      }
    const uint32_t approved = prompt ? (prompt(req) & newBits) : 0;
    granted |= approved;
    denied |= (newBits & ~approved);
    std::string decided;
    appendNames(newBits, decided);
    std::string okNames;
    appendNames(approved, okNames);
    logInfo("permissions: mod '%s' requested new permission(s) %s: approved %s",
            mod.id.c_str(), decided.c_str(), okNames.c_str());

    StoredGrant sg;
    sg.modVersion = mod.version;
    sg.permissionSetVersion = kPermissionSetVersion;
    sg.requested = requested;
    sg.granted = granted & requested;
    sg.denied = denied & requested;
    store.record(mod.id, sg);
  } else if (stored && !versionMatch) {
    // No prompt was needed (e.g. a content mod whose only requested bits
    // are content-safe and auto-approved), but the stored record belongs to
    // a different version: replace it with a fresh record for THIS
    // (id, version) holding only what this resolve actually decided —
    // never a copy of the old version's grants (F-M7).
    StoredGrant sg;
    sg.modVersion = mod.version;
    sg.permissionSetVersion = kPermissionSetVersion;
    sg.requested = requested;
    sg.granted = granted & requested;
    sg.denied = denied & requested;
    store.record(mod.id, sg);
  }

  // 4. Verdict. Denied load-critical bits refuse the load; everything else
  // denied is stripped and the mod runs reduced. Stripping is always logged:
  // a headless engine never silently elevates a mod.
  out.granted = granted & requested;
  out.denied = denied & requested;
  out.stripped = requested & ~out.granted;
  const uint32_t criticalDenied = (requested & mod.criticalPermissions) & ~out.granted;
  if (criticalDenied != 0) {
    out.action = ModLoadAction::kRefuse;
    std::string names;
    appendNames(criticalDenied, names);
    out.reason = "mod '" + mod.id + "': load-critical permission(s) denied: " + names;
    logError("permissions: %s", out.reason.c_str());
    return out;
  }
  if (out.stripped != 0) {
    out.action = ModLoadAction::kLoadStripped;
    std::string names;
    appendNames(out.stripped, names);
    out.reason = "mod '" + mod.id + "': loading with stripped permission(s): " + names;
    logError("permissions: %s (headless-safe default: DENY unless approved)",
             out.reason.c_str());
    return out;
  }
  out.action = ModLoadAction::kLoad;
  out.reason = "mod '" + mod.id + "': all requested permissions granted";
  logInfo("permissions: %s", out.reason.c_str());
  return out;
}

}  // namespace litholattice::mods
