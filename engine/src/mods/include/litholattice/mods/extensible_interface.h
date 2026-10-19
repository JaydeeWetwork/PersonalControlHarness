// IExtensible v1: the C-ABI facet by which a mod declares itself an extensible
// parent (a host for sub-mods, called "extensions").
// Same rules as mod_interface.h: no STL, exceptions, or RTTI across the
// boundary; opaque handles, versioned structs (size first), plain C types.
// Static linking for v1, designed as if DLLs are coming (Half-Life-SDK style).
//
// Terminology (JD-approved, 2026-10-11): "mod" = top-level IMod package;
// "extension" = sub-mod hosted inside an IExtensible parent.
//
// mod.json schema addition (IExtensible):
//   {
//     "id": "worldedit", "version": "1.0.0",
//     "extensible": true,
//     "extensions": [
//       {"id": "brushes", "version": ">=1.0.0", "budgetShare": 25},
//       {"id": "masks",   "version": "^1.2.0",  "budgetShare": 10}
//     ]
//   }
// - "extensible": boolean. A mod that hosts sub-mods sets true. Absent/false =
//   ordinary mod; the engine never queries it for the facet.
// - "extensions": the child slots the parent declares. Each entry:
//     "id":          extension id, [a-z0-9_]{1,64} (same rules as mod ids)
//     "version":     semver range the parent accepts, same syntax as "deps"
//                    ("*", "=x.y.z", ">=x.y.z", "^x.y.z")
//     "budgetShare": 0-100, percent of the parent's per-tick budget reserved
//                    for that extension. Siblings' shares should sum to <= 100;
//                    0 = no reserved share (best-effort within the parent).
// The engine merges these declarations with the facet below: mod.json is the
// load-time (DLL era) source of truth; the facet is the run-time source. For
// v1 static linking the facet alone is authoritative.
// (Worker 2: ModLoader::parseModJson parses "extensible"/"extensions" into
// ModInfo so the data is available; the recursive loader still keys off the
// facet for v1.)
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// One child slot the parent declares it can host.
typedef struct LithoLatticeExtensionDecl {
  uint32_t size;           ///< sizeof(this struct) — engine checks version
  const char* id;          ///< extension id, [a-z0-9_]{1,64}
  const char* versionRange;///< semver range the parent accepts, e.g. ">=1.0.0"
  uint32_t budgetShare;    ///< 0-100: percent of the parent's budget reserved
} LithoLatticeExtensionDecl;

// The parent's exposed API surface for its extensions (children). The engine
// hands this to each loaded extension; the extension calls in, the parent
// never calls out except through the child's own IMod lifecycle.
// v1 is deliberately small: logging + version queries. Worker 2/3 extend this
// (budgets, isolation hooks) without breaking the size-first layout.
typedef struct LithoLatticeExtensionApi {
  uint32_t size;  ///< sizeof(this struct)
  uint32_t sdkVersion;

  // Logging (always allowed).
  void (*log)(const char* msg);

  // Versioning: extensions query this to check compatibility at runtime.
  // Returns e.g. 0x00010000 for 0.1.0 (major << 16 | minor << 8 | patch).
  uint32_t (*engineVersion)(void);
  uint32_t (*sdkVersionFn)(void);
} LithoLatticeExtensionApi;

// The extensible facet itself: everything the engine needs to know about a
// parent mod's extension hosting.
typedef struct LithoLatticeExtensible {
  uint32_t size;  ///< sizeof(this struct) — engine checks version
  uint32_t extensionCount;
  const LithoLatticeExtensionDecl* extensions;  ///< array of extensionCount decls
  const LithoLatticeExtensionApi* childApi;    ///< parent's API surface for children

  // --- v1.1 (worker 2): first-party privilege grants. Optional tail — read
  // ONLY when size >= offsetof(LithoLatticeExtensible, firstPartyGrantCount)
  // + sizeof(uint32_t). A v1.0 facet (smaller size) grants nothing.
  //
  // Privilege rule (JD default, 2026-10-11): extensions are THIRD-PARTY by
  // default. A parent's isFirstParty()==1 NEVER propagates to its children.
  // The parent may elevate specific extensions by naming their ids here; any
  // extension NOT named stays third-party. The grant is explicit and
  // auditable: no inheritance, no wildcards, no transitive grants (a granted
  // extension's own children are still third-party unless granted in turn by
  // their own parent).
  uint32_t firstPartyGrantCount;
  const char** firstPartyGrants;  ///< ids of extensions granted first-party status
} LithoLatticeExtensible;

// Query mechanism (v1 static linking): the mod exports a function with this
// signature returning its facet, or exports nothing at all. The engine holds
// the exported function pointer (dlsym in the DLL era); a null pointer means
// "not extensible" and the engine must not treat the mod as a parent.
typedef const LithoLatticeExtensible* (*LithoLatticeQueryExtensibleFn)(void);

// Symbol name the DLL-era loader resolves: dlsym(handle,
// LITHOLATTICE_EXTENSIBLE_QUERY_SYMBOL). Absent symbol = not extensible.
#define LITHOLATTICE_EXTENSIBLE_QUERY_SYMBOL "LithoLattice_QueryExtensible"

// Resolves the facet from a possibly-null query pointer. Null in -> null out:
// this is the whole "is this mod extensible?" check.
static inline const LithoLatticeExtensible* LithoLatticeExtensibleQuery(
    LithoLatticeQueryExtensibleFn query) {
  return query ? query() : 0;
}

// First-party grant check (v1.1 tail). Returns 1 when the facet explicitly
// names id in firstPartyGrants, 0 otherwise. Size-gated: a v1.0 facet (too
// small to carry the tail) grants nothing — default deny, never inherit.
static inline int LithoLatticeExtensibleGrantsFirstParty(
    const LithoLatticeExtensible* facet, const char* id) {
  if (!facet || !id) return 0;
  if (facet->size <
      (uint32_t)(offsetof(LithoLatticeExtensible, firstPartyGrantCount) +
                 sizeof(uint32_t)))
    return 0;  // pre-grant facet: no grants possible
  for (uint32_t i = 0; i < facet->firstPartyGrantCount; ++i) {
    if (facet->firstPartyGrants && facet->firstPartyGrants[i] &&
        strcmp(facet->firstPartyGrants[i], id) == 0)
      return 1;
  }
  return 0;
}

#ifdef __cplusplus
}
#endif
