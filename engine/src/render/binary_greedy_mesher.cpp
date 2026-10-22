// Binary greedy meshing: bitwise face detection + greedy rectangle merging.
//
// Three stages (research doc 1.3):
//  1. Occupancy masks: per axis, one 256-bit plane (4x uint64_t) per slice.
//     Emitters (opaque cubes, 16 slices) and cullers (any opaque voxel, 18 slices
//     with the 1-cell border ring) are tracked separately.
//  2. Bitwise hidden-face culling, word-wide: face+ = emit & ~cullNext,
//     face- = emit & ~cullPrev. No per-voxel branches.
//  3. Greedy rectangle merging per (axis, dir, slice): scan rows low to high,
//     expand each run along the row, then down the rows while the run bits match
//     (word-wide row compares) and every voxel's material equals the seed row's.
//     Fixed iteration order => bit-identical quad streams for identical input.
#include "litholattice/render/binary_greedy_mesher.h"
#include "litholattice/world/block.h"

#include <bit>  // std::countr_zero (MSVC has no __builtin_ctz)

namespace litholattice {
namespace {

constexpr int kN = 16;

// Plane coords (p, q) -> voxel for axis a (0=X, 1=Y, 2=Z), slice i.
inline void planeToVoxel(int a, int i, int p, int q, int& x, int& y, int& z) {
  if (a == 0) { x = i; y = p; z = q; }
  else if (a == 1) { x = p; y = i; z = q; }
  else { x = p; y = q; z = i; }
}

// Per-face visibility masks: [axis][dir 0=+ 1=-][slice][word].
struct FaceMasks {
  uint64_t face[3][2][kN][4];
};

void buildFaceMasks(const uint8_t* emit16, const uint8_t* cull18, FaceMasks& m) {
  uint64_t emitM[3][kN][4] = {};
  uint64_t cullM[3][kN + 2][4] = {};
  for (int x = 0; x < kN; ++x)
    for (int y = 0; y < kN; ++y)
      for (int z = 0; z < kN; ++z) {
        if (!emit16[(y * kN + z) * kN + x]) continue;
        emitM[0][x][(y + z * kN) >> 6] |= 1ULL << ((y + z * kN) & 63);
        emitM[1][y][(x + z * kN) >> 6] |= 1ULL << ((x + z * kN) & 63);
        emitM[2][z][(x + y * kN) >> 6] |= 1ULL << ((x + y * kN) & 63);
      }
  // Culler ring: slice s in -1..16 -> index s+1; only tangent coords in 0..15 can
  // ever hide an interior face (the neighbour shares the face's tangent coords).
  for (int x = 0; x < kN + 2; ++x)
    for (int y = 0; y < kN + 2; ++y)
      for (int z = 0; z < kN + 2; ++z) {
        if (!cull18[(y * (kN + 2) + z) * (kN + 2) + x]) continue;
        const int sx = x - 1, sy = y - 1, sz = z - 1;
        if (sy >= 0 && sy < kN && sz >= 0 && sz < kN) {
          const int bit = sy + sz * kN;
          cullM[0][sx + 1][bit >> 6] |= 1ULL << (bit & 63);
        }
        if (sx >= 0 && sx < kN && sz >= 0 && sz < kN) {
          const int bit = sx + sz * kN;
          cullM[1][sy + 1][bit >> 6] |= 1ULL << (bit & 63);
        }
        if (sx >= 0 && sx < kN && sy >= 0 && sy < kN) {
          const int bit = sx + sy * kN;
          cullM[2][sz + 1][bit >> 6] |= 1ULL << (bit & 63);
        }
      }
  // Stage 2: word-wide hidden-face culling.
  for (int a = 0; a < 3; ++a)
    for (int i = 0; i < kN; ++i)
      for (int w = 0; w < 4; ++w) {
        m.face[a][0][i][w] = emitM[a][i][w] & ~cullM[a][i + 2][w];  // +dir: neighbour at i+1
        m.face[a][1][i][w] = emitM[a][i][w] & ~cullM[a][i][w];      // -dir: neighbour at i-1
      }
}

inline uint8_t matOf(const uint8_t* material, int a, int i, int p, int q) {
  if (!material) return 0;
  int x, y, z;
  planeToVoxel(a, i, p, q, x, y, z);
  return material[(y * kN + z) * kN + x];
}

// Stage 3: greedy rectangle merge for one (axis, dir, slice) face mask.
void mergeSlice(int a, int d, int i, const uint64_t in[4], const uint8_t* material,
                std::vector<BinaryQuad>& out) {
  uint64_t work[4] = {in[0], in[1], in[2], in[3]};
  auto rowOf = [&](int q) -> uint32_t {
    return uint32_t(work[q >> 2] >> ((q & 3) * kN)) & 0xFFFFu;
  };
  for (int q = 0; q < kN; ++q) {
    uint32_t row = rowOf(q);
    while (row) {
      const int p0 = std::countr_zero(row);
      const uint8_t m0 = matOf(material, a, i, p0, q);
      int p1 = p0;  // expand along the row while bits are set and material matches
      while (p1 + 1 < kN && (row & (1u << (p1 + 1))) && matOf(material, a, i, p1 + 1, q) == m0) ++p1;
      const int w = p1 - p0 + 1;
      const uint32_t run = (w == kN) ? 0xFFFFu : ((1u << w) - 1u);
      int q1 = q;  // expand down while the whole run is set and materials match the seed row
      while (q1 + 1 < kN) {
        const uint32_t r2 = rowOf(q1 + 1);
        if (((r2 >> p0) & run) != run) break;
        bool same = true;
        for (int c = 0; c < w; ++c)
          if (matOf(material, a, i, p0 + c, q1 + 1) != matOf(material, a, i, p0 + c, q)) { same = false; break; }
        if (!same) break;
        ++q1;
      }
      const int h = q1 - q + 1;
      BinaryQuad quad{};
      quad.dir = uint8_t(2 * a + d);
      quad.material = m0;
      quad.w = uint8_t(w);
      quad.h = uint8_t(h);
      const int plane = (d == 0) ? i + 1 : i;  // the face sits on the voxel's outer side
      if (a == 0) { quad.x = uint8_t(plane); quad.y = uint8_t(p0); quad.z = uint8_t(q); }
      else if (a == 1) { quad.x = uint8_t(p0); quad.y = uint8_t(plane); quad.z = uint8_t(q); }
      else { quad.x = uint8_t(p0); quad.y = uint8_t(q); quad.z = uint8_t(plane); }
      out.push_back(quad);
      for (int qq = q; qq <= q1; ++qq) work[qq >> 2] &= ~((uint64_t(run) << p0) << ((qq & 3) * kN));
      row = rowOf(q);
    }
  }
}


// Clear face bits shared by two voxels of the same transparent-flagged block id
// (glass-against-glass, mirroring the classic mesher's rule). Runs on the FaceMasks
// between buildFaceMasks and mergeSlice; the bit iteration is trivial next to the
// merge itself. Only faces whose emitter carries kTransparent are candidates, so
// non-flagged translucents (e.g. leaves) keep their internal faces like the classic path.
void cullSameTransparentFaces(const MeshInput& in, const BlockRegistry& reg, FaceMasks& m) {
  for (int a = 0; a < 3; ++a)
    for (int d = 0; d < 2; ++d)
      for (int i = 0; i < kN; ++i)
        for (int w = 0; w < 4; ++w) {
          uint64_t word = m.face[a][d][i][w];
          while (word) {
            const int b = std::countr_zero(word);
            word &= word - 1;
            const int bit = (w << 6) + b;
            int x, y, z;
            planeToVoxel(a, i, bit & 15, bit >> 4, x, y, z);
            const BlockId e = in.id[((y + 1) * (kN + 2) + (z + 1)) * (kN + 2) + (x + 1)];
            if (!(reg.def(e).flags & kTransparent)) continue;
            int nx = x + 1, ny = y + 1, nz = z + 1;
            if (a == 0) nx += (d == 0 ? 1 : -1);
            else if (a == 1) ny += (d == 0 ? 1 : -1);
            else nz += (d == 0 ? 1 : -1);
            if (in.id[(ny * (kN + 2) + nz) * (kN + 2) + nx] == e)
              m.face[a][d][i][w] &= ~(1ULL << b);
          }
        }
}

size_t binaryMeshCore(const uint8_t* emit16, const uint8_t* cull18, const uint8_t* material,
                      std::vector<BinaryQuad>& out) {
  FaceMasks m;
  buildFaceMasks(emit16, cull18, m);
  const size_t start = out.size();
  for (int a = 0; a < 3; ++a)
    for (int d = 0; d < 2; ++d)
      for (int i = 0; i < kN; ++i) mergeSlice(a, d, i, m.face[a][d][i], material, out);
  return out.size() - start;
}

}  // namespace

size_t binaryMeshChunk16(const uint8_t* solid, const uint8_t* material, std::vector<BinaryQuad>& out) {
  uint8_t cull18[(kN + 2) * (kN + 2) * (kN + 2)] = {};  // outside the region reads as air
  for (int x = 0; x < kN; ++x)
    for (int y = 0; y < kN; ++y)
      for (int z = 0; z < kN; ++z)
        cull18[((y + 1) * (kN + 2) + (z + 1)) * (kN + 2) + (x + 1)] = solid[(y * kN + z) * kN + x] ? 1 : 0;
  return binaryMeshCore(solid, cull18, material, out);
}

size_t binaryMeshSubChunk(const MeshInput& in, const MeshContext& ctx, const uint8_t* material,
                          std::vector<BinaryQuad>& out) {
  const BlockRegistry& reg = ctx.blocks;
  uint8_t emit16[kN * kN * kN];
  uint8_t cull18[(kN + 2) * (kN + 2) * (kN + 2)];
  for (int x = 0; x < kN + 2; ++x)
    for (int y = 0; y < kN + 2; ++y)
      for (int z = 0; z < kN + 2; ++z) {
        const BlockId b = in.id[(y * (kN + 2) + z) * (kN + 2) + x];
        const uint8_t opaque = reg.has(b, kOpaque) ? 1 : 0;
        cull18[(y * (kN + 2) + z) * (kN + 2) + x] = opaque;
        if (x > 0 && x < kN + 1 && y > 0 && y < kN + 1 && z > 0 && z < kN + 1)
          emit16[((y - 1) * kN + (z - 1)) * kN + (x - 1)] =
              (opaque && reg.def(b).shape == Shape::Cube) ? 1 : 0;
      }
  return binaryMeshCore(emit16, cull18, material, out);
}

// Transparent pass (fix 2026-10-11, audit HIGH): the opaque binaryMeshSubChunk emits
// opaque cubes only, so glass/water/etc. produced zero quads on the BinaryQuad/PBR path
// and PBR transparency tiers had no geometry. This pass emits non-opaque cubes through
// the same greedy core; faces between two voxels of the same transparent-flagged id are
// culled (glass-against-glass, like the classic mesher). Appends to `out` — the caller
// may merge into the opaque stream (the PBR shader resolves alpha per material, correct
// for the screen-door and refraction tiers) or keep a dedicated stream. NOTE: true
// alpha-blend tiers need back-to-front sorting of the transparent quads, not yet done.
size_t binaryMeshSubChunkTransparent(const MeshInput& in, const MeshContext& ctx, const uint8_t* material,
                                     std::vector<BinaryQuad>& out) {
  const BlockRegistry& reg = ctx.blocks;
  uint8_t emit16[kN * kN * kN];
  uint8_t cull18[(kN + 2) * (kN + 2) * (kN + 2)];
  for (int x = 0; x < kN + 2; ++x)
    for (int y = 0; y < kN + 2; ++y)
      for (int z = 0; z < kN + 2; ++z) {
        const BlockId b = in.id[(y * (kN + 2) + z) * (kN + 2) + x];
        const uint8_t opaque = reg.has(b, kOpaque) ? 1 : 0;
        cull18[(y * (kN + 2) + z) * (kN + 2) + x] = opaque;
        if (x > 0 && x < kN + 1 && y > 0 && y < kN + 1 && z > 0 && z < kN + 1)
          emit16[((y - 1) * kN + (z - 1)) * kN + (x - 1)] =
              (!opaque && reg.def(b).shape == Shape::Cube) ? 1 : 0;
      }
  FaceMasks m;
  buildFaceMasks(emit16, cull18, m);
  cullSameTransparentFaces(in, reg, m);
  const size_t start = out.size();
  for (int a = 0; a < 3; ++a)
    for (int d = 0; d < 2; ++d)
      for (int i = 0; i < kN; ++i) mergeSlice(a, d, i, m.face[a][d][i], material, out);
  return out.size() - start;
}

}  // namespace litholattice
