// Micro-face meshing for subdivided blocks (workstream 2, 2026-10-11). See
// micro_mesher.h for the contract, the LOD policy, and the chunk-build
// integration notes.
#include "litholattice/render/micro_mesher.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "litholattice/subdiv/subdiv_store.h"

namespace litholattice::micro {
namespace {

/// Micro-cells per axis at full detail (SparseOctree kMaxLevel == 6).
constexpr int kFull = 64;

/// Rasterize the leaf stream into a dense 64^3 material grid (0 = empty).
/// Leaves are non-overlapping per the octree contract; on overlap the later
/// leaf in stream order wins (deterministic either way).
void rasterizeLeaves(const std::vector<subdiv::MicroCell>& leaves, uint8_t* dense) {
  for (const subdiv::MicroCell& c : leaves) {
    if (c.material == 0 || c.level > subdiv::kMaxLevel) continue;
    const int w = 1 << (subdiv::kMaxLevel - c.level);  // cells per axis covered
    int x0 = c.x * w, y0 = c.y * w, z0 = c.z * w;
    // Defensive clamp; the contract guarantees ranges already.
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (z0 < 0) z0 = 0;
    const int x1 = std::min(x0 + w, kFull), y1 = std::min(y0 + w, kFull),
              z1 = std::min(z0 + w, kFull);
    for (int z = z0; z < z1; ++z)
      for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) dense[(z * kFull + y) * kFull + x] = c.material;
  }
}

/// Majority-vote downsample of the 64^3 grid to S^3 (S < 64). Each coarse cell
/// takes the most common material in its region; ties go to the smallest
/// material index (deterministic). Empty (0) participates: a mostly-empty
/// region stays empty.
void downsampleMajority(const uint8_t* dense, int S, uint8_t* coarse) {
  const int w = kFull / S;
  for (int z = 0; z < S; ++z)
    for (int y = 0; y < S; ++y)
      for (int x = 0; x < S; ++x) {
        int counts[256] = {};
        for (int dz = 0; dz < w; ++dz)
          for (int dy = 0; dy < w; ++dy)
            for (int dx = 0; dx < w; ++dx)
              ++counts[dense[((z * w + dz) * kFull + (y * w + dy)) * kFull + (x * w + dx)]];
        int best = 0, bestCount = -1;
        for (int m = 0; m < 256; ++m)
          if (counts[m] > bestCount) {
            bestCount = counts[m];
            best = m;
          }
        coarse[(z * S + y) * S + x] = static_cast<uint8_t>(best);
      }
}

/// Greedy-mesh the dense S^3 material grid: for each of the 6 facings, sweep
/// slices, build the exposed-face mask, cull faces hidden by filled neighbours
/// (or by `neighborSolid` at the block boundary), and greedy-merge rectangles
/// of equal material. Fixed iteration order everywhere -> deterministic.
size_t greedyMeshGrid(const uint8_t* g, int S, int lod, const MicroMeshParams& p,
                      std::vector<MicroQuad>& out) {
  const size_t n0 = out.size();
  std::vector<uint8_t> mask(static_cast<size_t>(S) * S);
  std::vector<uint8_t> done(static_cast<size_t>(S) * S);
  const float inv = 1.0f / float(S);
  for (int a = 0; a < 3; ++a) {
    // Tangent axes, same convention as BinaryQuad: X faces -> (Y, Z),
    // Y faces -> (X, Z), Z faces -> (X, Y).
    const int t0 = (a == 0) ? 1 : 0;
    const int t1 = (a == 2) ? 1 : 2;
    for (int neg = 0; neg < 2; ++neg) {
      const int dir = 2 * a + neg;  // 0..5: +X,-X,+Y,-Y,+Z,-Z
      for (int slice = 0; slice < S; ++slice) {
        for (int v = 0; v < S; ++v) {
          for (int u = 0; u < S; ++u) {
            int c[3] = {0, 0, 0};
            c[a] = slice;
            c[t0] = u;
            c[t1] = v;
            const uint8_t m = g[(c[2] * S + c[1]) * S + c[0]];
            uint8_t f = 0;
            if (m != 0) {
              const int ns = slice + (neg ? -1 : 1);
              bool nfilled;
              if (ns < 0 || ns >= S) {
                nfilled = p.neighborSolid[dir] != 0;
              } else {
                int nc[3] = {c[0], c[1], c[2]};
                nc[a] = ns;
                nfilled = g[(nc[2] * S + nc[1]) * S + nc[0]] != 0;
              }
              if (!nfilled) f = m;
            }
            mask[static_cast<size_t>(v) * S + u] = f;
          }
        }
        std::fill(done.begin(), done.end(), uint8_t(0));
        for (int v = 0; v < S; ++v) {
          for (int u = 0; u < S; ++u) {
            const size_t mi = static_cast<size_t>(v) * S + u;
            const uint8_t m = mask[mi];
            if (m == 0 || done[mi]) continue;
            int w = 1;
            while (u + w < S && mask[mi + w] == m && !done[mi + w]) ++w;
            int h = 1;
            while (v + h < S) {
              bool ok = true;
              for (int k = 0; k < w; ++k) {
                const size_t ci = static_cast<size_t>(v + h) * S + u + k;
                if (mask[ci] != m || done[ci]) {
                  ok = false;
                  break;
                }
              }
              if (!ok) break;
              ++h;
            }
            for (int dv = 0; dv < h; ++dv)
              for (int du = 0; du < w; ++du) done[static_cast<size_t>(v + dv) * S + u + du] = 1;
            // Emit: plane at slice (neg) or slice+1, min corner + w/h in block units.
            float e[3];
            e[a] = float(slice + (neg ? 0 : 1)) * inv;
            e[t0] = float(u) * inv;
            e[t1] = float(v) * inv;
            MicroQuad q;
            q.x = e[0] + p.originX;
            q.y = e[1] + p.originY;
            q.z = e[2] + p.originZ;
            q.w = float(w) * inv;
            q.h = float(h) * inv;
            q.dir = static_cast<uint8_t>(dir);
            q.material = m;
            q.lod = static_cast<uint8_t>(lod);
            q.reserved = 0;
            out.push_back(q);
          }
        }
      }
    }
  }
  return out.size() - n0;
}

}  // namespace

int selectLOD(float cameraDistance) {
  if (!(cameraDistance >= 0.0f)) cameraDistance = 0.0f;  // NaN -> closest
  if (cameraDistance < 4.0f) return 6;
  if (cameraDistance < 8.0f) return 5;
  if (cameraDistance < 16.0f) return 4;
  if (cameraDistance < 32.0f) return 3;
  if (cameraDistance < 64.0f) return 2;
  if (cameraDistance < 128.0f) return 1;
  return 0;
}

size_t meshMicroLeaves(const std::vector<subdiv::MicroCell>& leaves, int lodLevel,
                       const MicroMeshParams& params, std::vector<MicroQuad>& out) {
  if (lodLevel < 0) lodLevel = 0;
  if (lodLevel > subdiv::kMaxLevel) lodLevel = subdiv::kMaxLevel;
  const int S = 1 << lodLevel;
  std::vector<uint8_t> dense(static_cast<size_t>(kFull) * kFull * kFull, 0);
  rasterizeLeaves(leaves, dense.data());
  if (lodLevel == subdiv::kMaxLevel) return greedyMeshGrid(dense.data(), S, lodLevel, params, out);
  std::vector<uint8_t> coarse(static_cast<size_t>(S) * S * S);
  downsampleMajority(dense.data(), S, coarse.data());
  return greedyMeshGrid(coarse.data(), S, lodLevel, params, out);
}

size_t meshOctreeLOD(const subdiv::SparseOctree& tree, int lodLevel,
                     const MicroMeshParams& params, std::vector<MicroQuad>& out) {
  if (tree.empty()) return 0;
  return meshMicroLeaves(tree.leaves(), lodLevel, params, out);
}

size_t appendChunkMicroGeometry(const subdiv::SubdivStore& store, int regionMinX, int regionMinY,
                                int regionMinZ, float camX, float camY, float camZ,
                                std::vector<MicroQuad>& out,
                                std::function<void(int bx, int by, int bz, uint8_t solid[6])>
                                    neighborSolid) {
  return appendChunkMicroGeometryT(store, regionMinX, regionMinY, regionMinZ, camX, camY, camZ,
                                   out, neighborSolid);
}

}  // namespace litholattice::micro
