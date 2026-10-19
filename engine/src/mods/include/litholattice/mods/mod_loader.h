// IMod loader (v1 initial): finds mod.json, validates, loads.
//
// STATUS — STAGED / UNWIRED (F-L12, review 2026-10-13): no game code loads
// mods through this ModLoader; it is exercised by selftests only. The
// CANONICAL mod-manifest system is engine/src/assets/mod_manifest.*
// (schema-1 JSON, permissions as {api, reason}, signature/trust model) —
// the one the game actually uses for content packs and the boot seal /
// hot-swap decision. This loader's schema ("sdk" + permission names ->
// kPerm bits) is a DIFFERENT vocabulary kept for the future script/native
// loading work; do not wire new features to it. If script/native loading
// is wired, it migrates onto the canonical manifest first.
// Scans a mods/ directory for folders containing mod.json. Each mod.json has:
//   { "id": "mymod", "version": "1.0.0", "sdk": "1",
//     "type": "content|script|native", "permissions": ["blocks", "world_read"],
//     "permissionSet": 1,
//     "permissionRationales": {"blocks": "Adds 40 new building blocks"},
//     "criticalPermissions": ["world_write"],
//     "firstParty": false,
//     "extensible": true,
//     "extensions": [{"id": "brushes", "version": ">=1.0.0", "budgetShare": 25}] }
// "sdk" may also be a semver range, e.g. ">=1.0.0 <2.0.0".
// "permissions" names come from the permission manifest schema (see
// mods/mod_permissions.h); per-permission rationale strings explain WHY each
// is needed and are shown to the user at install. "criticalPermissions"
// marks load-critical bits: a denied critical bit REFUSES the load, any
// other denied bit is STRIPPED (headless-safe default: DENY unless approved).
// "firstParty" is a claim only — the gate grants first-party treatment only
// when the mod id is on the engine's trusted first-party list.
// "extensible"/"extensions" declare IExtensible hosting (worker 2 parses them
// into ModInfo; the recursive loader still keys off the C facet for v1).
// v1: static linking (mods are compiled in). DLL loading comes later.
//
// SDK enforcement (JD's call, 2026-10-11): the load path refuses mods whose
// declared SDK major version does not match the engine's (they fail gracefully
// with a logged reason); a minor-version mismatch only warns and continues.
// checkCompatibility() implements the check; load() enforces it.
//
// Permission approval (item 7): load() runs ModPermissionGate::resolve()
// for every mod BEFORE the mod's onLoad, so an unapproved mod never gets its
// lifecycle callbacks with ungranted permissions. The plain load(dir)
// overload uses the headless-safe default (deny-all prompt, grants stored
// next to the mods dir); the game passes its own store/prompt/gate via the
// four-argument overload.
#pragma once
#include <map>
#include <string>
#include <vector>
#include "litholattice/mods/mod_interface.h"
#include "litholattice/mods/mod_permissions.h"

namespace litholattice {

/// One "extensions" entry from mod.json: a child slot the parent declares.
/// Merged with the C facet at load time (DLL era); for v1 static linking the
/// facet alone is authoritative.
struct ModExtensionDecl {
  std::string id;                  ///< extension id, [a-z0-9_]{1,64}
  std::string versionRange = "*";  ///< semver range the parent accepts
  uint32_t budgetShare = 0;        ///< 0-100: percent of the parent's budget
};

struct ModInfo {
  std::string id;
  std::string version;
  uint32_t sdkVersion = 0;  // declared SDK major version (LITHOLATTICE_SDK_VERSION line)
  uint32_t sdkMinor = 0;
  uint32_t sdkPatch = 0;
  std::string sdkRange;  // original "sdk" string when it is a range ("" otherwise)
  std::string type;      // "content", "script", "native"
  uint32_t permissions = 0;  // LlModPermission bitmask
  // Permission manifest (item 7; see mods/mod_permissions.h for the schema).
  uint32_t permissionSetVersion = mods::kPermissionSetVersion;  // "permissionSet"
  std::map<std::string, std::string> permissionRationales;  // "permissionRationales": name -> WHY
  uint32_t criticalPermissions = 0;  // "criticalPermissions": denied -> load REFUSED
  bool firstPartyClaim = false;      // "firstParty": claim only, engine verifies
  // Approval-flow output, filled by load() before the mod's onLoad.
  uint32_t grantedPermissions = 0;  // effective mask after approval
  mods::ModLoadAction loadAction = mods::ModLoadAction::kLoad;
  std::string path;  // folder containing mod.json
  bool extensible = false;  // "extensible": true — hosts sub-mods (extensions)
  std::vector<ModExtensionDecl> extensions;  // "extensions": declared child slots
};

class ModLoader {
 public:
  /// Scans dir for mods (each in its own folder with mod.json).
  /// Returns the valid mods; invalid ones are logged and skipped.
  /// NOTE: scan() does NOT enforce SDK compatibility; load() does.
  std::vector<ModInfo> scan(const std::string& dir);

  /// Full load path: scan() + SDK enforcement. Mods with an incompatible SDK
  /// major version are refused (logged, skipped); a minor mismatch logs a
  /// warning and the mod still loads. Returns only the loadable mods.
  ///
  /// Permission approval runs for every mod BEFORE its onLoad: the
  /// headless-safe default (deny-all prompt; grants stored next to the mods
  /// dir) applies here. A mod whose load-critical permissions are denied is
  /// refused; any other unapproved permission is stripped (logged) and the
  /// mod loads reduced, with the effective mask in
  /// ModInfo::grantedPermissions. The API binder must gate every binding on
  /// that mask — an unapproved mod never gets lifecycle callbacks with
  /// ungranted permissions.
  std::vector<ModInfo> load(const std::string& dir);

  /// load() with an explicit approval flow: the game's PermissionStore
  /// (e.g. in the profile dir), ModPermissionGate (first-party list), and
  /// PermissionPromptFn (the install/enable UI dialog; null = deny-all).
  /// Same semantics as load(dir) otherwise.
  std::vector<ModInfo> load(const std::string& dir, mods::PermissionStore& store,
                            mods::ModPermissionGate& gate, mods::PermissionPromptFn prompt);

  /// Validates a mod.json's permissions against its type.
  /// Content mods need no prompt; script/native do.
  bool needsPermissionPrompt(const ModInfo& mod) const {
    return mod.type == "script" || mod.type == "native";
  }

  /// Checks if a mod's SDK declaration is compatible with the running engine.
  /// mod.json "sdk": "1" (major only) or ">=1.0.0 <2.0.0" (range).
  /// Returns false with a reason on MAJOR mismatch (refuse); returns true on a
  /// MINOR mismatch but sets reason to a warning (warn-and-continue).
  /// Forward compat: old mods (smaller struct sizes) work on new engines.
  /// Backward compat: new mods on old engines fail gracefully with a clear message.
  bool checkCompatibility(const ModInfo& mod, std::string& reason) const;

 private:
  ModInfo parseModJson(const std::string& path);
  uint32_t parsePermissions(const std::string& permList);
  bool parseSdk(const std::string& sdk, ModInfo& out) const;
};

}  // namespace litholattice
