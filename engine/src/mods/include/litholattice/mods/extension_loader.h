// ExtensionLoader (worker 2): recursive extension loading for IExtensible
// parents, scoped extension contexts, capability scoping, privilege
// default-deny, and cascade unload.
//
// Terminology (JD-approved, 2026-10-11): "mod" = top-level IMod package;
// "extension" = sub-mod hosted inside an IExtensible parent.
//
// What this file owns:
//   1. Recursive loading: for each loaded mod, query the IExtensible facet
//      (LithoLatticeExtensibleQuery); for each declared extension, resolve it
//      (v1 static linking: match by registered mod id), register it in the
//      ExtensionRegistry, and recurse. The depth cap is enforced during
//      recursion (fail-closed: over-cap levels are refused and the partial
//      load is rolled back). Cycles — a mod declaring itself or an ancestor
//      as its extension — are rejected with a clear error.
//   2. Scoped contexts: every loaded extension gets an ExtensionContext that
//      carries ONLY the parent's filtered childApi. The extension never sees
//      the full LithoLatticeModApi, and sibling extensions are isolated: a
//      context exposes no handle or path to siblings, only the parent.
//   3. Privilege default-deny: an extension is third-party unless its parent
//      explicitly grants first-party status through the facet's
//      firstPartyGrants tail (see extensible_interface.h). A parent's
//      isFirstParty()==1 NEVER propagates; grants are not transitive.
//   4. Cascade unload: unloading a parent unloads its extensions first, in
//      reverse depth order (children before parent).
//
// Save-data contract on unload (with mod_save_data):
//   - Extension save sections are keyed by dotted identity path
//     ("mod.parent.child"), so identity survives unload/reload.
//   - Unload NEVER deletes save data: the section stays on disk. Reloading the
//     same identity restores it (subject to ModSaveRegistry version-compat
//     rules: quarantine + disable on corruption or incompatible versions).
//   - If the extension never comes back, its section becomes Orphaned: kept
//     on disk, never deleted — the same policy as orphaned mod sections.
//   - Rationale: the engine must never destroy user data as a side effect of
//     unloading; fail-closed on the data's side.
//
// Seams for worker 3:
//   - ExtensionContext::budgetShare records the parent's reserved per-tick
//     budget percent for the extension; wire it into the tick scheduler for
//     budget enforcement (0 = no reserved share, best-effort in the parent).
//   - ExtensionLoader::registerStaticModule takes the IMod lifecycle so the
//     unload path can invoke onUnload; per-extension crash isolation (fault
//     domains around extension ticks) hooks in at the same place.
//   - ExtensionContext::childApi is the capability-scoping seam: future API
//     surface growth goes through the parent's childApi, never the full API.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "litholattice/mods/extensible_interface.h"
#include "litholattice/mods/extension_registry.h"
#include "litholattice/mods/mod_interface.h"

namespace litholattice {

/// The engine's scoped view of one loaded extension. This is ALL an extension
/// may observe about its host: the parent's filtered childApi, its own
/// identity, and its budget share. There is deliberately:
///   - NO pointer to the full LithoLatticeModApi (capability scoping: the
///     extension can only call what the parent chose to expose),
///   - NO handle or path to sibling extensions (siblings are isolated; an
///     extension addresses only its parent, via childApi, and itself, via
///     its own IMod lifecycle),
///   - NO inherited privilege (see isFirstParty).
///
/// AUDIT: the field table in mods_selftest.cpp (testExtensionContextIsolation)
/// pins this struct's shape — childApi is the last member, so no full-API
/// pointer can hide anywhere in the context.
struct ExtensionContext {
  std::string path;        ///< dotted identity, e.g. "lithokraft.worldedit.brushes"
  std::string parentPath;  ///< dotted path of the IExtensible parent
  uint32_t depth = 0;      ///< nesting depth (top-level mod = 0, first child = 1)
  /// Effective first-party flag. DEFAULT-DENY: false unless the parent
  /// explicitly named this extension id in the facet's firstPartyGrants.
  /// A first-party parent does NOT make its extensions first-party.
  bool isFirstParty = false;
  /// Percent (0-100) of the parent's per-tick budget reserved for this
  /// extension, from the decl. 0 = no reserved share (best-effort).
  /// Worker 3 seam: budget enforcement in the tick scheduler.
  uint32_t budgetShare = 0;
  /// The parent's exposed API surface — the ONLY engine/parent surface this
  /// extension may call. Never the full LithoLatticeModApi.
  const LithoLatticeExtensionApi* childApi = nullptr;
};

class ExtensionLoader {
 public:
  /// One statically-linked module (v1; the DLL era resolves these via dlsym).
  struct StaticModule {
    StaticModule() = default;
    StaticModule(std::string id_, std::string version_,
                 LithoLatticeQueryExtensibleFn query,
                 const LithoLatticeMod* imod_ = nullptr,
                 bool firstParty = false)
        : id(std::move(id_)),
          version(std::move(version_)),
          extensibleQuery(query),
          imod(imod_),
          isFirstParty(firstParty) {}

    std::string id;        ///< mod id, [a-z0-9_]{1,64}
    std::string version;   ///< semver, e.g. "1.0.0" (checked against decl ranges)
    /// IExtensible query export; null = ordinary mod, never queried as a parent.
    LithoLatticeQueryExtensibleFn extensibleQuery = nullptr;
    /// IMod lifecycle for onUnload during cascade unload; may be null.
    const LithoLatticeMod* imod = nullptr;
    /// The TOP-LEVEL mod's own isFirstParty flag. Never inherited by
    /// extensions (default-deny; explicit facet grant only).
    bool isFirstParty = false;
  };

  /// Registers a statically-linked module so the loader can resolve it by id.
  /// Duplicate ids are refused (fail-closed; first registration wins).
  void registerStaticModule(StaticModule module);

  /// Loads a top-level mod and its whole extension subtree, recursively.
  /// Idempotent: loading an already-loaded mod is a no-op success.
  /// Returns false with err set on: unknown id, invalid facet, extension
  /// cycle, depth-cap refusal, version-range mismatch, unresolvable extension
  /// id, or invalid decl. Fail-closed: a failed load rolls back every node it
  /// registered, leaving the registry as it was.
  bool loadMod(const std::string& id, std::string& err);

  /// Cascade unload: every extension under id, deepest-first (children before
  /// parent), then id itself. Invokes onUnload where an IMod lifecycle was
  /// registered. Save data is NOT deleted (see the header contract above).
  /// No-op when id is unknown.
  void unloadMod(const std::string& id);

  /// Scoped context for a loaded extension path; null for unknown paths and
  /// for top-level mods (they get no extension context).
  const ExtensionContext* contextFor(const std::string& path) const;

  const ExtensionRegistry& registry() const { return registry_; }
  ExtensionRegistry& registry() { return registry_; }

  /// Load order (dotted paths, parent before children) of the most recent
  /// loadMod() call. Test seam.
  const std::vector<std::string>& lastLoadOrder() const { return lastLoadOrder_; }
  /// Unload order (dotted paths, children before parent) of the most recent
  /// unloadMod() call. Test seam.
  const std::vector<std::string>& lastUnloadOrder() const { return lastUnloadOrder_; }

 private:
  bool validateFacet(const LithoLatticeExtensible* facet, std::string& err) const;
  // Recursive step: resolve one decl under parentPath. declIndex is the
  // decl's position in the parent facet's array (for the share plan).
  // ancestors holds the ids on the current recursion path (for cycle
  // detection); added collects every dotted path registered during this
  // loadMod (for rollback).
  bool loadExtension(const std::string& parentPath,
                     const LithoLatticeExtensionDecl& decl, uint32_t declIndex,
                     const LithoLatticeExtensible* parentFacet,
                     std::vector<std::string>& ancestors,
                     std::vector<std::string>& added, std::string& err);
  void collectSubtree(const std::string& path, std::vector<std::string>& out) const;
  // Effective (warn-and-clamped) share for a decl: the share plan computed
  // from the parent facet at load time, falling back to the raw decl.
  uint32_t effectiveShare(const std::string& parentPath, uint32_t declIndex,
                          uint32_t declared) const;

  ExtensionRegistry registry_;
  std::map<std::string, StaticModule> staticModules_;  // by mod id
  std::map<std::string, ExtensionContext> contexts_;   // by dotted path
  std::set<std::string> loadedMods_;                   // top-level ids
  // Per-load share plan: parentPath -> effective budgetShare per decl index
  // (ExtensionBudget::clampShares; warn-and-clamp when siblings sum > 100).
  // Populated during loadMod(), cleared when it returns (success or rollback).
  std::map<std::string, std::vector<uint32_t>> sharePlan_;
  std::vector<std::string> lastLoadOrder_;
  std::vector<std::string> lastUnloadOrder_;
};

}  // namespace litholattice
