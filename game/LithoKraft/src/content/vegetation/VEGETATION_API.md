# Vegetation API — `lithokraft::vegetation`

Game-side ProcGen vegetation (workstream 3 of 3, JD-authorized 2026-10-11).
Wraps the engine generators (workstreams 1+2:
`engine/src/procgen/include/litholattice/procgen/trees.h`,
`shrubs.h`), binds the engine's caller-supplied material slots to the **game
palette** (`lithokraft::model::authored::palette` — ids are stable, never
renumber), and returns ready-to-save `lithokraft::model::Model` values.

## Functions

```cpp
namespace lithokraft::vegetation {

// Tree: engine preset params -> game palette -> Model.
model::Model generateTreeModel(const procgen::TreeParams& p,
                               std::string name, std::string author = "");

// Shrub: engine preset params -> game palette -> Model. `crossOut` receives
// the 2D bundle when p.style == PlantStyle::CrossedQuads (may be null).
struct ShrubCrossInfo {
  float widthBlocks, heightBlocks;  // footprint in blocks
  uint32_t textureIndex;            // into the procedural texture set
  const char* textureName;          // plantTextureName(textureIndex)
  uint8_t tintR, tintG, tintB;      // per-plant tint
};
model::Model generateShrubModel(const procgen::ShrubParams& p,
                                std::string name, std::string author = "",
                                ShrubCrossInfo* crossOut = nullptr);

// Palette binding (applied automatically inside the generate functions):
void bindTreePalette(procgen::TreeParams& p);    // in place
void bindShrubPalette(procgen::ShrubParams& p);  // in place

}  // namespace lithokraft::vegetation
```

(`procgen` = `litholattice::procgen`.)

## Parameters

- `p` — engine params, normally from a family preset:
  `procgen::clockwoodWirewood(seed)`, `procgen::dopantCrystalTree(seed)`,
  `procgen::traceForestTree(seed)`; `procgen::presetBerryShrub()`,
  `procgen::presetTwistedShrub()`, `procgen::presetMeadowTuft()`,
  `procgen::presetDryTuft()`. A **copy** is bound and generated; the caller's
  params are never modified.
- `name` / `author` — the Model's display name and author credit.
- `crossOut` — optional out-param for the 2D render bundle (see below).

## The style flag (`PlantStyle`)

`ShrubParams::style` selects the render path:

- `Modeled3D` — `generateShrubOctree()`: a noisy voxel shrub mound.
  `crossOut` is left untouched.
- `CrossedQuads` — `generateShrubCross()`: a Minecraft-style cross sprite.
  `crossOut` receives `{widthBlocks, heightBlocks, textureIndex,
  textureName, tintR/G/B}` — the bundle the game's block appearance feeds
  the `Shape::Cross` path (register `BlockDef::tex =
  plantTextureName(i)`; see "2D RENDER BINDING" in `shrubs.h`). The Model
  geometry additionally holds a thin crossed-plane voxel stand-in (two
  1-cell-thick vertical planes forming an X, leaf material, stem core) so
  the `.lthm` stays meaningful as a 3D fallback; the **authoritative** 2D
  representation is the `ShrubCrossInfo` bundle, not the voxels.

The procedural texture set: `procgen::numPlantTextures()` (3:
`procgen_grass_tuft`, `procgen_fern`, `procgen_dry_scrub`),
`procgen::plantTextureName(i)`, `procgen::plantTexturePixels(i)` → 16×16
RGBA8, row 0 = top, alpha 0 = cutout. Deterministic, process-lifetime
pointers, `nullptr` out of range.

## Palette binding

The engine presets leave **placeholder slot ids** in the material fields
("Caller remap" comments in `trees.cpp` / `shrubs.cpp`). The binding:

| Engine preset (slots) | Game palette |
|---|---|
| `clockwoodWirewood` (trunk=3) | trunk → `kCopper` (4) |
| `dopantCrystalTree` (trunk=4, leaf=5, tip=6) | → `kCrystalBlue` (11), `kCrystalAmber` (12), `kCrystalCore` (13) |
| `traceForestTree` (trunk=7, leaf=9, tip=8) | trunk → `kCopper` (4), leaf → `kTraceBlue` (6), tip → `kTraceRed` (5) — two-colour red/blue tips |
| `presetBerryShrub` (stem=3, leaf=4) | stem → `kHandleWood` (1), leaf → `kTraceRed` (5) |
| `presetTwistedShrub` (stem=3, leaf=5) | stem → `kCopper` (4), leaf → `kShellDark` (7) |
| `presetMeadowTuft` / `presetDryTuft` (stem=1, leaf=2) | stem → `kHandleWood` (1), leaf → `kShellDark` (7); colour comes from the 2D tint |

Params that did **not** come from a family preset (unrecognized slot
pattern) keep their caller-supplied ids untouched — hand-rolled params
should use real palette ids (or 0), never the placeholder slots 3–9,
which are reserved for preset-pattern matching. If the engine ever changes
a preset's placeholder slots, `vegetation_selftest`'s palette checks fail
loudly by design.

## Determinism contract

Same `(params, seed)` → same plant, **always**: identical leaf stream
(same `subdiv::hashLeaves`, same `leaves()` order/content), and after
`saveModel`, a byte-identical `.lthm` file. No wall-clock, no thread
identity, no hash-map iteration anywhere in this path. Verified by
`vegetation_selftest`: re-author ⇒ `modelEquals` ⇒ byte-identical files,
matching the checked-in `assets/models/*.lthm`.

## Usage example

```cpp
#include "vegetation.h"
namespace veg = lithokraft::vegetation;

// 1. Pick a family preset at a fixed seed.
auto tp = litholattice::procgen::clockwoodWirewood(0xC10C700Dull);
// 2. Generate: engine core runs, game palette binds, Model comes back.
lithokraft::model::Model tree = veg::generateTreeModel(tp, "Wirewood", "me");
// 3. Save it.
std::string err;
lithokraft::model::saveModel("assets/models/wirewood.lthm", tree, &err);
// 4. A 2D grass tuft, with its render bundle:
veg::ShrubCrossInfo cross;
auto sp = litholattice::procgen::presetMeadowTuft();
lithokraft::model::Model tuft = veg::generateShrubModel(sp, "Tuft", "me", &cross);
// cross.textureName -> register as BlockDef::tex; cross.tintR/G/B -> tint.
```

## Engine-API notes (seam deviations absorbed)

The engine API as landed (workstreams 1+2) differs from the workstream
brief's sketch in these details; the game side absorbs all of them:

- Seeds are `uint64_t`, not `uint32_t` (`clockwoodWirewood(uint64_t)` etc.).
- The third tree family is **`traceForestTree`** (TraceForest
  circuit-tree, copper trunk, red/blue two-colour glowing tips) — the
  procgen sibling of the hand-authored `trace_tree.lthm`.
- `CrossQuadPlant` is `{float widthBlocks, heightBlocks; uint32_t
  textureIndex; uint8_t tintR/G/B}` (not int blocks / float tint).
- The texture set is 3 RGBA8 sprites accessed via `numPlantTextures()` /
  `plantTextureName(i)` / `plantTexturePixels(i)` (not a struct accessor).
- Shrub presets carry fixed seeds (`presetBerryShrub()` etc. take no
  arguments); tree presets take the seed.
- Engine placeholder material slots are real small ints (1–9) documented
  as "caller palette" — hence the pattern-matching binding above.
