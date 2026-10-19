// Ecosystem & environment diversity stages: implementations. See diversity.h for the
// design notes. Every placement decision is a pure function of (dimensionSeed, world
// position); chunks never read each other at gen time.
#include "litholattice/procgen/diversity.h"
#include <algorithm>
#include <cmath>
#include "litholattice/procgen/seed.h"

namespace litholattice::procgen {
namespace {

// ---- deterministic position-keyed helpers (mirror stages.cpp's discipline) ----

uint64_t posSeed(uint64_t featSeed, int32_t wx, int32_t wz) {
  return deriveSeed(featSeed, mix64((uint64_t(uint32_t(wx)) << 32) | uint64_t(uint32_t(wz))));
}

float posRoll(uint64_t featSeed, int32_t wx, int32_t wz) {
  return unitFloat(latticeHash(featSeed, wx, wz));
}

/// A second independent roll for the same position (different salt stream).
float posRoll2(uint64_t featSeed, int32_t wx, int32_t wz) {
  return unitFloat(latticeHash(featSeed ^ 0x9E3779B97F4A7C15ull, wx, wz));
}

/// Writes m at chunk-local (x, y, z) when inside the chunk and the cell is still air.
bool divIntoAir(StageContext& ctx, int x, int y, int z, MaterialId m) {
  if (m == kAirMaterial || !ctx.blocks.contains(x, y, z)) return false;
  if (ctx.blocks.get(x, y, z) != kAirMaterial) return false;
  ctx.blocks.set(x, y, z, m);
  return true;
}

/// Smooth 2D value noise on the integer lattice: pure function of (seed, position).
float valueNoise2(uint64_t seed, float x, float z) {
  const int xi = int(std::floor(x)), zi = int(std::floor(z));
  const float xf = x - float(xi), zf = z - float(zi);
  const float u = xf * xf * (3.f - 2.f * xf), v = zf * zf * (3.f - 2.f * zf);
  const float c00 = signedUnitFloat(latticeHash(seed, xi, 0, zi));
  const float c10 = signedUnitFloat(latticeHash(seed, xi + 1, 0, zi));
  const float c01 = signedUnitFloat(latticeHash(seed, xi, 0, zi + 1));
  const float c11 = signedUnitFloat(latticeHash(seed, xi + 1, 0, zi + 1));
  return (c00 + u * (c10 - c00)) + v * ((c01 + u * (c11 - c01)) - (c00 + u * (c10 - c00)));
}

uint64_t featSeedFor(uint64_t dimensionSeed, const std::string& seedName, const char* fallback) {
  return deriveSeed(dimensionSeed, nameHash(seedName.empty() ? fallback : seedName));
}

}  // namespace

// ---- 1. ground cover ---------------------------------------------------------------

void GroundCoverFeature::place(StageContext& ctx, Rng&) const {
  if (s_.entries.empty()) return;
  const uint64_t featSeed = featSeedFor(ctx.chunk.dimensionSeed, s_.seedName, "groundcover");
  const int n = ctx.chunk.size, top = ctx.blocks.height();
  const int x0 = ctx.chunk.worldX(0), z0 = ctx.chunk.worldZ(0);
  for (int lz = 0; lz < n; ++lz)
    for (int lx = 0; lx < n; ++lx) {
      const int32_t wx = x0 + lx, wz = z0 + lz;
      const size_t col = size_t(lz * n + lx);
      // Single roll against the cumulative density: at most one cover per column.
      float total = 0.f;
      for (const auto& e : s_.entries) total += blendedDensity(e.biomes, ctx.maps, col);
      if (total <= 0.f) continue;
      const float roll = posRoll(featSeed, wx, wz);
      if (roll >= total) continue;
      float acc = 0.f;
      const GroundCoverEntry* pick = nullptr;
      for (const auto& e : s_.entries) {
        acc += blendedDensity(e.biomes, ctx.maps, col);
        if (roll < acc) { pick = &e; break; }
      }
      if (!pick) pick = &s_.entries.back();
      const int surfY = ctx.maps.height[col];
      if (surfY < 0 || surfY + pick->maxH >= top) continue;
      const MaterialId topBlock = ctx.blocks.get(lx, surfY, lz);
      if (pick->surface != kAirMaterial) {
        if (topBlock != pick->surface) continue;
      } else if (topBlock == kAirMaterial) {
        continue;  // needs solid ground of some kind
      }
      Rng r(posSeed(featSeed, wx, wz));
      const int h = r.range(pick->minH, pick->maxH);
      const uint8_t tint = pick->tintByBiome && col < ctx.maps.biome.size() ? ctx.maps.biome[col] : 0;
      for (int i = 1; i <= h; ++i) {
        if (!divIntoAir(ctx, lx, surfY + i, lz, pick->material)) break;
        if (pick->tintByBiome) ctx.blocks.setState(lx, surfY + i, lz, tint);
      }
    }
}

// ---- 2. rock formations ------------------------------------------------------------

void RockFormationFeature::place(StageContext& ctx, Rng&) const {
  const uint64_t featSeed = featSeedFor(ctx.chunk.dimensionSeed, s_.seedName, "rock_formation");
  const uint64_t noiseSeed = deriveSeed(featSeed, nameHash("wobble"));
  const int n = ctx.chunk.size, top = ctx.blocks.height();
  const int reach = s_.maxR;
  const int x0 = ctx.chunk.worldX(0), z0 = ctx.chunk.worldZ(0);
  const float wobFreq = 0.35f;  // silhouette wobble frequency (per block)
  for (int bz = -reach; bz < n + reach; ++bz)
    for (int bx = -reach; bx < n + reach; ++bx) {
      const int lx = std::clamp(bx, 0, n - 1), lz = std::clamp(bz, 0, n - 1);
      const int32_t wx = x0 + bx, wz = z0 + bz;
      const size_t col = size_t(lz * n + lx);
      const float p = s_.density * blendedDensity(s_.biomes, ctx.maps, col);
      if (p <= 0.f || posRoll(featSeed, wx, wz) >= p) continue;
      const int surfY = ctx.maps.height[col];
      if (surfY < 0) continue;
      const MaterialId topBlock = ctx.blocks.get(lx, surfY, lz);
      if (s_.surface != kAirMaterial) {
        if (topBlock != s_.surface) continue;
      } else if (topBlock == kAirMaterial) {
        continue;
      }
      Rng r(posSeed(featSeed, wx, wz));
      const int rad = r.range(s_.minR, s_.maxR);
      const int h = r.range(s_.minH, s_.maxH);
      if (surfY + h >= top) continue;
      // Noise-wobbled ellipsoid blob grown upward from the surface.
      for (int dz = -rad; dz <= rad; ++dz)
        for (int dx = -rad; dx <= rad; ++dx) {
          const float dr = std::sqrt(float(dx * dx + dz * dz)) / float(rad);
          if (dr >= 1.f) continue;
          const float wob = 1.f + s_.wobble * valueNoise2(noiseSeed, float(wx + dx) * wobFreq,
                                                          float(wz + dz) * wobFreq);
          const int ch = std::max(1, int(float(h) * (1.f - dr * dr) * wob));
          const int lx2 = bx + dx, lz2 = bz + dz;  // chunk-local, may be out of range
          for (int i = 1; i <= ch; ++i) divIntoAir(ctx, lx2, surfY + i, lz2, s_.material);
        }
    }
}

// ---- 3. biome transition bands -----------------------------------------------------

void BiomeTransitionStage::onPipelineBuilt(const GeneratorPipeline* pipeline) {
  // The un-transitioned dominant biome at any world column. The pipeline's biomeAt uses
  // only the selector, so this cannot recurse into process().
  probe_ = [pipeline](int32_t wx, int32_t wz) { return pipeline->biomeAt(wx, wz); };
}

void BiomeTransitionStage::process(StageContext& ctx) const {
  if (rules_.empty() || ctx.maps.biome.empty()) return;
  const int n = ctx.chunk.size;
  const int x0 = ctx.chunk.worldX(0), z0 = ctx.chunk.worldZ(0);
  const size_t nb = ctx.maps.biomeCount;
  const bool haveWeights = nb > 0 && ctx.maps.biomeWeight.size() >= ctx.maps.biome.size() * nb;
  // Snapshot before mutating: in-chunk neighbors must read the pre-transition
  // biomes, otherwise transitions cascade within the chunk and disagree with
  // the un-transitioned probe used for out-of-chunk neighbors (border seams).
  const std::vector<uint8_t> before = ctx.maps.biome;
  for (int lz = 0; lz < n; ++lz)
    for (int lx = 0; lx < n; ++lx) {
      const size_t col = size_t(lz * n + lx);
      const uint8_t d = before[col];
      for (const auto& rule : rules_) {
        if (rule.a != d && rule.b != d) continue;
        const uint8_t other = (rule.a == d) ? rule.b : rule.a;
        bool found = false;
        const int w = std::max(1, rule.width);
        for (int dz = -w; dz <= w && !found; ++dz)
          for (int dx = -w; dx <= w && !found; ++dx) {
            if (dx == 0 && dz == 0) continue;
            const int nx = lx + dx, nz = lz + dz;
            uint8_t nd;
            if (nx >= 0 && nx < n && nz >= 0 && nz < n) {
              nd = before[size_t(nz * n + nx)];
            } else if (probe_) {
              nd = probe_(x0 + lx + dx, z0 + lz + dz);
            } else {
              continue;
            }
            if (nd == other) found = true;
          }
        if (found) {
          ctx.maps.biome[col] = rule.result;
          if (haveWeights) {
            float* weights = ctx.maps.biomeWeight.data() + col * nb;
            for (size_t b = 0; b < nb; ++b) weights[b] = 0.f;
            if (rule.result < nb) weights[rule.result] = 1.f;
          }
          break;  // first matching rule wins; bands are narrow by design
        }
      }
    }
}

// ---- 4. Worley ore veins -----------------------------------------------------------

float WorleyOreVeinFeature::worleyF1(uint64_t seed, float x, float y, float z) {
  const int xi = int(std::floor(x)), yi = int(std::floor(y)), zi = int(std::floor(z));
  float best = 8.f;
  for (int dz = -1; dz <= 1; ++dz)
    for (int dy = -1; dy <= 1; ++dy)
      for (int dx = -1; dx <= 1; ++dx) {
        const int cx = xi + dx, cy = yi + dy, cz = zi + dz;
        // Three jittered feature-point coordinates from hashed lattice bits.
        const uint32_t h1 = latticeHash(seed, cx, cy, cz);
        const uint32_t h2 = latticeHash(seed ^ 0x9E3779B97F4A7C15ull, cx, cy, cz);
        const float px = float(cx) + float(h1 >> 8) * (1.f / 16777216.f);
        const float py = float(cy) + float(h1 & 0xFFFFFFu) * (1.f / 16777216.f);
        const float pz = float(cz) + float(h2 >> 8) * (1.f / 16777216.f);
        const float ddx = x - px, ddy = y - py, ddz = z - pz;
        const float d2 = ddx * ddx + ddy * ddy + ddz * ddz;
        if (d2 < best) best = d2;
      }
  return std::sqrt(best);  // ~[0, 1.74]; normalized below by the caller if needed
}

void WorleyOreVeinFeature::place(StageContext& ctx, Rng&) const {
  const int n = ctx.chunk.size, height = ctx.blocks.height();
  const int lo = std::max(0, s_.minY), hi = std::min(height - 1, s_.maxY);
  if (lo > hi || s_.material == kAirMaterial || s_.replace == kAirMaterial) return;
  const uint64_t seed = featSeedFor(ctx.chunk.dimensionSeed, s_.seedName, "worley_ore");
  const uint64_t maskSeed = deriveSeed(seed, nameHash("cluster"));
  const float f = s_.frequency > 0.f ? s_.frequency : 0.08f;
  const int x0 = ctx.chunk.worldX(0), z0 = ctx.chunk.worldZ(0);
  const bool clustered = s_.clusterFreq > 0.f;
  for (int lz = 0; lz < n; ++lz)
    for (int lx = 0; lx < n; ++lx) {
      const float wx = float(x0 + lx), wz = float(z0 + lz);
      float mask = 1.f;
      if (clustered) {
        mask = valueNoise2(maskSeed, wx * s_.clusterFreq, wz * s_.clusterFreq);
        if (mask <= s_.clusterThreshold) continue;
      }
      for (int y = lo; y <= hi; ++y) {
        if (ctx.blocks.get(lx, y, lz) != s_.replace) continue;
        // The Worley field is a pure function of world position: chunk-pure, no replay.
        const float f1 = worleyF1(seed, wx * f, float(y) * f, wz * f);
        if (f1 < s_.threshold) ctx.blocks.set(lx, y, lz, s_.material);
      }
    }
}

// ---- 7. weather-shaped generation --------------------------------------------------

void SnowCapRule::apply(StageContext& ctx) const {
  if (s_.snow == kAirMaterial) return;
  const uint64_t seed = featSeedFor(ctx.chunk.dimensionSeed, s_.seedName, "snowcap");
  const int n = ctx.chunk.size;
  const int z0 = ctx.chunk.worldZ(0);
  const float freq = s_.noiseFreq > 0.f ? s_.noiseFreq : 0.01f;
  for (int lz = 0; lz < n; ++lz)
    for (int lx = 0; lx < n; ++lx) {
      const size_t col = size_t(lz * n + lx);
      const int surfY = ctx.maps.height[col];
      if (surfY < s_.minHeight) continue;
      const int32_t wz = z0 + lz;
      const float line = float(s_.baseLine) - float(std::abs(wz)) * s_.latitudeFalloff +
                         s_.noiseAmp * valueNoise2(seed, float(ctx.chunk.worldX(lx)) * freq,
                                                   float(wz) * freq);
      if (float(surfY) >= line && ctx.blocks.get(lx, surfY, lz) != kAirMaterial)
        ctx.blocks.set(lx, surfY, lz, s_.snow);
    }
}

void SandDriftRule::apply(StageContext& ctx) const {
  if (s_.sand == kAirMaterial) return;
  const uint64_t seed = featSeedFor(ctx.chunk.dimensionSeed, s_.seedName, "sand_drift");
  const int n = ctx.chunk.size;
  const int x0 = ctx.chunk.worldX(0), z0 = ctx.chunk.worldZ(0);
  const float freq = s_.frequency > 0.f ? s_.frequency : 0.02f;
  for (int lz = 0; lz < n; ++lz)
    for (int lx = 0; lx < n; ++lx) {
      const size_t col = size_t(lz * n + lx);
      const float p = blendedDensity(s_.biomes, ctx.maps, col);
      if (p <= 0.f) continue;
      const int32_t wx = x0 + lx, wz = z0 + lz;
      if (posRoll(seed, wx, wz) >= p) continue;
      if (valueNoise2(seed ^ 0x51ED2707A94E9575ull, float(wx) * freq, float(wz) * freq) <=
          s_.threshold)
        continue;
      const int surfY = ctx.maps.height[col];
      for (int i = 0; i < s_.depth; ++i) {
        const int y = surfY - i;
        if (y < 0 || ctx.blocks.get(lx, y, lz) == kAirMaterial) break;
        ctx.blocks.set(lx, y, lz, s_.sand);
      }
    }
}

}  // namespace litholattice::procgen
