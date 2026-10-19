// ChipOR viewport (workstream 4, JD-locked 2026-10-11).
//
// WHAT: the ChipOR viewport block has a transparent top; peer inside and see
//   your construction miniaturized at full detail - YOUR circuit, not a
//   generic texture - rendered via the sub-block subdivision mesher. The
//   micro-wiring doesn't fire and there's no fancy lighting yet (maybe
//   later); it's a static representation. The trophy case for the lithography
//   loop: build big -> print small -> hold the proof.
// WHY: the viewport is how a printed chip shows its contents in-world.
// HOW: the viewport is its own block id (kChipORViewport = 52; the plain
//   solid ChipOR is kChipOR = 51), Shape::Custom so the classic mesher routes
//   it to LithoKraftAppearance::meshCustom, which draws the opaque body
//   (5 faces, neighbours cull against its kOpaque flag), the transparent top
//   (kShadeGlass - the existing transparent shading path, same mode the glass
//   blocks use) and the miniaturized model inside the top half.
//
// CONTRACTS / SEAMS:
//   - Subdivision types come from
//     engine/src/subdiv/include/litholattice/subdiv/octree.h (workstream 1,
//     implemented in engine/src/subdiv/octree.cpp, linked via
//     litholattice_subdiv). The demo model is built with the real
//     SparseOctree::set and read back with the real leaves().
//   - loadModelCells is the workstream 3 seam: it reads real .lthm files with
//     lithokraft::model::loadModel (see src/content/model/FORMAT.md). The
//     fixed default model (kDefaultModel) falls back to the built-in demo
//     when the file is absent, so the viewport always shows something;
//     any other missing/corrupt/unsanitary name is a clean empty.
//   - meshMiniatureLOD is the workstream 2 seam: it mirrors the documented
//     intent of subdiv::meshOctreeLOD (LOD mesh of an octree -> renderable
//     quads). The reference implementation below is replaced by the real
//     meshOctreeLOD on workstream 2's landing (same input contract: leaf
//     cells; same output: renderable quads). It lives in the lithokraft
//     namespace (not litholattice::subdiv) so the landing can't collide.
//
// MODEL NAME (documented choice): the viewport shows one fixed default model
//   for now (kDefaultModel). Per-block model names need block-entity data
//   plumbing that doesn't exist in the tree yet; when it does, the appearance
//   reads the name from the block entity and passes it to meshViewportCell.
#pragma once
#include <cstdint>
#include <string_view>
#include <vector>
#include "litholattice/subdiv/octree.h"
#include "litholattice/render/block_appearance.h"

namespace lithokraft::chipor {

/// Model space: workstream 1's 64^3 micro-space (kMaxCells per axis).
inline constexpr int kModelCells = litholattice::subdiv::kMaxCells;

/// Viewport interior in 1/16-block units, relative to the cell corner. The
/// miniature lives in the block's top half, under the transparent top face,
/// inset one unit from the walls so the body reads as a frame.
inline constexpr int kInteriorLo[3] = {1, 8, 1};
inline constexpr int kInteriorHi[3] = {15, 15, 15};

/// Default model every viewport shows (assets/models/chipor-demo.lthm;
/// built-in demo circuit when the file is absent, e.g. a shipping build
/// without the source tree).
inline constexpr std::string_view kDefaultModel = "chipor-demo";

/// Model loader (workstream 3 seam). Returns the model's leaf cells as
/// subdiv::MicroCell (the octree header's serialization primitive):
///   - kDefaultModel: assets/models/chipor-demo.lthm when readable, else the
///     built-in demo circuit (the viewport always shows something);
///   - any other name: assets/models/<name>.lthm via model::loadModel;
///     missing/corrupt/unsanitary names are a clean empty (missing model =>
///     empty viewport, no crash).
/// Results are cached (mutex-guarded); thread-safe for mesh workers.
/// Deterministic for a fixed models directory.
std::vector<litholattice::subdiv::MicroCell> loadModelCells(std::string_view name);

/// Built-in demo model: a tiny circuit (silicon die plate, red + blue trace
/// runs, a torch post, a via pillar) authored with the real SparseOctree.
/// Micro-materials: 1 = red trace, 2 = blue trace, 3 = silicon, 4 = post.
/// This is also what generates assets/models/chipor-demo.lthm.
std::vector<litholattice::subdiv::MicroCell> demoModelCells();

/// One miniature quad in 1/16-block units, relative to the cell corner.
struct MiniQuad {
  uint8_t face = 0;      ///< 0..5 (+X,-X,+Y,-Y,+Z,-Z, same order as MeshWriter::face)
  uint8_t material = 0;  ///< micro-material index (mesher passthrough)
  uint8_t lo[3] = {};    ///< inclusive min corner, 1/16-block units
  uint8_t hi[3] = {};    ///< exclusive max corner, 1/16-block units
};

/// Miniature mesher (workstream 2 seam): LOD mesh of the leaf cells into the
/// interior box - hidden faces culled, flat shaded, deterministic. The
/// reference implementation rasterizes to the interior grid and emits unit
/// quads (no greedy merging; workstream 2's meshOctreeLOD will produce the
/// merged LOD quads). A leaf smaller than one interior unit still covers at
/// least one unit (ceil on the hi side), so no detail silently vanishes.
std::vector<MiniQuad> meshMiniatureLOD(const std::vector<litholattice::subdiv::MicroCell>& cells);

/// Meshes one ChipOR viewport cell: opaque body (5 faces, culled against opaque
/// neighbours), the transparent top face (kShadeGlass), then the miniature
/// interior for `modelName` (skipped, like the top face, when the cell above
/// is opaque - nothing is visible then). Render-only: no sim state is read or
/// written. Thread-safe (read-only inputs; the writer is the caller's).
void meshViewportCell(const litholattice::CustomBlock& cell, litholattice::MeshWriter& out,
                      std::string_view modelName = kDefaultModel);

}  // namespace lithokraft::chipor
