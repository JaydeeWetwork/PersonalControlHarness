// Biome scouting: spiral search for the nearest column of a biome type.
// Deterministic: the spiral order is fixed and the biome query
// (GeneratorPipeline::biomeAt) is a pure function of seed + position.
// LF line endings.
#include "lithokraft/game/scout.h"

#include <cmath>
#include <cstdio>
#include <vector>

#include "litholattice/world/column_generator.h"

namespace lithokraft {
namespace {

/// Floor division for a possibly-negative dividend (b > 0).
int64_t floorDiv64(int64_t a, int64_t b) {
  const int64_t q = a / b, r = a % b;
  return r < 0 ? q - 1 : q;
}

}  // namespace

BiomeLocateResult locateBiome(const litholattice::ColumnGenerator& gen, uint8_t biomeIndex,
                             double px, double pz, int maxDistanceBlocks) {
  BiomeLocateResult out;
  constexpr int kStep = 8;  // sample grid spacing in blocks
  // The player's sample cell; sample (i, j) sits at (i*8 + 4, j*8 + 4).
  const int64_t ipx = floorDiv64(int64_t(std::floor(px)), kStep);
  const int64_t ipz = floorDiv64(int64_t(std::floor(pz)), kStep);
  const int maxRing = std::max(0, (maxDistanceBlocks + kStep - 1) / kStep);

  bool found = false;
  int32_t bestX = 0, bestZ = 0;
  double bestD = 1e30;
  std::vector<int32_t> xs, zs;
  std::vector<uint8_t> biomes;
  xs.reserve(1024);
  zs.reserve(1024);
  biomes.reserve(1024);

  for (int r = 0; r <= maxRing; ++r) {
    // Ring r: max(|di|, |dj|) == r. Batched: one sampleBiomes call per ring.
    xs.clear();
    zs.clear();
    if (r == 0) {
      xs.push_back(int32_t(ipx * kStep + kStep / 2));
      zs.push_back(int32_t(ipz * kStep + kStep / 2));
    } else {
      for (int64_t d = -r; d <= r; ++d) {
        for (int k = 0; k < 4; ++k) {
          // Walk the four edges without duplicating corners.
          const int64_t di = k == 0 ? d : (k == 1 ? r : (k == 2 ? -d : -r));
          const int64_t dj = k == 0 ? -r : (k == 1 ? d : (k == 2 ? r : -d));
          if (k > 0 && (d == -r || d == r)) continue;  // corners belong to edge 0/2
          xs.push_back(int32_t((ipx + di) * kStep + kStep / 2));
          zs.push_back(int32_t((ipz + dj) * kStep + kStep / 2));
        }
      }
    }
    biomes.assign(xs.size(), 0);
    gen.pipeline().sampleBiomes(xs.data(), zs.data(), xs.size(), biomes.data());
    for (size_t i = 0; i < xs.size(); ++i) {
      if (biomes[i] != biomeIndex) continue;
      const double dx = double(xs[i]) - px, dz = double(zs[i]) - pz;
      const double d = std::sqrt(dx * dx + dz * dz);
      if (d < bestD) {
        bestD = d;
        bestX = xs[i];
        bestZ = zs[i];
        found = true;
      }
    }
    // Early termination: ring r+1's closest possible sample is (r + 0.5) * kStep
    // blocks out; when the best hit beats that, no later ring can win.
    if (found && bestD < (double(r) + 0.5) * kStep) break;
  }

  if (!found) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "no biome %u within %d blocks", unsigned(biomeIndex), maxDistanceBlocks);
    out.message = buf;
    return out;
  }
  // Refine: 1-block scan of the 48-block neighborhood around the best sample.
  int32_t fx = bestX, fz = bestZ;
  double fd = bestD;
  constexpr int kRefine = 24;
  for (int32_t z = bestZ - kRefine; z < bestZ + kRefine; ++z)
    for (int32_t x = bestX - kRefine; x < bestX + kRefine; ++x) {
      if (gen.pipeline().biomeAt(x, z) != biomeIndex) continue;
      const double dx = double(x) - px, dz = double(z) - pz;
      const double d = std::sqrt(dx * dx + dz * dz);
      if (d < fd) {
        fd = d;
        fx = x;
        fz = z;
      }
    }
  out.found = true;
  out.x = fx;
  out.z = fz;
  out.distance = fd;
  char buf[160];
  std::snprintf(buf, sizeof buf, "biome %u: nearest at (%d, %d), %.0f blocks away", unsigned(biomeIndex),
                fx, fz, fd);
  out.message = buf;
  return out;
}

}  // namespace lithokraft
