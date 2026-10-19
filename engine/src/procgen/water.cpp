#include "litholattice/procgen/water.h"
#include <algorithm>
#include <cmath>
#include "litholattice/procgen/seed.h"

namespace litholattice::procgen {
namespace {

/// Seeded fbm relief/mask noise: OpenSimplex2S source, 4 fbm octaves, output in [-1, 1].
std::shared_ptr<const INoise> makeWaterNoise(uint64_t seed, std::string_view salt) {
  NoiseSettings ns;
  ns.type = NoiseType::OpenSimplex2S;
  ns.seed = deriveSeed(seed, nameHash(salt));
  std::shared_ptr<const INoise> source = createNoise(ns);
  FractalSettings fs;
  fs.type = FractalType::FBm;
  fs.octaves = 4;
  return createFractal(source, fs, deriveSeed(seed, nameHash(salt) ^ 0x9E3779B97F4A7C15ull));
}

int clampLevel(int level, int top) { return std::clamp(level, 1, top); }

}  // namespace

// ---- oceans

OceanFillRule::OceanFillRule(OceanSettings s) : s_(std::move(s)) {
  // No engine cap (2026-10-13 parameterization): maxDepth is a settings value.
  // Sanity only: a depth must be positive, and an explicit minDepth must lie in
  // [0, maxDepth]. (The loader validates seaLevel vs maxDepth vs minSolidBelow.)
  s_.maxDepth = std::max(1, s_.maxDepth);
  if (s_.minDepth >= 0) s_.minDepth = std::clamp(s_.minDepth, 0, s_.maxDepth);
  if (s_.minSolidBelow < 0) s_.minSolidBelow = 0;
  if (!(s_.depthFrequency > 0.f) || !std::isfinite(s_.depthFrequency)) s_.depthFrequency = 1.f / 256.f;
  relief_ = makeWaterNoise(s_.seed, "litholattice.procgen.ocean");
}

int OceanFillRule::depthForUnit(float u, int maxDepth, int minDepth) {
  const float uc = std::clamp(u, 0.f, 1.f);
  // minDepth < 0 = unset: derive maxDepth/4 -- exactly the pre-parameterization
  // behaviour (196/4 = 49 at the LithoKraft default).
  const int minD = minDepth >= 0 ? minDepth : maxDepth / 4;
  return minD + int(uc * float(maxDepth - minD));  // == maxDepth at u == 1, monotone
}

int OceanFillRule::floorTarget(int wx, int wz, int seaLevel) const {
  const float d01 =
      (relief_->sample2(float(wx) * s_.depthFrequency, float(wz) * s_.depthFrequency) + 1.f) * 0.5f;
  const int depth = depthForUnit(d01, s_.maxDepth, s_.minDepth);
  // Never below seaLevel - maxDepth (and never below y 0): arithmetic, from the
  // configured depth -- not an engine cap.
  return std::clamp(seaLevel - depth, 0, seaLevel - 1);
}

void OceanFillRule::apply(StageContext& ctx) const {
  if (s_.water == kAirMaterial) return;
  const int n = ctx.chunk.size, top = ctx.blocks.height() - 1;
  const int sea = clampLevel(s_.seaLevel, top);
  for (int z = 0; z < n; ++z) {
    for (int x = 0; x < n; ++x) {
      const size_t i = size_t(z * n + x);
      const float w = blendedDensity(s_.biomes, ctx.maps, i);
      if (!(w > 0.f)) continue;
      const int h = ctx.maps.height[i];
      if (h < 0) continue;  // no solid floor: no ocean here
      const int wx = ctx.chunk.worldX(x), wz = ctx.chunk.worldZ(z);
      const int ft = floorTarget(wx, wz, sea);
      // Blend the existing (already biome-blended) terrain toward the ocean floor by the
      // eligibility weight: shores grade smoothly, no cliffs at biome borders. One-hot
      // weights reduce to the dominant biome's choice.
      int floorY = h;
      if (ft > h) floorY = h + int(float(ft - h) * w + 0.5f);
      if (floorY > h) {
        // Raise an over-deep floor: sample the column's own surface block (post surface
        // rules, e.g. sand) unless the config names a floor material. Solid from y 0 up
        // is preserved: the raise only ever adds solid, and floorY >= sea - maxDepth
        // keeps the configured solid-below margin under the liquid.
        const MaterialId fill =
            s_.floorMaterial != kAirMaterial ? s_.floorMaterial : ctx.blocks.get(x, h, z);
        for (int y = h + 1; y <= floorY; ++y) ctx.blocks.set(x, y, z, fill);
        ctx.maps.height[i] = floorY;  // highest solid y moved: carvers/shore see the truth
      }
      if (floorY < sea) {
        for (int y = floorY + 1; y <= sea; ++y) {
          ctx.blocks.set(x, y, z, s_.water);
          ctx.blocks.setState(x, y, z, kStillWaterLevel);
        }
      }
    }
  }
}

// ---- lakes

LakeFillRule::LakeFillRule(LakeSettings s) : s_(std::move(s)) {
  s_.maxDepth = std::max(1, s_.maxDepth);  // settings value, no engine cap; sanity: positive
  s_.threshold = std::clamp(s_.threshold, 0.f, 1.f);
  if (!(s_.frequency > 0.f) || !std::isfinite(s_.frequency)) s_.frequency = 1.f / 64.f;
  mask_ = makeWaterNoise(s_.seed, "litholattice.procgen.lake");
}

float LakeFillRule::maskAt(int wx, int wz) const {
  return (mask_->sample2(float(wx) * s_.frequency, float(wz) * s_.frequency) + 1.f) * 0.5f;
}

void LakeFillRule::apply(StageContext& ctx) const {
  if (s_.water == kAirMaterial) return;
  const int n = ctx.chunk.size, top = ctx.blocks.height() - 1;
  const int wt = clampLevel(s_.waterTable, top);
  for (int z = 0; z < n; ++z) {
    for (int x = 0; x < n; ++x) {
      const size_t i = size_t(z * n + x);
      const float w = blendedDensity(s_.biomes, ctx.maps, i);
      if (!(w > 0.f)) continue;
      const int h = ctx.maps.height[i];
      if (h < 0 || h >= wt) continue;  // needs a depression below the water table
      const int wx = ctx.chunk.worldX(x), wz = ctx.chunk.worldZ(z);
      // Eligibility-blended mask threshold: full eligibility uses the configured
      // threshold, fading eligibility needs a stronger mask peak. Continuous in w.
      const float effThreshold = 1.f - w * (1.f - s_.threshold);
      if (!(maskAt(wx, wz) > effThreshold)) continue;
      // Fill flat to the spill level, capped: deeper pits hold exactly maxDepth.
      const int depth = std::min(wt - h, s_.maxDepth);
      for (int y = h + 1; y <= h + depth; ++y) {
        ctx.blocks.set(x, y, z, s_.water);
        ctx.blocks.setState(x, y, z, kStillWaterLevel);
      }
      // maps.height stays the highest SOLID y: the surface rules already ran, and the
      // shoreline rule keys off the true floor.
    }
  }
}

// ---- shoreline

ShorelineRule::ShorelineRule(Settings s, OceanSettings ocean, bool hasOcean, LakeSettings lake, bool hasLake)
    : s_(std::move(s)),
      ocean_(std::move(ocean)),
      hasOcean_(hasOcean),
      lake_(std::move(lake)),
      hasLake_(hasLake) {
  if (s_.band < 0) s_.band = 0;
  if (s_.depth < 0) s_.depth = 0;
  if (hasLake_) {
    // Normalize the same way LakeFillRule does, so the mask test here reproduces the
    // fill rule's field exactly.
    lake_.maxDepth = std::max(1, lake_.maxDepth);
    lake_.threshold = std::clamp(lake_.threshold, 0.f, 1.f);
    if (!(lake_.frequency > 0.f) || !std::isfinite(lake_.frequency)) lake_.frequency = 1.f / 64.f;
    lakeMask_ = makeWaterNoise(lake_.seed, "litholattice.procgen.lake");
  }
}

void ShorelineRule::apply(StageContext& ctx) const {
  if (s_.perBiome.empty() || (!hasOcean_ && !hasLake_)) return;
  const int n = ctx.chunk.size, top = ctx.blocks.height() - 1;
  const size_t last = s_.perBiome.size() - 1;
  const int sea = hasOcean_ ? clampLevel(ocean_.seaLevel, top) : 0;
  const int wt = hasLake_ ? clampLevel(lake_.waterTable, top) : 0;
  for (int z = 0; z < n; ++z) {
    for (int x = 0; x < n; ++x) {
      const size_t i = size_t(z * n + x);
      const int h = ctx.maps.height[i];
      if (h < 0 || h > top) continue;
      const size_t dom = std::min<size_t>(ctx.maps.biome[i], last);
      const SurfaceLayers& sl = s_.perBiome[dom];
      if (sl.top == kAirMaterial && sl.filler == kAirMaterial) continue;
      bool shore = false;
      if (hasOcean_ && blendedDensity(ocean_.biomes, ctx.maps, i) > 0.f) {
        if (h >= sea - s_.depth && h <= sea + s_.band) shore = true;
      }
      if (!shore && hasLake_) {
        const float w = blendedDensity(lake_.biomes, ctx.maps, i);
        if (w > 0.f && h < wt) {
          const int wx = ctx.chunk.worldX(x), wz = ctx.chunk.worldZ(z);
          const float m01 =
              (lakeMask_->sample2(float(wx) * lake_.frequency, float(wz) * lake_.frequency) + 1.f) * 0.5f;
          const float effThreshold = 1.f - w * (1.f - lake_.threshold);
          if (m01 > effThreshold && h >= wt - s_.depth && h <= wt + s_.band) shore = true;
        }
      }
      if (!shore) continue;
      // Rewrite the solid top only: water cells above h are never touched.
      if (sl.top != kAirMaterial) ctx.blocks.set(x, h, z, sl.top);
      if (sl.filler != kAirMaterial && sl.fillerDepth > 0) {
        for (int y = h - 1, c = 0; y >= 0 && c < sl.fillerDepth; --y, ++c) {
          if (ctx.blocks.get(x, y, z) == kAirMaterial) break;  // don't fill air pockets
          ctx.blocks.set(x, y, z, sl.filler);
        }
      }
    }
  }
}

}  // namespace litholattice::procgen
