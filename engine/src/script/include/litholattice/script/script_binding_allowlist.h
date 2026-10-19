// Script binding allowlist: the definitive registry of AngelScript APIs a mod's
// scripts may receive. The script host (ScriptEngine) refuses to bind anything
// not on this list, and refuses bindings the mod's permission bitmask does not
// grant (see LithoLatticeModPermission in litholattice/mods/mod_interface.h).
//
// THREAT MODEL
// A malicious or buggy mod's scripts must never reach outside the sandbox:
// no raw file I/O, no network, no process/system calls, no wall-clock, no
// unseeded RNG. The allowlist is the enforcement point for that:
//  1. Deny-by-default: ScriptEngine::bindGlobalFunction() only forwards to
//     AngelScript when the exact declaration string is on the allowlist AND
//     the mod's granted permissions cover the entry's required bits.
//  2. No dangerous bindings exist in the list. Registration of new entries
//     goes through registerBinding(), which rejects declarations containing
//     known-dangerous tokens (file, socket, system, exec, ...) as a
//     defense-in-depth heuristic; the primary control is code review of every
//     entry added here or at runtime by the game.
//  3. The raw asIScriptEngine* is NOT exposed (ScriptEngine::raw() was
//     removed); the game registers its API through the gated bind call, so
//     there is no bypass path around the allowlist.
// Entries are reviewed against mod_safety.h: deterministic engine APIs only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace litholattice {

// One bindable API: the AngelScript declaration plus the permission bits a mod
// must hold (0 = always allowed, e.g. log and version queries).
struct ScriptBindingEntry {
  const char* decl;              // exact AngelScript declaration string
  uint32_t requiredPermissions;  // LithoLatticeModPermission bitmask
  const char* description;       // human-readable; used by docs/tests
};

class ScriptBindingAllowlist {
 public:
  /// Registers the built-in table (core + standard game/mod API). Idempotent;
  /// called by ScriptEngine::init() so scripts always have a complete list.
  static void registerBuiltins();

  /// Registers one game-specific binding. Refused (false, logged) when:
  ///  - the declaration is already registered, or
  ///  - the declaration contains a forbidden token (file/network/process/
  ///    system access heuristic, defense in depth; the real control is that
  ///    every entry is reviewed before it is added).
  static bool registerBinding(const ScriptBindingEntry& entry);

  /// True when decl is on the allowlist (exact declaration-string match).
  static bool isAllowlisted(const std::string& decl);
  /// Required permission bits for decl; 0 when not allowlisted.
  static uint32_t requiredPermissions(const std::string& decl);
  /// Full table (for documentation and tests).
  static const std::vector<ScriptBindingEntry>& entries();

 private:
  ScriptBindingAllowlist() = delete;
};

}  // namespace litholattice
