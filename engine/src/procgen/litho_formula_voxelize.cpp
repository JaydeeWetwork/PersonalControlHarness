// Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
// concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
// from the mathematical definition; no third-party code referenced. Underlying patent
// expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
// ambition of No Man's Sky (Hello Games).

#include "litholattice/procgen/litho_formula_voxelize.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

namespace litholattice::procgen::litho_formula {

VoxelResult voxelizeLithoShape(const LithoParams& pTheta, const LithoParams& pPhi,
                               int resolution, uint8_t material) {
  (void)material;  // reserved for future use; see the header.
  if (resolution != 8 && resolution != 16 && resolution != 32 && resolution != 64) {
    std::fprintf(stderr,
                 "litho_formula_voxelize: unsupported resolution %d "
                 "(must be 8, 16, 32, or 64)\n",
                 resolution);
    VoxelResult rejected;
    rejected.resolution = 0;
    return rejected;
  }
  const int n = resolution;

  // 1. Sample the parametric surface densely; track the bounding radius so
  //    the shape is normalized into the grid with a one-cell border.
  //    Sampling density scales with n to keep thin features watertight.
  const int nTheta = 8 * n;
  const int nPhi = 4 * n;
  const double pi = std::acos(-1.0);
  double maxR = 0.0;
  for (int i = 0; i < nTheta; ++i) {
    const double theta = -pi + 2.0 * pi * (i + 0.5) / nTheta;
    for (int j = 0; j < nPhi; ++j) {
      const double phi = -pi / 2.0 + pi * (j + 0.5) / nPhi;
      const auto pt = lithoPoint(theta, phi, pTheta, pPhi);
      if (!std::isfinite(pt.x + pt.y + pt.z)) continue;
      maxR = std::max({maxR, std::fabs(pt.x), std::fabs(pt.y), std::fabs(pt.z)});
    }
  }
  VoxelResult out;
  out.resolution = n;
  if (!(maxR > 0.0)) return out;  // degenerate parameters: empty result

  // 2. Rasterize + interior fill, sub-cell aware. For each (x, y) column,
  //    track the continuous min/max sample z-coordinate, then fill the
  //    cells whose centers lie within the sampled span (a parity scan
  //    along z, but with sub-cell crossing estimates).
  //
  //    Axis mapping: formula (x, y, z) -> grid (x, z, y) so the polar axis
  //    (formula z) becomes game up (grid y).
  //
  //    Why continuous spans instead of binary shell marks: a binary shell
  //    forces whole boundary cells in/out, overcounting the surface by
  //    ~0.5 cell per side (~7% volume error on a 64^3 sphere). The sampled
  //    span estimates the true surface crossing within the sample spacing
  //    (~0.2 cells), and filling by cell centers keeps the bias near zero.
  //    A column's fill is always bounded by its own samples, so -- unlike
  //    a naive even-odd toggle, which under-counts merged crossings and
  //    leaks to the grid edge -- this can never escape the shape.
  const double scale = (n / 2.0 - 1.0) / maxR;
  const double center = (n - 1) / 2.0;
  const double kInf = 1e30;
  // Per (x, y) column: min/max sample z (continuous grid coordinates).
  std::vector<double> colLo(static_cast<size_t>(n) * n, kInf);
  std::vector<double> colHi(static_cast<size_t>(n) * n, -kInf);
  for (int i = 0; i < nTheta; ++i) {
    const double theta = -pi + 2.0 * pi * (i + 0.5) / nTheta;
    for (int j = 0; j < nPhi; ++j) {
      const double phi = -pi / 2.0 + pi * (j + 0.5) / nPhi;
      const auto pt = lithoPoint(theta, phi, pTheta, pPhi);
      if (!std::isfinite(pt.x + pt.y + pt.z)) continue;
      const double mx = pt.x * scale + center;
      const double my = pt.z * scale + center;
      const double mz = pt.y * scale + center;
      const int ix = static_cast<int>(std::floor(mx));
      const int iy = static_cast<int>(std::floor(my));
      const int iz = static_cast<int>(std::floor(mz));
      if (ix < 0 || ix >= n || iy < 0 || iy >= n || iz < 0 || iz >= n) continue;
      const size_t c = static_cast<size_t>(ix) * n + iy;
      if (mz < colLo[c]) colLo[c] = mz;
      if (mz > colHi[c]) colHi[c] = mz;
    }
  }

  // 3. Interior fill: for each (x, y) column, fill the cells whose centers
  //    lie within the column's sampled z-span.
  const size_t total = static_cast<size_t>(n) * n * n;
  std::vector<unsigned char> filledMask(total, 0);
  for (int ix = 0; ix < n; ++ix) {
    for (int iy = 0; iy < n; ++iy) {
      const size_t c = static_cast<size_t>(ix) * n + iy;
      const double lo = colLo[c], hi = colHi[c];
      if (lo > hi) continue;  // column never sampled: outside the shape
      for (int iz = 0; iz < n; ++iz) {
        const double ctr = iz + 0.5;
        if (ctr >= lo && ctr <= hi)
          filledMask[(static_cast<size_t>(iz) * n + iy) * n + ix] = 1;
      }
    }
  }
  out.filled.reserve(1024);
  // 4. Speckle filter: parametric sampling can leave isolated 1-cell marks
  //    floating apart from the body. The shape is defined as the largest
  //    6-connected component; anything else is sampling dust and is
  //    discarded.
  std::vector<int> comp(total, -1);
  std::vector<std::array<int, 3>> stack;
  int bestLabel = -1;
  size_t bestSize = 0;
  int nextLabel = 0;
  const int NB[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                        {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  auto linIdx = [&](int x, int y, int z) -> size_t {
    return static_cast<size_t>((z * n + y) * n + x);
  };
  for (int z = 0; z < n; ++z)
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        const size_t c = linIdx(x, y, z);
        if (!filledMask[c] || comp[c] >= 0) continue;
        // Flood-fill this component.
        size_t size = 0;
        stack.clear();
        stack.push_back({x, y, z});
        comp[c] = nextLabel;
        while (!stack.empty()) {
          const auto cur = stack.back();
          stack.pop_back();
          ++size;
          for (const auto& d : NB) {
            const int nx = cur[0] + d[0], ny = cur[1] + d[1], nz = cur[2] + d[2];
            if (nx < 0 || nx >= n || ny < 0 || ny >= n || nz < 0 || nz >= n)
              continue;
            const size_t nc = linIdx(nx, ny, nz);
            if (filledMask[nc] && comp[nc] < 0) {
              comp[nc] = nextLabel;
              stack.push_back({nx, ny, nz});
            }
          }
        }
        if (size > bestSize) {
          bestSize = size;
          bestLabel = nextLabel;
        }
        ++nextLabel;
      }
  for (int z = 0; z < n; ++z)
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x)
        if (bestLabel >= 0 && comp[linIdx(x, y, z)] == bestLabel)
          out.filled.push_back({x, y, z});
  return out;
}

void emitToOctree(const VoxelResult& result, litholattice::subdiv::SparseOctree& tree) {
  emitToOctree(result, tree, 1);
}

void emitToOctree(const VoxelResult& result, litholattice::subdiv::SparseOctree& tree,
                  uint8_t material) {
  const int n = result.resolution;
  // Only results from accepted resolutions emit; rejected (resolution 0)
  // or hand-built results with an unsupported resolution emit nothing.
  if (n != 8 && n != 16 && n != 32 && n != 64) return;
  if (result.filled.empty()) return;
  int level = 0;
  for (int p = n; p > 1; p >>= 1) ++level;  // exact log2: 8->3, 16->4, 32->5, 64->6
  for (const auto& cell : result.filled) {
    // Coordinates are in [0, n) == [0, 2^level).
    tree.set(level, cell[0], cell[1], cell[2], material);
  }
}

}  // namespace litholattice::procgen::litho_formula
