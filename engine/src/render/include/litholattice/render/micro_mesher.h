// Micro-face meshing for subdivided blocks (workstream 2, 2026-10-11).
//
// WHAT: the renderer learns micro-faces. A 1m block subdivided via
//   litholattice::subdiv::SparseOctree (up to 64^3 micro-cells, render/detail
//   level only - the sim still ticks whole blocks) meshes here into merged
//   quads with fractional positions, at a distance-selected LOD.
// WHY: subdivision is the modeling system (armory, silicon trees, mobs, the
//   ChipOR viewport). The chunk mesher emits whole-block cubes; subdivided
//   blocks need their micro-surface instead, with LOD so far blocks stay cheap.
// HOW: rasterize the tree's leaf stream into a dense 64^3 grid, majority-vote
//   downsample to the LOD grid (2^lod)^3, then greedy-merge exposed faces.
//   Deterministic: fixed iteration orders, no hashing, no thread-locals - the
//   same leaves always produce bit-identical quads.
//
// This header deliberately does NOT touch BinaryQuad (8-byte layout frozen) or
// the existing meshers' behavior: micro-geometry is a separate quad stream the
// chunk build appends alongside the normal mesh.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "litholattice/subdiv/octree.h"
#include "litholattice/subdiv/subdiv_store.h"

namespace litholattice::micro {

/// One merged micro-face quad. Positions are in world units (block origin baked
/// in); the subdivided block spans [origin, origin+1] on each axis.
/// Deliberately NOT BinaryQuad: BinaryQuad carries whole-voxel 0..16 coords in a
/// frozen 8-byte layout, while micro-faces need fractional positions.
/// Tangent-axis convention matches BinaryQuad: X faces -> (Y, Z); Y faces ->
/// (X, Z); Z faces -> (X, Y); w runs along the first tangent axis, h along the
/// second. A CPU shader (or the ChipOR viewport) rebuilds the 4 corners from
/// min corner + w/h + dir exactly like BinaryQuad.
struct MicroQuad {
  float x, y, z;     ///< min corner of the quad, world units
  float w, h;        ///< extent in world units along the first / second tangent axis
  uint8_t dir;       ///< 0..5: +X, -X, +Y, -Y, +Z, -Z (same order as BinaryQuad)
  uint8_t material;  ///< micro-cell material index (mesher passthrough, from MicroCell)
  uint8_t lod;       ///< LOD level this quad was emitted at (0..6)
  uint8_t reserved = 0;
};
static_assert(sizeof(MicroQuad) == 24, "micro quads stay 24 bytes");

/// What the micro-mesher reads besides the octree. All read-only, worker-safe.
struct MicroMeshParams {
  float originX = 0.0f, originY = 0.0f, originZ = 0.0f;  ///< world coords of the block's min corner
  /// Boundary-face culling, indexed by dir (0..5 as above): 1 = the abutting
  /// neighbour block is opaque, so micro-faces on that block side are hidden
  /// and skipped. 0 (default) = neighbour is air / unknown: boundary faces emit.
  /// The chunk build should fill this from its block snapshot (see
  /// appendChunkMicroGeometry's contract below).
  uint8_t neighborSolid[6] = {};
};

// ---------------------------------------------------------------------------
// LOD policy (distance-based). lodLevel 0 = coarsest (the block meshes as one
// cube), 6 = full 64^3 micro-detail. Level L meshes a (2^L)^3 grid per block.
//
// Thresholds are powers of two (JD's motif), in metres from the camera to the
// block centre, for 1m blocks:
//
//   distance <    4m -> LOD 6 (full detail; 64^3 cells are ~1.6cm - visible only up close)
//   distance <    8m -> LOD 5
//   distance <   16m -> LOD 4
//   distance <   32m -> LOD 3
//   distance <   64m -> LOD 2
//   distance <  128m -> LOD 1
//   otherwise        -> LOD 0 (single cube; a subdivided block is one quad per
//                             face past 128m, exactly like a plain block)
//
// Rationale: each LOD step halves the linear micro-resolution (8x fewer cells)
// per doubling of distance, so a block's screen-space micro-detail stays
// roughly constant. Coarsening uses a majority vote per LOD cell, so solid
// regions stay solid instead of dissolving into noise.
// ---------------------------------------------------------------------------

/// Distance-based LOD selection with the default policy above. Negative/NaN
/// distances clamp to 0 (closest). Always returns 0..6.
int selectLOD(float cameraDistance);

/// Mesh one subdivided block from its leaf stream (the documented
/// SparseOctree::leaves() output: non-overlapping, deterministic order).
/// lodLevel is clamped to [0, 6]. Appends to `out`; returns quads appended.
/// Empty leaf stream -> 0 quads, `out` untouched. Bit-identical output for
/// identical inputs (fixed iteration order; positions are exact int/S ratios).
size_t meshMicroLeaves(const std::vector<subdiv::MicroCell>& leaves, int lodLevel,
                       const MicroMeshParams& params, std::vector<MicroQuad>& out);

/// Convenience wrapper: mesh a whole SparseOctree at the given LOD.
/// Empty tree -> 0 quads, `out` untouched.
size_t meshOctreeLOD(const subdiv::SparseOctree& tree, int lodLevel,
                     const MicroMeshParams& params, std::vector<MicroQuad>& out);

// ---------------------------------------------------------------------------
// Chunk-build integration point.
//
// SubdivStore is workstream 1's chunk-scoped block -> octree map
// (subdiv/subdiv_store.h): chunk-local block coords 0..15, entries sorted
// deterministically. The concrete overload below is the signature sibling
// workstreams (ChipOR viewport, first models) should call; the template
// underneath keeps the same logic usable against store doubles in tests.
//
// For every subdivided block in the 16^3 region, appends its micro-geometry at
// the per-block LOD selected from the camera position. Blocks WITHOUT
// subdivision are never touched: with an empty store (blockCount() == 0) this
// returns 0 without reading anything, so plain chunks see zero behavior change.
//
// TWO-SIDED CONTRACT for the chunk build:
//  1. Mask subdivided blocks as AIR when building the NORMAL chunk mesh, so no
//     plain cube is emitted for them (else double-draw / z-fighting). Neighbour
//     blocks then emit faces toward the subdivided block; those faces are
//     exactly covered where micro-cells fill the boundary, and correctly
//     visible through carved-out micro-cells - no holes either way.
//  2. Pass neighbour opacity via `neighborSolid` (read from the same snapshot)
//     so boundary micro-faces hidden inside solid neighbours are culled.
//
// FOV-CULLING COMPOSITION: every emitted quad lies strictly inside its block's
// [origin, origin+1]^3 AABB (asserted by self-test), hence inside the chunk's
// AABB. Subdivision never widens chunk bounds, so the existing chunk-level
// frustum test (aabbInFrustumMargin with render.frustum_margin_deg) covers
// micro-geometry unchanged: LOD selection and chunk culling are orthogonal -
// micro-detail NEVER disables chunk-level culling.
// ---------------------------------------------------------------------------
size_t appendChunkMicroGeometry(const subdiv::SubdivStore& store, int regionMinX, int regionMinY,
                                int regionMinZ, float camX, float camY, float camZ,
                                std::vector<MicroQuad>& out,
                                std::function<void(int bx, int by, int bz, uint8_t solid[6])>
                                    neighborSolid = nullptr);

/// Generic form of the integration point: any store-like providing
///   size_t blockCount() const;
///   const subdiv::SparseOctree* getBlockOctree(int x, int y, int z) const;
/// (chunk-local 0..15, nullptr = no subdivision) gets the same treatment.
template <typename SubdivStore>
size_t appendChunkMicroGeometryT(const SubdivStore& store, int regionMinX, int regionMinY,
                                 int regionMinZ, float camX, float camY, float camZ,
                                 std::vector<MicroQuad>& out,
                                 std::function<void(int bx, int by, int bz, uint8_t solid[6])>
                                     neighborSolid = nullptr) {
  if (store.blockCount() == 0) return 0;  // plain chunks: zero behavior change, zero cost
  const size_t n0 = out.size();
  for (int bz = 0; bz < 16; ++bz)
    for (int by = 0; by < 16; ++by)
      for (int bx = 0; bx < 16; ++bx) {
        const subdiv::SparseOctree* tree = store.getBlockOctree(bx, by, bz);
        if (!tree || tree->empty()) continue;  // no subdivision -> untouched
        const float cx = float(regionMinX + bx) + 0.5f;
        const float cy = float(regionMinY + by) + 0.5f;
        const float cz = float(regionMinZ + bz) + 0.5f;
        const float dx = cx - camX, dy = cy - camY, dz = cz - camZ;
        const int lod = selectLOD(std::sqrt(dx * dx + dy * dy + dz * dz));
        MicroMeshParams p;
        p.originX = float(regionMinX + bx);
        p.originY = float(regionMinY + by);
        p.originZ = float(regionMinZ + bz);
        if (neighborSolid) neighborSolid(bx, by, bz, p.neighborSolid);
        meshOctreeLOD(*tree, lod, p, out);
      }
  return out.size() - n0;
}

}  // namespace litholattice::micro
