// ModLoader: mod.json discovery, validation, and SDK-version enforcement.
// The load path (load()) refuses mods on SDK major-version mismatch and
// warns-and-continues on minor mismatch (JD's call, 2026-10-11).
#include "litholattice/mods/mod_loader.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "litholattice/core/json.h"
#include "litholattice/core/log.h"
#include "litholattice/mods/extension_registry.h"  // isValidId for extension decls

namespace litholattice {
namespace {

// Engine SDK as semver: LITHOLATTICE_SDK_VERSION is the major line.
constexpr uint32_t kEngineSdkMajor = LITHOLATTICE_SDK_VERSION;
constexpr uint32_t kEngineSdkMinor = 0;
constexpr uint32_t kEngineSdkPatch = 0;

struct Semver {
  uint32_t major = 0, minor = 0, patch = 0;
};

// Fail-closed digits -> uint32: std::stoul throws std::out_of_range on huge
// inputs (uncaught = process crash at startup on a malformed mod.json), and
// silently narrows values in (2^32, 2^64) on 64-bit. Either way a bad
// manifest must refuse the mod, never crash or wrap.
bool parseUint32(const std::string& tok, uint32_t& out) {
  if (tok.empty() || tok.find_first_not_of("0123456789") != std::string::npos) return false;
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
    size_t dot = s.find('.', pos);
    const std::string tok = (dot == std::string::npos) ? s.substr(pos) : s.substr(pos, dot - pos);
    if (!parseUint32(tok, parts[i])) return false;
    if (dot == std::string::npos) break;
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

std::string trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return {};
  const size_t b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}

}  // namespace

bool ModLoader::parseSdk(const std::string& sdk, ModInfo& out) const {
  const std::string s = trim(sdk);
  if (s.empty()) return false;
  if (s.find_first_not_of("0123456789") == std::string::npos) {
    // Bare integer: SDK major line, e.g. "sdk": "1". Fail-closed on absurd
    // values ("99999999999999999999" refuses the mod instead of crashing).
    uint32_t v = 0;
    if (!parseUint32(s, v)) return false;
    out.sdkVersion = v;
    return true;
  }
  Semver v;
  if (parseSemver(s, v)) {
    // Bare semver: "sdk": "1.2.0".
    out.sdkVersion = v.major;
    out.sdkMinor = v.minor;
    out.sdkPatch = v.patch;
    return true;
  }
  // Range: ">=1.0.0 <2.0.0". Use the lower bound as the declared version and
  // keep the original string for range enforcement in checkCompatibility().
  const size_t sp = s.find(' ');
  const std::string lo = s.substr(0, sp);
  const char* ops[] = {">=", "<=", ">", "<", "="};
  for (const char* op : ops) {
    const std::string prefix = op;
    if (lo.rfind(prefix, 0) == 0 && parseSemver(lo.substr(prefix.size()), v)) {
      out.sdkVersion = v.major;
      out.sdkMinor = v.minor;
      out.sdkPatch = v.patch;
      out.sdkRange = s;
      return true;
    }
  }
  return false;
}

uint32_t ModLoader::parsePermissions(const std::string& permList) {
  uint32_t bits = 0;
  std::stringstream ss(permList);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    tok = trim(tok);
    if (tok.empty()) continue;
    // Canonical name table (mods/mod_permissions.h): one source of truth for
    // manifest permission names, including "net" -> kPermNet (mod hardening
    // item 2's bit, which the old if/else chain could never parse).
    const uint32_t bit = mods::permissionBitForName(tok);
    if (bit) bits |= bit;
    else logError("mod.json: unknown permission '%s' (ignored)", tok.c_str());
  }
  return bits;
}

ModInfo ModLoader::parseModJson(const std::string& path) {
  ModInfo info;
  std::ifstream in(path, std::ios::binary);
  if (!in) { logError("mod.json: cannot open %s", path.c_str()); return info; }
  std::stringstream ss;
  ss << in.rdbuf();
  Json j;
  std::string err;
  if (!Json::parse(ss.str(), j, &err) || j.type != Json::Type::Object) {
    logError("mod.json: %s: %s", path.c_str(), err.empty() ? "not an object" : err.c_str());
    return info;
  }
  info.id = j.getStr("id");
  info.version = j.getStr("version");
  info.type = j.getStr("type", "content");
  if (info.id.empty()) { logError("mod.json: %s: missing id", path.c_str()); return info; }
  // Review F-M7: mod ids are validated, not just non-empty — the same
  // [a-z0-9_]{1,64} format ExtensionRegistry enforces for extension ids.
  // An unvalidated id is the key permission grants are stored under, so a
  // crafted id must never reach the store or the loader.
  if (!ExtensionRegistry::isValidId(info.id)) {
    logError("mod.json: %s: invalid mod id '%s' (must match [a-z0-9_]{1,64})", path.c_str(),
             info.id.c_str());
    info.id.clear();
    return info;
  }
  const Json* sdk = j.get("sdk");
  std::string sdkStr;
  if (sdk && sdk->type == Json::Type::String) sdkStr = sdk->str;
  else if (sdk && sdk->type == Json::Type::Number) {
    // Review F-L8: route the numeric form through a range check first —
    // static_cast<uint32_t> of an out-of-range double (1e30, negatives) is
    // UB. The file's own parseUint32-style discipline applies: the value
    // must be integral and inside uint32 range, else the mod is refused.
    const double v = sdk->num;
    if (v < 0.0 || v > 4294967295.0 || std::floor(v) != v) {
      logError("mod.json: %s: numeric sdk %g is out of range", path.c_str(), v);
      info.id.clear();
      return info;
    }
    sdkStr = std::to_string(static_cast<uint32_t>(v));
  }
  if (!parseSdk(sdkStr, info)) { logError("mod.json: %s: bad sdk '%s'", path.c_str(), sdkStr.c_str()); info.id.clear(); return info; }
  const Json* perms = j.get("permissions");
  std::string permList;
  if (perms && perms->type == Json::Type::Array) {
    for (const Json& p : perms->arr)
      if (p.type == Json::Type::String) { if (!permList.empty()) permList += ','; permList += p.str; }
  }
  info.permissions = parsePermissions(permList);
  // Permission manifest (item 7): permission-set version, per-permission
  // rationales, load-critical subset, first-party claim.
  info.permissionSetVersion =
      static_cast<uint32_t>(j.getNum("permissionSet", mods::kPermissionSetVersion));
  const Json* rats = j.get("permissionRationales");
  if (rats && rats->type == Json::Type::Object) {
    for (const auto& [name, v] : rats->obj) {
      if (v.type != Json::Type::String) continue;
      if (!mods::permissionBitForName(name)) {
        logError("mod.json: %s: rationale for unknown permission '%s' (ignored)",
                 path.c_str(), name.c_str());
        continue;
      }
      info.permissionRationales[name] = v.str;
    }
  }
  const Json* crit = j.get("criticalPermissions");
  std::string critList;
  if (crit && crit->type == Json::Type::Array) {
    for (const Json& p : crit->arr)
      if (p.type == Json::Type::String) { if (!critList.empty()) critList += ','; critList += p.str; }
  }
  const uint32_t critBits = parsePermissions(critList);
  if ((critBits & ~info.permissions) != 0)
    logError("mod.json: %s: criticalPermissions names permission(s) the mod did not request (dropped)",
             path.c_str());
  info.criticalPermissions = critBits & info.permissions;
  info.firstPartyClaim = j.getBool("firstParty", false);
  // IExtensible declarations: "extensible" + "extensions".
  info.extensible = j.getBool("extensible", false);
  const Json* exts = j.get("extensions");
  if (exts && exts->type == Json::Type::Array) {
    for (const Json& e : exts->arr) {
      if (e.type != Json::Type::Object) {
        logError("mod.json: %s: 'extensions' entry is not an object (skipped)", path.c_str());
        continue;
      }
      ModExtensionDecl decl;
      decl.id = e.getStr("id");
      decl.versionRange = e.getStr("version", "*");
      const double share = e.getNum("budgetShare", 0);
      if (!ExtensionRegistry::isValidId(decl.id)) {
        logError("mod.json: %s: bad extension id '%s' (entry skipped)", path.c_str(),
                 decl.id.c_str());
        continue;
      }
      if (share < 0 || share > 100) {
        logError("mod.json: %s: extension '%s' budgetShare %g out of 0-100 (clamped)",
                 path.c_str(), decl.id.c_str(), share);
      }
      decl.budgetShare = static_cast<uint32_t>(share < 0 ? 0 : (share > 100 ? 100 : share));
      info.extensions.push_back(std::move(decl));
    }
  }
  return info;
}

std::vector<ModInfo> ModLoader::scan(const std::string& dir) {
  namespace fs = std::filesystem;
  std::vector<ModInfo> mods;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) { logError("mods dir not found: %s", dir.c_str()); return mods; }
  std::vector<fs::path> folders;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_directory()) folders.push_back(e.path());
  std::sort(folders.begin(), folders.end());
  for (const auto& f : folders) {
    const fs::path json = f / "mod.json";
    if (!fs::is_regular_file(json, ec)) continue;  // not a mod folder
    ModInfo info = parseModJson(json.string());
    if (info.id.empty()) continue;  // invalid: already logged
    info.path = f.string();
    mods.push_back(std::move(info));
  }
  logInfo("mods: %zu mod(s) found in %s", mods.size(), dir.c_str());
  return mods;
}

bool ModLoader::checkCompatibility(const ModInfo& mod, std::string& reason) const {
  const Semver engine{kEngineSdkMajor, kEngineSdkMinor, kEngineSdkPatch};
  // Range form ("sdk": ">=1.0.0 <2.0.0"): enforce both bounds.
  if (!mod.sdkRange.empty()) {
    const size_t sp = mod.sdkRange.find(' ');
    const std::string lo = mod.sdkRange.substr(0, sp);
    const std::string hi = sp == std::string::npos ? "" : trim(mod.sdkRange.substr(sp + 1));
    auto bound = [](const std::string& b, Semver& v) {
      const char* ops[] = {">=", "<=", ">", "<", "="};
      for (const char* op : ops)
        if (b.rfind(op, 0) == 0 && parseSemver(b.substr(std::string(op).size()), v)) return std::string(op);
      return std::string();
    };
    Semver loV, hiV;
    const std::string loOp = bound(lo, loV), hiOp = bound(hi, hiV);
    if (!loOp.empty() && loOp != ">=" && loOp != ">" && loOp != "=") {
      reason = "mod '" + mod.id + "': unsupported sdk range '" + mod.sdkRange + "'";
      return false;
    }
    if (!loOp.empty()) {
      const int c = cmpSemver(engine, loV);
      const bool okLo = loOp == ">=" ? c >= 0 : loOp == ">" ? c > 0 : c == 0;
      if (!okLo) {
        reason = "mod '" + mod.id + "': SDK too new for mod (engine " +
                 std::to_string(engine.major) + "." + std::to_string(engine.minor) + "." +
                 std::to_string(engine.patch) + " < required " + mod.sdkRange + ")";
        return false;  // mod wants a NEWER sdk: backward-compat failure, refuse
      }
    }
    if (!hiOp.empty()) {
      const int c = cmpSemver(engine, hiV);
      const bool okHi = hiOp == "<" ? c < 0 : hiOp == "<=" ? c <= 0 : true;
      if (!okHi) {
        reason = "mod '" + mod.id + "': SDK major mismatch (engine " +
                 std::to_string(engine.major) + "." + std::to_string(engine.minor) + "." +
                 std::to_string(engine.patch) + " outside required " + mod.sdkRange + ")";
        return false;  // refuse on major mismatch
      }
    }
    if (engine.major == loV.major && cmpSemver(engine, loV) > 0) {
      reason = "mod '" + mod.id + "': built for older SDK minor (" + mod.sdkRange +
               "); continuing with a warning";
      return true;  // minor mismatch: warn-and-continue
    }
    reason.clear();
    return true;
  }
  // Bare major ("sdk": "1"): major must match; no minor information to warn on.
  if (mod.sdkVersion != kEngineSdkMajor) {
    reason = "mod '" + mod.id + "': SDK major mismatch (mod wants SDK " +
             std::to_string(mod.sdkVersion) + ", engine has SDK " + std::to_string(kEngineSdkMajor) + ")";
    return false;
  }
  // Bare semver ("sdk": "1.2.0"): warn-and-continue when the mod targets a
  // different minor on the same major line.
  if ((mod.sdkMinor != kEngineSdkMinor || mod.sdkPatch != kEngineSdkPatch) &&
      (mod.sdkMinor != 0 || mod.sdkPatch != 0)) {
    reason = "mod '" + mod.id + "': built for SDK " + std::to_string(mod.sdkVersion) + "." +
             std::to_string(mod.sdkMinor) + "." + std::to_string(mod.sdkPatch) + ", engine has SDK " +
             std::to_string(kEngineSdkMajor) + "." + std::to_string(kEngineSdkMinor) + "." +
             std::to_string(kEngineSdkPatch) + "; continuing with a warning";
    return true;
  }
  reason.clear();
  return true;
}

std::vector<ModInfo> ModLoader::load(const std::string& dir) {
  // Headless-safe default: deny-all prompt, grants stored next to the mods
  // dir. The game passes its own store/prompt/gate via the overload below.
  mods::PermissionStore store(dir + "/permission_grants.json");
  mods::ModPermissionGate gate;
  return load(dir, store, gate, mods::denyAllPermissions);
}

std::vector<ModInfo> ModLoader::load(const std::string& dir, mods::PermissionStore& store,
                                     mods::ModPermissionGate& gate,
                                     mods::PermissionPromptFn prompt) {
  std::vector<ModInfo> loaded;
  std::vector<std::string> seenIds;
  for (ModInfo mod : scan(dir)) {
    // Review F-M7: duplicate mod ids are rejected — first folder wins.
    // Two folders claiming one id would otherwise both load and both
    // inherit the permission grants stored under that id.
    if (std::find(seenIds.begin(), seenIds.end(), mod.id) != seenIds.end()) {
      logError("mods: refusing duplicate mod id '%s' (%s): an earlier folder already claimed it",
               mod.id.c_str(), mod.path.c_str());
      continue;
    }
    seenIds.push_back(mod.id);
    std::string reason;
    if (!checkCompatibility(mod, reason)) {
      logError("mods: refusing '%s': %s", mod.id.c_str(), reason.c_str());
      continue;
    }
    if (!reason.empty()) logError("mods: WARNING for '%s': %s", mod.id.c_str(), reason.c_str());
    // Permission approval BEFORE the mod's onLoad: an unapproved mod never
    // gets lifecycle callbacks with ungranted permissions. The effective
    // mask rides on ModInfo for the API binder to gate every binding on.
    const mods::PermissionResolution res = gate.resolve(mod, store, prompt);
    mod.grantedPermissions = res.granted;
    mod.loadAction = res.action;
    if (res.action == mods::ModLoadAction::kRefuse) {
      logError("mods: refusing '%s': %s", mod.id.c_str(), res.reason.c_str());
      continue;
    }
    loaded.push_back(std::move(mod));
  }
  store.save();
  logInfo("mods: %zu mod(s) loadable", loaded.size());
  return loaded;
}

}  // namespace litholattice
