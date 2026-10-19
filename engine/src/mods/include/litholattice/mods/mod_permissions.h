// Permission manifest schema + install/enable approval flow (mod hardening
// item 7, 2026-10-11).
//
// mod.json declares a permission manifest:
//   "permissionSet": 1,                          // which permission-set version
//                                                // this manifest targets
//   "permissions": ["blocks", "world_read", "net"],   // requested permissions
//   "permissionRationales": {                    // per-permission WHY strings
//     "blocks":     "Adds 40 new building blocks",
//     "world_read": "Scans nearby ore for the vein HUD",
//     "net":        "Syncs waypoints via the engine-gated channel only"
//   },
//   "criticalPermissions": ["world_write"],      // load-critical: a denial of
//                                                // one of these REFUSES the load
//   "firstParty": false                          // CLAIM only: the engine grants
//                                                // first-party treatment only
//                                                // when the mod id is on its
//                                                // trusted first-party list
//
// SCHEMA RULES (fail-closed):
// - Unknown permission names in "permissions" are ignored and logged (they
//   can never become bits); the mod still loads without them.
// - A permission without a rationale parses fine; the prompt shows
//   "(no rationale given)" so thin manifests are visible, not fatal.
// - "criticalPermissions" names an explicit subset of "permissions". A denied
//   critical permission refuses the load (logged reason); every other denied
//   permission is STRIPPED (the mod loads with a reduced mask, logged).
// - "firstParty": true is a claim, not a grant. The engine treats a mod as
//   first-party only when its id is on the gate's trusted first-party list
//   (game-configured via ModPermissionGate::setFirstPartyIds). A claim with
//   no engine confirmation is logged and the mod is treated as third-party.
//   No silent privilege inheritance: a first-party parent never makes its
//   extensions first-party (that rule lives with the IExtensible worker).
// - The runtime C facet's isFirstParty() remains the authority for native
//   mods at runtime; the manifest path covers install-time resolution for
//   content/script mods and the UI prompt.
//
// APPROVAL FLOW (UI-agnostic): ModPermissionGate::resolve() collects the
// requested permissions + rationales into a PermissionRequest and calls the
// game's PermissionPromptFn, which returns the approved bit subset. The game
// presents PermissionRequest however it likes (install dialog, settings
// screen); the headless default (denyAllPermissions) approves NOTHING, so a
// headless engine never loads a mod with unapproved permissions.
//
// HEADLESS-SAFE DEFAULT: DENY unless approved. Unapproved permissions are
// stripped (logged) and the mod loads reduced, UNLESS the mod declared them
// load-critical, in which case the load is refused. ModLoader::load() runs
// the check before the mod's onLoad: an unapproved mod never gets lifecycle
// callbacks with ungranted permissions — ModInfo::grantedPermissions carries
// the effective mask and the (builder-side) API binder must gate each
// binding on it.
//
// PERSISTENCE: PermissionStore records per-mod grants (granted/denied masks
// plus the requested snapshot, mod version, and permission-set version) and
// persists them as JSON (atomic write). Re-prompts happen only when the
// manifest ASKS FOR MORE: new bits vs. the stored request snapshot (an update
// diff), or a permission-set version change. An explicit denial persists
// across updates OF THE SAME VERSION — the user is not re-prompted for a
// bit they already denied; only genuinely NEW bits trigger the prompt.
//
// GRANT BINDING (review F-M7): a stored grant is honored only for the exact
// (mod id, mod version) it was recorded under — resolve() treats a record
// whose version differs from the manifest's as absent, so grants and
// denials are re-decided on every version change and a new mod reusing a
// previously approved id inherits nothing. The store already carried the
// version per record, so this binding needed no file-format change. A
// signer hash is NOT bound: neither ModInfo nor the store carries one
// today; adding one is a format extension for when mod signing lands.
// (First-party grants are separate: they rest on the gate's engine-
// configured trusted-id list, not on this store.)
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "litholattice/mods/mod_interface.h"  // kPerm* bits

namespace litholattice {
/// Declared in mod_loader.h (the gate's resolve() input). Forward-declared
/// here so this header does not include the loader.
struct ModInfo;
namespace mods {

/// Current permission-set version. The manifest declares which set it
/// targets ("permissionSet"); the engine re-reviews grants when this moves.
/// v2 adds kPermPlayerControl (MCP player piloting).
inline constexpr uint32_t kPermissionSetVersion = 2;

/// All permission bits the manifest may name.
inline constexpr uint32_t kAllPermissionBits =
    kPermBlocks | kPermWorldRead | kPermWorldWrite | kPermTick | kPermEvents |
    kPermUI | kPermInput | kPermAssets | kPermNet | kPermConsole |
    kPermPlayerControl | kPermPilotSenses;

/// Permissions a content mod gets without a prompt (pure data: registering
/// blocks/items and loading assets; the file sandbox bounds the reads).
/// Anything else a content mod requests goes through the prompt path —
/// headless default denies it, same as for script/native mods.
inline constexpr uint32_t kContentSafePermissions = kPermBlocks | kPermAssets;

/// Canonical manifest name -> permission bit. Returns 0 for unknown names
/// (unknown names never become bits; the caller logs and ignores them).
uint32_t permissionBitForName(std::string_view name);
/// Permission bit -> canonical manifest name. Returns nullptr for bits the
/// manifest cannot name.
const char* permissionNameForBit(uint32_t bit);

/// One permission the prompt must decide on.
struct PermissionRequestItem {
  uint32_t bit = 0;        ///< the kPerm* bit
  std::string name;        ///< canonical manifest name ("net", ...)
  std::string rationale;   ///< author's WHY ("" = "(no rationale given)")
  bool previouslyDenied = false;  ///< was denied before (context for the UI)
};

/// UI-agnostic install/enable prompt payload. The game renders this however
/// it wants (dialog, settings screen); the headless default denies all.
struct PermissionRequest {
  std::string modId;
  std::string modVersion;
  std::string modType;  ///< "content" | "script" | "native"
  uint32_t permissionSetVersion = 0;  ///< declared by the manifest
  bool firstPartyClaim = false;       ///< manifest "firstParty" value
  uint32_t requested = 0;             ///< full requested mask
  uint32_t alreadyGranted = 0;        ///< still-requested bits approved earlier
  std::vector<PermissionRequestItem> items;  ///< bits needing a decision
};

/// The decision-maker: returns the approved subset of request.items bits.
/// Must be a pure function of the request for testability; the game wires
/// its UI dialog here. Bits outside the request are masked off by resolve().
using PermissionPromptFn = std::function<uint32_t(const PermissionRequest&)>;

/// Headless-safe default prompt: approves nothing (DENY unless approved).
uint32_t denyAllPermissions(const PermissionRequest&);

/// What the gate decided for one mod's load.
enum class ModLoadAction : uint8_t {
  kLoad = 0,        ///< all requested permissions granted (or none requested)
  kLoadStripped,    ///< denied permissions stripped; the mod runs reduced
  kRefuse,          ///< a load-critical permission was denied: do not load
};

/// The gate's verdict for one mod.
struct PermissionResolution {
  ModLoadAction action = ModLoadAction::kLoad;
  uint32_t granted = 0;  ///< effective mask the mod may run with
  uint32_t stripped = 0;  ///< requested & ~granted (logged when non-empty)
  uint32_t denied = 0;    ///< explicitly denied bits (subset of stripped)
  std::string reason;     ///< human-readable, always logged
};

/// One persisted grant record per mod.
struct StoredGrant {
  std::string modVersion;             ///< manifest version at decision time
  uint32_t permissionSetVersion = 0;  ///< permission set at decision time
  uint32_t requested = 0;  ///< what the manifest asked for (the diff base)
  uint32_t granted = 0;    ///< what the user approved
  uint32_t denied = 0;     ///< what the user denied (persists across updates)
};

/// Persistent grant storage: <path> holds one JSON object mapping mod id to
/// its StoredGrant (names, not raw bits, for readability and forward
/// compatibility). Writes are atomic (temp + fsync + rename). "" path =
/// in-memory only (no persistence).
class PermissionStore {
 public:
  explicit PermissionStore(std::string path = {});
  /// Loads the store from disk. A missing file is fine (fresh store);
  /// a corrupt file is logged and treated as empty (never fatal).
  bool load();
  /// Persists the store (no-op true when the path is empty or nothing
  /// changed since the last save/load).
  bool save();

  const StoredGrant* lookup(const std::string& modId) const;
  void record(const std::string& modId, StoredGrant grant);
  size_t count() const { return grants_.size(); }
 private:
  std::string path_;
  std::map<std::string, StoredGrant> grants_;
  bool dirty_ = false;
};

/// Install/enable-time permission gate. resolve() is the only entry point
/// the loader needs; the game configures the trusted first-party list.
class ModPermissionGate {
 public:
  /// Mod ids the engine trusts as first-party (e.g. {"lithokraft"}).
  /// A manifest "firstParty": true claim only takes effect for ids on
  /// this list; everything else is treated as third-party (logged).
  void setFirstPartyIds(std::vector<std::string> ids) { firstPartyIds_ = std::move(ids); }
  bool isTrustedFirstParty(const std::string& modId) const;

  /// Runs the approval flow for one mod:
  ///   1. first-party verification (trusted claim -> grant all, logged);
  ///   2. store lookup + diff: prompt only for NEW bits (update re-prompt)
  ///      or for not-yet-approved bits when the permission set changed;
  ///   3. critical check: a denied load-critical bit -> kRefuse;
  ///      otherwise denied bits are stripped -> kLoadStripped (logged).
  /// A null prompt behaves like denyAllPermissions (headless-safe).
  PermissionResolution resolve(const ModInfo& mod, PermissionStore& store,
                               PermissionPromptFn prompt);

 private:
  std::vector<std::string> firstPartyIds_;
};

}  // namespace mods
}  // namespace litholattice
