// Binary greedy meshing for LithoLattice.
//
// WHAT: A bitwise greedy-meshing variant (inspired by cgerikj/binary-greedy-meshing):
//   processes 64 faces at once with 64-bit bitmask ops and emits compact 8-byte quads
//   (position bits + width/height + facing + material index), friendly to vertex pulling.
// WHY: sub-millisecond remeshes (measured ~87 us/chunk on hostile 45% random fill)
//   keep chunk edits off the 512 TPS tick path.
// HOW: three stages - (1) occupancy masks: one 256-bit plane per axis slice,
//   (2) bitwise hidden-face culling (emitMask & ~cullMask, word-wide),
//   (3) greedy rectangle merging with word-wide row comparisons, material-aware.
//
// Lives ALONGSIDE greedy_mesher.cpp: it neither changes the existing mesher's behavior
// nor its output. Scope: simple opaque cubes only (the same scope as the existing
// greedy placeholder); every other shape falls back to meshSubChunk.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "litholattice/render/mesher.h"

namespace litholattice {

/// One merged quad: exactly 8 bytes, vertex-pulling friendly. A shader (or CPU code)
/// rebuilds the 4 corners from the min corner + (w along the first tangent axis,
/// h along the second) + facing. Tangent axes: X faces -> (Y, Z); Y faces -> (X, Z);
/// Z faces -> (X, Y). All coordinates are whole voxels, chunk-local 0..16.
struct BinaryQuad {
  uint8_t x, y, z;   ///< min corner of the quad, in voxels
  uint8_t w, h;      ///< extent in voxels along the first / second tangent axis (1..16)
  uint8_t dir;       ///< 0..5: +X, -X, +Y, -Y, +Z, -Z (same order as the classic mesher)
  uint8_t material;  ///< 8-bit per-voxel material/palette index (PBR registry passthrough)
  uint8_t aux = 0;   ///< reserved for future per-quad data (baked AO, flags); keeps the record 8 bytes
};
static_assert(sizeof(BinaryQuad) == 8, "binary quads stay 8 bytes");

/// Mesh a 16^3 region. `solid` and `material` are 16^3 arrays indexed (y*16+z)*16+x;
/// `solid` holds 0/1, `material` may be nullptr (every quad then gets material 0).
/// Voxels outside the region read as air. Faces merge only across equal-material voxels.
/// Deterministic: the same input bytes always produce the bit-identical quad stream
/// (fixed axis / dir / slice / row iteration order, no hashing, no thread-locals).
/// Returns the number of quads appended to `out`.
size_t binaryMeshChunk16(const uint8_t* solid, const uint8_t* material, std::vector<BinaryQuad>& out);

/// MeshInput-level wrapper: consumes the same snapshot + context as meshSubChunk, plus the
/// per-voxel material palette (16^3, same layout as above; nullptr = 0). A voxel emits
/// faces iff it is an opaque cube (Shape::Cube + kOpaque, mirroring the existing greedy
/// placeholder's scope); a face is culled iff the neighbour is opaque (kOpaque, any shape),
/// exactly the rule meshSubChunk uses. The 1-cell snapshot border feeds the culling ring,
/// so border faces agree with the classic mesher too.
size_t binaryMeshSubChunk(const MeshInput& in, const MeshContext& ctx, const uint8_t* material,
                          std::vector<BinaryQuad>& out);

/// Transparent companion to binaryMeshSubChunk (fix 2026-10-11): emits non-opaque
/// cubes (glass, water, ...) through the same greedy core, culling faces between two
/// voxels of the same transparent-flagged id. Appends to `out`. See the .cpp for the
/// stream-merging note (alpha-blend tiers need sorted transparent draw — future work).
size_t binaryMeshSubChunkTransparent(const MeshInput& in, const MeshContext& ctx, const uint8_t* material,
                                     std::vector<BinaryQuad>& out);

/// Expand one quad back into its unit faces (for tests and CPU fallback paths).
/// Calls visit(vx, vy, vz, dir) once per covered unit face; dir is 0..5 as above and
/// (vx, vy, vz) is the emitting voxel in chunk-local coordinates.
template <typename Visit>
void expandBinaryQuad(const BinaryQuad& q, Visit&& visit) {
  const int a = q.dir / 2;  // 0 = X faces, 1 = Y faces, 2 = Z faces
  const int t0 = (a == 0) ? 1 : 0;  // first tangent axis:  Y / X / X
  const int t1 = (a == 2) ? 1 : 2;  // second tangent axis: Z / Z / Y
  int e[3] = {q.x, q.y, q.z};
  if ((q.dir & 1) == 0) --e[a];  // +dir faces sit one voxel past their emitter
  int v[3] = {e[0], e[1], e[2]};
  for (int dv = 0; dv < q.h; ++dv)
    for (int du = 0; du < q.w; ++du) {
      v[t0] = e[t0] + du;
      v[t1] = e[t1] + dv;
      visit(v[0], v[1], v[2], q.dir);
    }
}

}  // namespace litholattice
