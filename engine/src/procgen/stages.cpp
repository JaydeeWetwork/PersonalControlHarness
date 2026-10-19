#include "litholattice/procgen/stages.h"
#include "litholattice/procgen/litho_formula.h"
#include "litholattice/procgen/modifiers.h"
#include "litholattice/procgen/noise.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace litholattice::procgen {

// ---- climate

GraphClimate::GraphClimate(std::shared_ptr<const NoiseGraph> graph, std::array<int, kClimateParamCount> outputs, float fallback)
    : graph_(std::move(graph)), outputs_(outputs), fallback_(fallback) {}

void GraphClimate::sample(StageContext& ctx) const {
  std::vector<float> values(ctx.columns.size());
  for (size_t p = 0; p < kClimateParamCount; ++p) {
    if (outputs_[p] < 0) {
      for (ClimatePoint& c : ctx.maps.climate) c[p] = fallback_;
      continue;
    }
    graph_->evaluate(outputs_[p], ctx.graph, values);
    for (size_t i = 0; i < values.size(); ++i) ctx.maps.climate[i][p] = values[i];
  }
}

namespace {
/// Weighted squared climate-space distance (the multi-noise metric).
float climateDist2(const BiomePoint& b, const ClimatePoint& c) {
  float d = 0;
  for (size_t p = 0; p < kClimateParamCount; ++p) {
    const float diff = c[p] - b.point[p];
    d += b.weight[p] * diff * diff;
  }
  return d;
}
/// Deterministic uniform [0, 1) from a column position and salt (palette dither).
float hash01(int x, int z, uint64_t salt) {
  const uint64_t h =
      mix64(uint64_t(uint32_t(x)) | (uint64_t(uint32_t(z)) << 32) ^ mix64(salt ^ 0x9E3779B97F4A7C15ull));
  return float(h >> 40) * (1.0f / 16777216.0f);
}
}  // namespace

// ---- biomes

MultiNoiseBiomeSelector::MultiNoiseBiomeSelector(std::vector<BiomePoint> biomes) : biomes_(std::move(biomes)) {}

uint8_t MultiNoiseBiomeSelector::select(const ClimatePoint& climate) const {
  size_t best = 0;
  float bestDist = std::numeric_limits<float>::max();
  for (size_t b = 0; b < biomes_.size() && b < 256; ++b) {
    const float d = climateDist2(biomes_[b], climate);
    if (d < bestDist) {
      bestDist = d;
      best = b;
    }
  }
  return uint8_t(best);
}

BlendedBiomeSelector::BlendedBiomeSelector(std::vector<BiomePoint> biomes, float blendRadius, int blendCount)
    : biomes_(std::move(biomes)), blendRadius_(blendRadius), blendCount_(blendCount) {}

uint8_t BlendedBiomeSelector::select(const ClimatePoint& climate) const {
  // Dominant = nearest; matches the argmax of blendWeights (kernel decreases with distance).
  size_t best = 0;
  float bestDist = std::numeric_limits<float>::max();
  for (size_t b = 0; b < biomes_.size() && b < 256; ++b) {
    const float d = climateDist2(biomes_[b], climate);
    if (d < bestDist) {
      bestDist = d;
      best = b;
    }
  }
  return uint8_t(best);
}

void BlendedBiomeSelector::blendWeights(const ClimatePoint& climate, float* out) const {
  const size_t nb = biomes_.size();
  for (size_t b = 0; b < nb; ++b) out[b] = 0.f;
  if (nb == 0 || blendRadius_ <= 0.f) return;
  // blendCount nearest by (distance, index); the index tiebreak keeps it deterministic.
  std::vector<size_t> order(nb);
  std::vector<float> dist(nb);
  for (size_t b = 0; b < nb; ++b) {
    order[b] = b;
    dist[b] = climateDist2(biomes_[b], climate);
  }
  const int keep = std::max(1, std::min<int>(blendCount_, int(nb)));
  std::partial_sort(order.begin(), order.begin() + keep, order.end(), [&](size_t a, size_t c) {
    return dist[a] < dist[c] || (dist[a] == dist[c] && a < c);
  });
  // Softmax over the gap to the nearest kept biome: the nearest always has weight 1
  // pre-normalization, the rest fade with their distance gap. Smooth everywhere -- biome
  // edges are always gradients, even far from every biome point (no hard fallback).
  // blend_radius is the softness scale for the gap (smaller = purer interiors).
  const float invT = 2.f / (blendRadius_ * blendRadius_);
  const float dmin = dist[order[0]];
  float sum = 0.f;
  for (int k = 0; k < keep; ++k) {
    const float w = std::exp(-(dist[order[size_t(k)]] - dmin) * invT);
    out[order[size_t(k)]] = w;
    sum += w;
  }
  if (sum > 0.f)
    for (size_t b = 0; b < nb; ++b) out[b] /= sum;
}

// ---- terrain

HeightmapTerrain::HeightmapTerrain(std::shared_ptr<const NoiseGraph> graph, int output, MaterialId material, int depth)
    : graph_(std::move(graph)), output_(output), material_(material), depth_(depth) {}

void HeightmapTerrain::generate(StageContext& ctx) const {
  std::vector<float> h(ctx.columns.size());
  graph_->evaluate(output_, ctx.graph, h);
  const int n = ctx.chunk.size, top = ctx.blocks.height() - 1;
  for (int z = 0; z < n; ++z)
    for (int x = 0; x < n; ++x) {
      const size_t i = size_t(z * n + x);
      const float v = std::isfinite(h[i]) ? std::floor(h[i]) : -1.f;
      const int surface = int(std::clamp(v, -1.f, float(top)));
      const int bottom = depth_ > 0 ? std::max(0, surface - depth_ + 1) : 0;
      for (int y = bottom; y <= surface; ++y) ctx.blocks.set(x, y, z, material_);
      ctx.maps.height[i] = surface;
    }
}

DensityTerrain::DensityTerrain(std::shared_ptr<const NoiseGraph> graph, int output, MaterialId material, int minY, int maxY)
    : graph_(std::move(graph)), output_(output), material_(material), minY_(minY), maxY_(maxY) {}

void DensityTerrain::generate(StageContext& ctx) const {
  const int n = ctx.chunk.size, top = std::min(maxY_, ctx.blocks.height() - 1), lo = std::max(0, minY_);
  const size_t columns = ctx.columns.size();
  std::vector<float> ys(columns), density(columns);
  GraphContext layerCtx;  // own context: ctx.graph stays bound to the columns
  for (int y = 0; y <= top; ++y) {
    if (y < lo) {
      for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) ctx.blocks.set(x, y, z, material_);
    } else {
      std::fill(ys.begin(), ys.end(), float(y));
      layerCtx.bind({ctx.columns.x, ys, ctx.columns.z, ctx.columns.w});
      graph_->evaluate(output_, layerCtx, density);
      for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
          if (density[size_t(z * n + x)] > 0.f) ctx.blocks.set(x, y, z, material_);
    }
    for (int z = 0; z < n; ++z)
      for (int x = 0; x < n; ++x)
        if (ctx.blocks.get(x, y, z) != kAirMaterial) ctx.maps.height[size_t(z * n + x)] = y;
  }
}

BiomeHeightmapTerrain::BiomeHeightmapTerrain(std::shared_ptr<const NoiseGraph> graph, std::vector<Entry> perBiome,
                                               int depth)
    : graph_(std::move(graph)), perBiome_(std::move(perBiome)), depth_(depth) {}

void BiomeHeightmapTerrain::generate(StageContext& ctx) const {
  if (perBiome_.empty()) return;
  const size_t columns = ctx.columns.size();
  // Evaluate each distinct height/uplift output once; several biomes may share one.
  // Uplift outputs are interned into the same table (kNoUplift marks entries without one).
  constexpr size_t kNoUplift = size_t(-1);
  std::vector<int> distinct;
  std::vector<size_t> slotBase(perBiome_.size()), slotUplift(perBiome_.size(), kNoUplift);
  const auto intern = [&](int output) {
    const auto it = std::find(distinct.begin(), distinct.end(), output);
    if (it == distinct.end()) {
      distinct.push_back(output);
      return distinct.size() - 1;
    }
    return size_t(it - distinct.begin());
  };
  for (size_t b = 0; b < perBiome_.size(); ++b) {
    slotBase[b] = intern(perBiome_[b].output);
    if (perBiome_[b].upliftOutput >= 0) slotUplift[b] = intern(perBiome_[b].upliftOutput);
  }
  std::vector<std::vector<float>> h(distinct.size(), std::vector<float>(columns));
  for (size_t d = 0; d < distinct.size(); ++d) graph_->evaluate(distinct[d], ctx.graph, h[d]);
  const int n = ctx.chunk.size, top = ctx.blocks.height() - 1;
  const size_t last = perBiome_.size() - 1;
  const size_t nb = std::min(ctx.maps.biomeCount, perBiome_.size());
  const bool blended = nb > 0 && ctx.maps.biomeWeight.size() >= size_t(n) * size_t(n) * nb;
  const auto biomeHeight = [&](size_t b, size_t i) {
    float v = h[slotBase[b]][i];
    if (slotUplift[b] != kNoUplift) v += perBiome_[b].upliftAmplitude * h[slotUplift[b]][i];
    return v;
  };
  for (int z = 0; z < n; ++z)
    for (int x = 0; x < n; ++x) {
      const size_t i = size_t(z * n + x);
      const size_t dom = std::min<size_t>(ctx.maps.biome[i], last);
      // Biome blending: the height is the weight-averaged per-biome height, so biome
      // edges read as gradients. One-hot (unblended) weights reduce to the dominant height.
      float hv = biomeHeight(dom, i);
      if (blended) {
        const float* w = ctx.maps.biomeWeight.data() + i * nb;
        float hsum = 0.f, wsum = 0.f;
        for (size_t b = 0; b < nb; ++b)
          if (w[b] > 0.f) {
            hsum += w[b] * biomeHeight(b, i);
            wsum += w[b];
          }
        if (wsum > 0.f) hv = hsum / wsum;
      }
      const float fv = std::isfinite(hv) ? std::floor(hv) : -1.f;
      const int surface = int(std::clamp(fv, -1.f, float(top)));
      const int bottom = depth_ > 0 ? std::max(0, surface - depth_ + 1) : 0;
      const MaterialId m = perBiome_[dom].material;  // crust material: dominant biome
      for (int y = bottom; y <= surface; ++y) ctx.blocks.set(x, y, z, m);
      ctx.maps.height[i] = surface;
    }
}

// ---- surface

LayeredSurface::LayeredSurface(std::vector<SurfaceLayers> perBiome, bool topStateIsBiome, uint64_t ditherSeed)
    : perBiome_(std::move(perBiome)), topStateIsBiome_(topStateIsBiome), ditherSeed_(ditherSeed) {}

void LayeredSurface::apply(StageContext& ctx) const {
  if (perBiome_.empty()) return;
  const int n = ctx.chunk.size;
  for (int z = 0; z < n; ++z)
    for (int x = 0; x < n; ++x) {
      const size_t i = size_t(z * n + x);
      const int h = ctx.maps.height[i];
      if (h < 0) continue;
      uint8_t biome = ctx.maps.biome[i];
      // Weight-proportioned palette dither: with mixed biome weights the top palette is
      // picked by a seeded hash of the column position, so the boundary is a noisy
      // gradient rather than a line. One-hot weights always pick the dominant biome.
      const size_t nb = std::min(ctx.maps.biomeCount, perBiome_.size());
      if (nb > 1 && ctx.maps.biomeWeight.size() >= (i + 1) * nb) {
        const float* w = ctx.maps.biomeWeight.data() + i * nb;
        int mixed = 0;
        for (size_t b = 0; b < nb; ++b) mixed += (w[b] > 0.f) ? 1 : 0;
        if (mixed > 1) {
          const float t = hash01(ctx.chunk.worldX(x), ctx.chunk.worldZ(z), ditherSeed_);
          float acc = 0.f;
          for (size_t b = 0; b < nb; ++b) {
            acc += w[b];
            if (t < acc) {
              biome = uint8_t(b);
              break;
            }
          }
        }
      }
      const SurfaceLayers& l = perBiome_[std::min<size_t>(biome, perBiome_.size() - 1)];
      if (l.top != kAirMaterial) ctx.blocks.set(x, h, z, l.top);
      if (topStateIsBiome_) ctx.blocks.setState(x, h, z, biome);
      for (int y = h - 1; y >= std::max(0, h - l.fillerDepth); --y) {
        if (ctx.blocks.get(x, y, z) == kAirMaterial) break;
        if (l.filler != kAirMaterial) ctx.blocks.set(x, y, z, l.filler);
      }
    }
}

// ---- carvers

namespace {
// ---- carver helpers

/// Floor division for possibly-negative world coordinates.
int carveFloorDiv(int a, int b) {
  const int q = a / b, r = a % b;
  return r < 0 ? q - 1 : q;
}

/// Seed for one coarse lattice cell: a pure function of (featureSeed, ccx, ccz).
uint64_t carveCellSeed(uint64_t featSeed, int32_t ccx, int32_t ccz) {
  return deriveSeed(featSeed, mix64((uint64_t(uint32_t(ccx)) << 32) | uint64_t(uint32_t(ccz))));
}

/// Smooth trilinear value noise on the integer lattice: a pure function of (seed, position),
/// no state, thread-safe. CavernCarver uses it for wobbly chamber walls.
float carveNoise3(uint64_t seed, float x, float y, float z) {
  const int xi = int(std::floor(x)), yi = int(std::floor(y)), zi = int(std::floor(z));
  const float xf = x - float(xi), yf = y - float(yi), zf = z - float(zi);
  const float u = xf * xf * (3.f - 2.f * xf), v = yf * yf * (3.f - 2.f * yf), w = zf * zf * (3.f - 2.f * zf);
  float c[8];
  for (int i = 0; i < 8; ++i)
    c[i] = signedUnitFloat(latticeHash(seed, xi + (i & 1), yi + ((i >> 1) & 1), zi + ((i >> 2) & 1)));
  const float x00 = c[0] + u * (c[1] - c[0]), x10 = c[2] + u * (c[3] - c[2]);
  const float x01 = c[4] + u * (c[5] - c[4]), x11 = c[6] + u * (c[7] - c[6]);
  const float y0 = x00 + v * (x10 - x00), y1 = x01 + v * (x11 - x01);
  return y0 + w * (y1 - y0);
}

/// Per-biome density multiplier; missing entries read as 1.
float carveDensityAt(const std::vector<float>& density, uint8_t biome) {
  return biome < density.size() ? density[size_t(biome)] : 1.f;
}

}  // namespace

WormCaveCarver::WormCaveCarver(WormCaveSettings s) : s_(std::move(s)) {
  s_.minLength = std::max(1, s_.minLength);
  s_.maxLength = std::max(s_.minLength, s_.maxLength);
  s_.maxRadius = std::max(s_.minRadius, s_.maxRadius);
  s_.chance = std::clamp(s_.chance, 0.f, 1.f);
  s_.branchChance = std::clamp(s_.branchChance, 0.f, 1.f);
  s_.maxBranches = std::max(0, s_.maxBranches);
  s_.branchLengthFrac = std::clamp(s_.branchLengthFrac, 0.05f, 1.f);
  s_.branchRadiusFrac = std::clamp(s_.branchRadiusFrac, 0.05f, 1.f);
}

void WormCaveCarver::onPipelineBuilt(const GeneratorPipeline* pipeline) {
  // Bind the dominant-biome probe to the finished pipeline. The pipeline outlives its
  // carvers, so the raw pointer is safe.
  probe_ = [pipeline](int32_t wx, int32_t wz) { return pipeline->biomeAt(wx, wz); };
}

void WormCaveCarver::carve(StageContext& ctx) const {
  const ChunkInfo& c = ctx.chunk;
  const int n = c.size;
  const int reach = int(std::ceil((float(s_.maxLength) + s_.maxRadius) / float(n)));  // chunks a worm can cross
  const float x0 = float(c.worldX(0)), z0 = float(c.worldZ(0));
  const auto carveSphere = [&](float px, float py, float pz, float r) {
    if (px + r < x0 || px - r >= x0 + float(n) || pz + r < z0 || pz - r >= z0 + float(n)) return;
    const int ylo = std::max(1, int(std::floor(py - r))), yhi = std::min(ctx.blocks.height() - 1, int(std::ceil(py + r)));
    for (int lz = std::max(0, int(std::floor(pz - r - z0))); lz <= std::min(n - 1, int(std::ceil(pz + r - z0))); ++lz)
      for (int lx = std::max(0, int(std::floor(px - r - x0))); lx <= std::min(n - 1, int(std::ceil(px + r - x0))); ++lx) {
        const int limit = ctx.maps.height[size_t(lz * n + lx)] - s_.surfaceMargin;
        const float dx = x0 + float(lx) + 0.5f - px, dz = z0 + float(lz) + 0.5f - pz;
        for (int y = ylo; y <= std::min(yhi, limit); ++y) {
          const float dy = float(y) + 0.5f - py;
          if (dx * dx + dy * dy + dz * dz < r * r) ctx.blocks.set(lx, y, lz, kAirMaterial);
        }
      }
  };
  // One walk of `length` steps; the radius swells mid-walk and pinches at both ends.
  const auto walk = [&](Rng& w, float px, float py, float pz, float yaw, float pitch, int length,
                        float rMin, float rMax) {
    for (int step = 0; step < length; ++step) {
      const float t = float(step) / float(length);
      const float r = rMin + (rMax - rMin) * std::sin(std::numbers::pi_v<float> * t);
      carveSphere(px, py, pz, r);
      px += std::cos(yaw) * std::cos(pitch);
      pz += std::sin(yaw) * std::cos(pitch);
      py += std::sin(pitch);
      yaw += w.range(-0.25f, 0.25f);
      pitch = pitch * 0.85f + w.range(-0.12f, 0.12f);
    }
  };
  for (int sz = c.cz - reach; sz <= c.cz + reach; ++sz)
    for (int sx = c.cx - reach; sx <= c.cx + reach; ++sx) {
      Rng rng(deriveSeed(chunkSeed(c.dimensionSeed, sx, sz), nameHash("worm_cave")));
      if (rng.uniform() >= s_.chance) continue;
      float px = float(sx * n) + rng.range(0.f, float(n)), pz = float(sz * n) + rng.range(0.f, float(n));
      float py = float(rng.range(s_.minY, s_.maxY));
      // Per-biome start gate: a pure function of the worm origin via the probe, so every
      // replaying chunk agrees. Drawn only when the feature is configured (density set and
      // a pipeline bound); otherwise the legacy RNG stream is untouched.
      if (!s_.density.empty() && probe_ &&
          rng.uniform() >= carveDensityAt(s_.density, probe_(int32_t(std::floor(px)), int32_t(std::floor(pz)))))
        continue;
      float yaw = rng.range(0.f, 2.f * std::numbers::pi_v<float>), pitch = rng.range(-0.3f, 0.3f);
      const int length = rng.range(s_.minLength, s_.maxLength);
      Rng walkStart = rng;  // pre-walk state, so branches can rewind to the walk start
      Rng walkRng = rng;    // the walk draws on the copy; rng stays at the parameter position
      walk(walkRng, px, py, pz, yaw, pitch, length, s_.minRadius, s_.maxRadius);
      // Forks: each branch rewinds the copy to the parent's walk start and fast-forwards to
      // its fork step, so the branch starts exactly on the parent's path. Opt-in only:
      // branchChance = 0 keeps the legacy stream identical.
      if (s_.branchChance > 0.f && s_.maxBranches > 0) {
        int forks = 0;
        for (int b = 0; b < s_.maxBranches; ++b) {
          if (rng.uniform() < s_.branchChance) ++forks;
          else break;
        }
        for (int b = 0; b < forks; ++b) {
          const int forkStep = rng.range(0, length - 1);
          Rng w = walkStart;  // rewind to the parent's walk start, then fast-forward
          float bx = px, by = py, bz = pz, byaw = yaw, bpitch = pitch;
          for (int s2 = 0; s2 < forkStep; ++s2) {
            bx += std::cos(byaw) * std::cos(bpitch);
            bz += std::sin(byaw) * std::cos(bpitch);
            by += std::sin(bpitch);
            byaw += w.range(-0.25f, 0.25f);
            bpitch = bpitch * 0.85f + w.range(-0.12f, 0.12f);
          }
          const int blen = std::max(8, int(float(length) * s_.branchLengthFrac));
          const float brMax = s_.maxRadius * s_.branchRadiusFrac;
          walk(w, bx, by, bz, byaw + w.range(-0.9f, 0.9f), bpitch + w.range(-0.3f, 0.3f), blen,
               std::min(s_.minRadius, brMax), brMax);
        }
      }
    }
}

RavineCarver::RavineCarver() : RavineCarver(RavineSettings{}) {}

RavineCarver::RavineCarver(RavineSettings s) : s_(std::move(s)) {
  s_.chance = std::clamp(s_.chance, 0.f, 1.f);
  s_.minLength = std::max(8, s_.minLength);
  s_.maxLength = std::max(s_.minLength, s_.maxLength);
  s_.minWidth = std::max(1.f, s_.minWidth);
  s_.maxWidth = std::max(s_.minWidth, s_.maxWidth);
  s_.minDepth = std::max(2.f, s_.minDepth);
  s_.maxDepth = std::max(s_.minDepth, s_.maxDepth);
  s_.meander = std::clamp(s_.meander, 0.f, 0.5f);
  s_.bottomMargin = std::max(1, s_.bottomMargin);
}

void RavineCarver::onPipelineBuilt(const GeneratorPipeline* pipeline) {
  // Bind the dominant-biome probe to the finished pipeline. The pipeline outlives its
  // carvers, so the raw pointer is safe.
  probe_ = [pipeline](int32_t wx, int32_t wz) { return pipeline->biomeAt(wx, wz); };
}

void RavineCarver::carve(StageContext& ctx) const {
  const ChunkInfo& c = ctx.chunk;
  const int n = c.size, height = ctx.blocks.height();
  // A ravine step moves 1 block, so no carved point is further than maxLength + maxWidth
  // from the origin chunk: this reach covers every ravine that can touch the chunk.
  const int reach = (s_.maxLength + int(std::ceil(s_.maxWidth)) + n - 1) / n;
  const float x0 = float(c.worldX(0)), z0 = float(c.worldZ(0));
  // One trench cross-section around the path point (px, pz): a V-shaped cut `cut` deep at
  // the centerline, tapering linearly to 0 at half-width `hw` -- the sloped walls. Only this
  // chunk's columns are ever written, using each column's own (deterministic) surface.
  const auto carveStep = [&](float px, float pz, float dirX, float dirZ, float hw, float cut) {
    if (cut < 1.f || hw < 0.5f) return;
    if (px + hw < x0 || px - hw >= x0 + float(n) || pz + hw < z0 || pz - hw >= z0 + float(n)) return;
    const int lx0 = std::max(0, int(std::floor(px - hw - x0))), lx1 = std::min(n - 1, int(std::ceil(px + hw - x0)));
    const int lz0 = std::max(0, int(std::floor(pz - hw - z0))), lz1 = std::min(n - 1, int(std::ceil(pz + hw - z0)));
    for (int lz = lz0; lz <= lz1; ++lz)
      for (int lx = lx0; lx <= lx1; ++lx) {
        const float rx = x0 + float(lx) + 0.5f - px, rz = z0 + float(lz) + 0.5f - pz;
        const float d = std::fabs(rx * dirZ - rz * dirX);  // perpendicular distance, unit dir
        if (d >= hw) continue;
        const int surf = ctx.maps.height[size_t(lz * n + lx)];
        if (surf < 1) continue;
        const int cutDepth = int(std::floor(cut * (1.f - d / hw)));
        if (cutDepth < 1) continue;
        const int yBot = std::max(s_.bottomMargin, surf - cutDepth);
        for (int y = yBot; y <= std::min(surf, height - 1); ++y) ctx.blocks.set(lx, y, lz, kAirMaterial);
      }
  };
  for (int sz = c.cz - reach; sz <= c.cz + reach; ++sz)
    for (int sx = c.cx - reach; sx <= c.cx + reach; ++sx) {
      Rng rng(deriveSeed(chunkSeed(c.dimensionSeed, sx, sz), nameHash("ravine")));
      const float ox = float(sx * n) + rng.range(0.f, float(n));
      const float oz = float(sz * n) + rng.range(0.f, float(n));
      // Per-biome start gate: a pure function of the origin via the probe, so every
      // replaying chunk agrees.
      const float gate = (!s_.density.empty() && probe_)
                             ? s_.chance * carveDensityAt(s_.density, probe_(int32_t(std::floor(ox)),
                                                                                 int32_t(std::floor(oz))))
                             : s_.chance;
      if (rng.uniform() >= gate) continue;
      float yaw = rng.range(0.f, 2.f * std::numbers::pi_v<float>);
      const int length = rng.range(s_.minLength, s_.maxLength);
      const float width = rng.range(s_.minWidth, s_.maxWidth);
      const float depth = rng.range(s_.minDepth, s_.maxDepth);
      float px = ox, pz = oz;
      for (int step = 0; step < length; ++step) {
        // Pinched ends, deep middle: the gash swells out of the terrain and sinks back in.
        const float t = (float(step) + 0.5f) / float(length);
        const float taper = std::sin(std::numbers::pi_v<float> * t);
        const float dirX = std::cos(yaw), dirZ = std::sin(yaw);
        carveStep(px, pz, dirX, dirZ, width * std::sqrt(taper), depth * taper);
        px += dirX;
        pz += dirZ;
        yaw += rng.range(-s_.meander, s_.meander);
      }
    }
}

CavernCarver::CavernCarver(CavernSettings s) : s_(std::move(s)) {
  s_.cellSize = std::max(16, s_.cellSize);
  s_.chance = std::clamp(s_.chance, 0.f, 1.f);
  s_.minRadius = std::max(2, s_.minRadius);
  s_.maxRadius = std::max(s_.minRadius, s_.maxRadius);
  s_.squashY = std::clamp(s_.squashY, 0.125f, 1.f);
  s_.minY = std::max(1, s_.minY);
  s_.maxY = std::max(s_.minY, s_.maxY);
  s_.surfaceMargin = std::max(2, s_.surfaceMargin);
  s_.noiseFreq = std::max(1.f / 64.f, s_.noiseFreq);
  s_.coreFrac = std::clamp(s_.coreFrac, 0.f, 1.f);
}

void CavernCarver::onPipelineBuilt(const GeneratorPipeline* pipeline) {
  // Bind the dominant-biome probe to the finished pipeline. The pipeline outlives its
  // carvers, so the raw pointer is safe.
  probe_ = [pipeline](int32_t wx, int32_t wz) { return pipeline->biomeAt(wx, wz); };
}

void CavernCarver::carve(StageContext& ctx) const {
  const ChunkInfo& c = ctx.chunk;
  const int n = c.size, height = ctx.blocks.height();
  const int cell = s_.cellSize, maxR = s_.maxRadius;
  const uint64_t featSeed = deriveSeed(c.dimensionSeed, nameHash("cavern"));
  const uint64_t noiseSeed = deriveSeed(c.dimensionSeed, nameHash("cavern_walls"));
  const int x0 = c.worldX(0), z0 = c.worldZ(0);
  // Cells whose chamber (radius <= maxR) can touch this chunk.
  const int ccx0 = carveFloorDiv(x0 - maxR, cell), ccx1 = carveFloorDiv(x0 + n - 1 + maxR, cell);
  const int ccz0 = carveFloorDiv(z0 - maxR, cell), ccz1 = carveFloorDiv(z0 + n - 1 + maxR, cell);
  const float core2 = s_.coreFrac * s_.coreFrac;
  const float invRim = core2 < 1.f ? 1.f / (1.f - core2) : 0.f;  // 0: pure ellipsoid, no wobble
  for (int ccz = ccz0; ccz <= ccz1; ++ccz)
    for (int ccx = ccx0; ccx <= ccx1; ++ccx) {
      Rng r(carveCellSeed(featSeed, ccx, ccz));
      const float ox = float(ccx * cell) + r.range(0.f, float(cell));
      const float oz = float(ccz * cell) + r.range(0.f, float(cell));
      const float radius = float(r.range(s_.minRadius, s_.maxRadius));
      // Per-biome chamber gate: a pure function of the chamber center via the probe, so
      // every replaying chunk agrees.
      const float gate = (!s_.density.empty() && probe_)
                             ? s_.chance * carveDensityAt(s_.density, probe_(int32_t(std::floor(ox)),
                                                                                 int32_t(std::floor(oz))))
                             : s_.chance;
      if (r.uniform() >= gate) continue;
      const float cy = float(r.range(s_.minY, s_.maxY));
      const float ry = radius * s_.squashY;
      // Chamber bounding box clipped to this chunk; the ellipsoid test below does the rest.
      const int lx0 = std::max(0, int(std::floor(ox - radius)) - x0);
      const int lx1 = std::min(n - 1, int(std::ceil(ox + radius)) - x0);
      const int lz0 = std::max(0, int(std::floor(oz - radius)) - z0);
      const int lz1 = std::min(n - 1, int(std::ceil(oz + radius)) - z0);
      if (lx1 < lx0 || lz1 < lz0) continue;
      const int ylo = std::max(1, int(std::floor(cy - ry)));
      const int yhi = std::min(height - 1, int(std::ceil(cy + ry)));
      const float freq = s_.noiseFreq;
      for (int lz = lz0; lz <= lz1; ++lz)
        for (int lx = lx0; lx <= lx1; ++lx) {
          const float wx = float(x0 + lx) + 0.5f, wz = float(z0 + lz) + 0.5f;
          const float nx = (wx - ox) / radius, nz = (wz - oz) / radius;
          if (nx * nx + nz * nz >= 1.f) continue;
          const int limit = ctx.maps.height[size_t(lz * n + lx)] - s_.surfaceMargin;
          const int yhiC = std::min(yhi, limit);
          for (int y = ylo; y <= yhiC; ++y) {
            const float ny = (float(y) + 0.5f - cy) / ry;
            const float d2 = nx * nx + ny * ny + nz * nz;
            if (d2 >= 1.f) continue;
            bool in = d2 < core2;
            if (!in && invRim > 0.f) {
              // Wobbly rim: the carve boundary is where the noise field crosses a level
              // rising from -1 at the core edge to +1 at the ellipsoid surface.
              const float t = (d2 - core2) * invRim;
              in = carveNoise3(noiseSeed, wx * freq, float(y) * freq, wz * freq) > 2.f * t - 1.f;
            }
            if (in) ctx.blocks.set(lx, y, lz, kAirMaterial);
          }
        }
      if (invRim > 0.f) {
        // The wobble rim is not convex: sweep the chamber box once and dissolve any solid
        // block left fully surrounded by air (no stranded single blocks). Out-of-chunk
        // neighbours count as solid, so the sweep never touches another chunk's blocks, and
        // the sweep stays within the surface margin like the carve itself.
        const int sx0 = std::max(0, lx0 - 1), sx1 = std::min(n - 1, lx1 + 1);
        const int sz0 = std::max(0, lz0 - 1), sz1 = std::min(n - 1, lz1 + 1);
        constexpr int kD[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (int lz = sz0; lz <= sz1; ++lz)
          for (int lx = sx0; lx <= sx1; ++lx) {
            const int limit = ctx.maps.height[size_t(lz * n + lx)] - s_.surfaceMargin;
            for (int y = std::max(2, ylo); y <= std::min(yhi, limit); ++y) {
              if (ctx.blocks.get(lx, y, lz) == kAirMaterial) continue;
              bool floating = true;
              for (const auto& d : kD) {
                const int qx = lx + d[0], qy = y + d[1], qz = lz + d[2];
                if (qx < 0 || qx >= n || qz < 0 || qz >= n || qy < 0 || qy >= height ||
                    ctx.blocks.get(qx, qy, qz) != kAirMaterial) {
                  floating = false;
                  break;
                }
              }
              if (floating) ctx.blocks.set(lx, y, lz, kAirMaterial);
            }
          }
      }
    }
}

// ---- noise-field carvers: cheese (density field) + spaghetti/noodle (tube family)
// Clean-room, 2026-10-13. Both are pure fields of (dimensionSeed, world position):
// no paths, no replay lists, no neighbour reads -- chunk borders cannot seam.

namespace {

/// One seeded field-noise instance for a field carver. fractal=true wraps the source
/// in a 3-octave FBm (the cheese density character); tube fields use the raw source.
std::shared_ptr<const INoise> makeFieldNoise(uint64_t dimSeed, const char* salt, bool fractal) {
  NoiseSettings ns;
  ns.type = NoiseType::OpenSimplex2S;
  ns.seed = deriveSeed(dimSeed, nameHash(salt));
  std::shared_ptr<const INoise> src = createNoise(ns);
  if (!fractal) return src;
  FractalSettings fs;
  fs.type = FractalType::FBm;
  fs.octaves = 3;
  return createFractal(std::move(src), fs, deriveSeed(dimSeed, nameHash(salt) ^ 0x5DEECE66Dull));
}

/// Per-column density gate for the field carvers: a pure function of the column's
/// world position (position-hash dither against the biome multiplier), so every chunk
/// agrees on its own columns without any origin replay. Empty table = no gating.
bool fieldColumnOk(const std::vector<float>& density, const ColumnMaps& maps, size_t i, int wx, int wz,
                   uint64_t gateSalt) {
  if (density.empty()) return true;
  const float mult = carveDensityAt(density, maps.biome[i]);
  if (mult >= 1.f) return true;
  if (mult <= 0.f) return false;
  return hash01(wx, wz, gateSalt) < mult;
}

void sanitizeTubeSettings(TubeCaveSettings& s, float defaultFreq) {
  s.tubeRadius = std::clamp(s.tubeRadius, 0.02f, 0.5f);
  if (!(s.frequency > 0.f) || !std::isfinite(s.frequency)) s.frequency = defaultFreq;
  s.minY = std::max(1, s.minY);
  s.surfaceMargin = std::max(2, s.surfaceMargin);
}

/// Shared tube-family carve: carve where A^2 + B^2 < tubeRadius^2 for two raw
/// OpenSimplex2S fields keyed by (saltA, saltB).
void carveNoiseTubes(StageContext& ctx, const TubeCaveSettings& s, const char* saltA, const char* saltB,
                     uint64_t gateSalt) {
  const ChunkInfo& c = ctx.chunk;
  const int n = c.size, height = ctx.blocks.height();
  const auto fieldA = makeFieldNoise(c.dimensionSeed, saltA, false);
  const auto fieldB = makeFieldNoise(c.dimensionSeed, saltB, false);
  const float r2 = s.tubeRadius * s.tubeRadius;
  for (int lz = 0; lz < n; ++lz)
    for (int lx = 0; lx < n; ++lx) {
      const size_t i = size_t(lz * n + lx);
      const int wx = c.worldX(lx), wz = c.worldZ(lz);
      if (!fieldColumnOk(s.density, ctx.maps, i, wx, wz, gateSalt)) continue;
      const int top = std::min(ctx.maps.height[i] - s.surfaceMargin, height - 1);
      if (top < s.minY) continue;
      const float fx = float(wx) * s.frequency, fz = float(wz) * s.frequency;
      for (int y = s.minY; y <= top; ++y) {
        if (ctx.blocks.get(lx, y, lz) == kAirMaterial) continue;
        const float fy = float(y) * s.frequency;
        const float a = fieldA->sample3(fx, fy, fz);
        const float b = fieldB->sample3(fx, fy, fz);
        if (a * a + b * b < r2) ctx.blocks.set(lx, y, lz, kAirMaterial);
      }
    }
}

}  // namespace

CheeseCarver::CheeseCarver(CheeseSettings s) : s_(std::move(s)) {
  s_.threshold = std::clamp(s_.threshold, -0.95f, 0.95f);
  if (!(s_.frequency > 0.f) || !std::isfinite(s_.frequency)) s_.frequency = 1.f / 24.f;
  s_.minY = std::max(1, s_.minY);
  s_.surfaceMargin = std::max(2, s_.surfaceMargin);
}

void CheeseCarver::onPipelineBuilt(const GeneratorPipeline* pipeline) {
  probe_ = [pipeline](int32_t wx, int32_t wz) { return pipeline->biomeAt(wx, wz); };
}

void CheeseCarver::carve(StageContext& ctx) const {
  const ChunkInfo& c = ctx.chunk;
  const int n = c.size, height = ctx.blocks.height();
  const auto field = makeFieldNoise(c.dimensionSeed, "cheese", true);
  const uint64_t gateSalt = deriveSeed(c.dimensionSeed, nameHash("cheese_gate"));
  for (int lz = 0; lz < n; ++lz)
    for (int lx = 0; lx < n; ++lx) {
      const size_t i = size_t(lz * n + lx);
      const int wx = c.worldX(lx), wz = c.worldZ(lz);
      if (!fieldColumnOk(s_.density, ctx.maps, i, wx, wz, gateSalt)) continue;
      const int top = std::min(ctx.maps.height[i] - s_.surfaceMargin, height - 1);
      if (top < s_.minY) continue;
      const float fx = float(wx) * s_.frequency, fz = float(wz) * s_.frequency;
      for (int y = s_.minY; y <= top; ++y) {
        if (ctx.blocks.get(lx, y, lz) == kAirMaterial) continue;
        // Carve where the density field crosses the threshold; the rock the field
        // leaves below threshold is the pillar structure of the cheese.
        if (field->sample3(fx, float(y) * s_.frequency, fz) > s_.threshold)
          ctx.blocks.set(lx, y, lz, kAirMaterial);
      }
    }
}

SpaghettiCarver::SpaghettiCarver() : SpaghettiCarver(TubeCaveSettings{}) {}

SpaghettiCarver::SpaghettiCarver(TubeCaveSettings s) : s_(std::move(s)) { sanitizeTubeSettings(s_, 1.f / 20.f); }

void SpaghettiCarver::onPipelineBuilt(const GeneratorPipeline* pipeline) {
  probe_ = [pipeline](int32_t wx, int32_t wz) { return pipeline->biomeAt(wx, wz); };
}

void SpaghettiCarver::carve(StageContext& ctx) const {
  carveNoiseTubes(ctx, s_, "spaghetti_a", "spaghetti_b",
                  deriveSeed(ctx.chunk.dimensionSeed, nameHash("spaghetti_gate")));
}

NoodleCarver::NoodleCarver() : NoodleCarver([] {
  TubeCaveSettings s;
  s.tubeRadius = 0.125f;      // tighter than spaghetti's 0.14
  s.frequency = 1.f / 13.f;   // higher-frequency wander than spaghetti's 1/20
  return s;
}()) {}

NoodleCarver::NoodleCarver(TubeCaveSettings s) : s_(std::move(s)) { sanitizeTubeSettings(s_, 1.f / 13.f); }

void NoodleCarver::onPipelineBuilt(const GeneratorPipeline* pipeline) {
  probe_ = [pipeline](int32_t wx, int32_t wz) { return pipeline->biomeAt(wx, wz); };
}

void NoodleCarver::carve(StageContext& ctx) const {
  carveNoiseTubes(ctx, s_, "noodle_a", "noodle_b", deriveSeed(ctx.chunk.dimensionSeed, nameHash("noodle_gate")));
}

// ---- underground ruins

RuinsCarver::RuinsCarver(RuinsSettings s) : s_(std::move(s)) {
  s_.depthMin = std::max(1, s_.depthMin);
  s_.depthMax = std::max(s_.depthMin, s_.depthMax);
  s_.surfaceMargin = std::max(2, s_.surfaceMargin);
  s_.traceMinLen = std::max(1, s_.traceMinLen);
  s_.traceMaxLen = std::max(s_.traceMinLen, s_.traceMaxLen);
  s_.tracesPerChunk = std::max(0, s_.tracesPerChunk);
  s_.viasPerChunk = std::max(0, s_.viasPerChunk);
  s_.viaMinH = std::max(1, s_.viaMinH);
  s_.viaMaxH = std::max(s_.viaMinH, s_.viaMaxH);
  s_.gapChance = std::clamp(s_.gapChance, 0.f, 0.95f);
  s_.voidChance = std::clamp(s_.voidChance, 0.f, 1.f);
  s_.viaTraceChance = std::clamp(s_.viaTraceChance, 0.f, 1.f);
  s_.delamChance = std::clamp(s_.delamChance, 0.f, 1.f);
  s_.delamMinY = std::max(1, s_.delamMinY);
  s_.openRoomsPerChunk = std::max(0, s_.openRoomsPerChunk);
  s_.openMinSize = std::max(1, s_.openMinSize);
  s_.openMaxSize = std::max(s_.openMinSize, s_.openMaxSize);
  s_.openHeight = std::max(1, s_.openHeight);
  s_.rubbleChance = std::clamp(s_.rubbleChance, 0.f, 1.f);
}

void RuinsCarver::carve(StageContext& ctx) const {
  if (s_.trace == kAirMaterial || s_.crust.empty()) return;
  const ChunkInfo& c = ctx.chunk;
  const int n = c.size, height = ctx.blocks.height();
  Rng rng(deriveSeed(c.chunkSeed, nameHash("ruins")));

  const auto densityAt = [&](uint8_t biome) -> float {
    return biome < s_.density.size() ? s_.density[size_t(biome)] : 1.f;
  };
  const auto isCrust = [&](MaterialId m) -> bool {
    return std::find(s_.crust.begin(), s_.crust.end(), m) != s_.crust.end();
  };
  const auto surfaceAt = [&](int lx, int lz) -> int { return ctx.maps.height[size_t(lz * n + lx)]; };
  /// Buried: y stays below the surface margin and above y 1. Surface pokes belong to the
  /// biome surface rule; the ruins carver never breaks the surface.
  const auto buriedY = [&](int lx, int lz, int y) -> bool {
    const int surf = surfaceAt(lx, lz);
    return y >= 2 && y < height && surf > 0 && y <= surf - s_.surfaceMargin;
  };
  /// 1:1 replacement of crust only; returns false when the block was left untouched.
  /// Traces also keep one block of diagonal clearance from other traces (circuits route
  /// orthogonally and keep spacing); the skipped block reads as another broken segment.
  const auto tryPlace = [&](int lx, int lz, int y, MaterialId m, bool clearance) -> bool {
    if (!ctx.blocks.contains(lx, y, lz) || !buriedY(lx, lz, y)) return false;
    if (!isCrust(ctx.blocks.get(lx, y, lz))) return false;
    if (clearance) {
      constexpr int kDiag[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
      for (const auto& d : kDiag) {
        const int qx = lx + d[0], qz = lz + d[1];
        if (ctx.blocks.contains(qx, y, qz) && ctx.blocks.get(qx, y, qz) == s_.trace) return false;
      }
    }
    ctx.blocks.set(lx, y, lz, m);
    return true;
  };

  // Orthogonal (Manhattan) trace route from (sx, sy, sz): axis-aligned segments with
  // right-angle bends only, clamped inside the chunk. Records placed positions in `placed`.
  const auto routeTrace = [&](int sx, int sy, int sz, int len, std::vector<std::array<int, 3>>& placed) {
    int dx = 0, dz = 0;
    switch (rng.range(0, 3)) {
      case 0: dx = 1; break;
      case 1: dx = -1; break;
      case 2: dz = 1; break;
      default: dz = -1; break;
    }
    int x = sx, z = sz, stepsUntilTurn = rng.range(3, 8);
    for (int i = 0; i < len; ++i) {
      if (rng.uniform() >= s_.gapChance && tryPlace(x, sz, sy, s_.trace, true)) {
        const std::array<int, 3> p{x, sy, z};
        if (placed.empty() || placed.back() != p) placed.push_back(p);
      }
      if (--stepsUntilTurn <= 0) {  // right-angle turn, never a reversal
        const bool left = (rng.next() & 1u) == 0;
        const int ndx = left ? -dz : dz, ndz = left ? dx : -dx;
        dx = ndx;
        dz = ndz;
        stepsUntilTurn = rng.range(3, 8);
      }
      int nx = x + dx, nz = z + dz;
      if (nx < 0 || nx >= n || nz < 0 || nz >= n) {
        // Chunk edge: turn instead of leaving, so veins stay chunk-local and
        // generation never reads neighbour chunks.
        const int tdx = -dz, tdz = dx;
        dx = tdx;
        dz = tdz;
        nx = std::clamp(x + dx, 0, n - 1);
        nz = std::clamp(z + dz, 0, n - 1);
        stepsUntilTurn = rng.range(3, 8);
      }
      x = nx;
      z = nz;
    }
  };

  // ---- fossil traces: horizontal dead-trace veins with missing segments
  for (int t = 0; t < s_.tracesPerChunk; ++t) {
    const int lx = rng.range(0, n - 1), lz = rng.range(0, n - 1);
    if (rng.uniform() >= densityAt(ctx.maps.biome[size_t(lz * n + lx)])) continue;
    const int surf = surfaceAt(lx, lz);
    const int y = surf - rng.range(s_.depthMin, s_.depthMax);
    if (y < 2 || y > surf - s_.surfaceMargin) continue;
    std::vector<std::array<int, 3>> placed;
    routeTrace(lx, y, lz, rng.range(s_.traceMinLen, s_.traceMaxLen), placed);
    if (!placed.empty() && rng.uniform() < s_.voidChance) {
      // Collapsed segment: a tiny air void, at most 2x1x1, kept buried by the same margin.
      const auto& p = placed[size_t(rng.range(0, int(placed.size()) - 1))];
      ctx.blocks.set(p[0], p[1], p[2], kAirMaterial);
      constexpr int kDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      const auto& d = kDirs[rng.range(0, 3)];
      const int qx = p[0] + d[0], qz = p[2] + d[1];
      if (ctx.blocks.contains(qx, p[1], qz) && ctx.blocks.get(qx, p[1], qz) == s_.trace)
        ctx.blocks.set(qx, p[1], qz, kAirMaterial);
    }
  }

  // ---- broken via stubs: vertical columns, capped below the surface, never surfacing
  if (s_.via != kAirMaterial) {
    for (int v = 0; v < s_.viasPerChunk; ++v) {
      const int lx = rng.range(0, n - 1), lz = rng.range(0, n - 1);
      if (rng.uniform() >= densityAt(ctx.maps.biome[size_t(lz * n + lx)])) continue;
      const int surf = surfaceAt(lx, lz);
      const int baseY = surf - rng.range(s_.depthMin, s_.depthMax);
      int top = baseY + rng.range(s_.viaMinH, s_.viaMaxH) - 1;
      top = std::min(top, surf - s_.surfaceMargin);  // capped: broken stubs stay buried
      if (baseY < 2 || top < baseY + s_.viaMinH - 1) continue;
      for (int y = baseY; y <= top; ++y) {
        if (!isCrust(ctx.blocks.get(lx, y, lz))) break;  // never float inside caves
        ctx.blocks.set(lx, y, lz, s_.via);
      }
      if (rng.uniform() < s_.viaTraceChance) {
        std::vector<std::array<int, 3>> stub;  // trace connecting at the via base
        routeTrace(lx, baseY, lz, rng.range(4, 10), stub);
      }
    }
  }

  // ---- delaminated layers: thin buried bands where the crust alternates materials
  if (s_.delamA != kAirMaterial && s_.delamB != kAirMaterial && rng.uniform() < s_.delamChance) {
    const int bands = 1 + int(rng.next() & 1u);
    for (int b = 0; b < bands; ++b) {
      const int pcx = rng.range(0, n - 1), pcz = rng.range(0, n - 1);
      const int rad = rng.range(2, 5), thickness = 1 + int(rng.next() & 1u);
      const int bandY = surfaceAt(pcx, pcz) - rng.range(s_.depthMin, s_.depthMax);
      for (int dz = -rad; dz <= rad; ++dz)
        for (int dx = -rad; dx <= rad; ++dx) {
          if (dx * dx + dz * dz > rad * rad) continue;
          const int lx = pcx + dx, lz = pcz + dz;
          if (lx < 0 || lx >= n || lz < 0 || lz >= n) continue;
          for (int dy = 0; dy < thickness; ++dy) {
            const int y = bandY + dy;
            if (y < s_.delamMinY || y >= height || y > surfaceAt(lx, lz) - s_.surfaceMargin) continue;
            if (!isCrust(ctx.blocks.get(lx, y, lz))) continue;
            ctx.blocks.set(lx, y, lz, ((lx + lz) & 1) ? s_.delamA : s_.delamB);
          }
        }
    }
  }

  // ---- open ruined volumes (config-gated; OFF by default)
  // Skipped before any RNG draw when openMode is off, so the default inlay-only
  // stream -- and therefore its output -- is bit-identical to the pre-open-mode
  // carver. When on: stamp a buried template volume, erode its boundary with a
  // noise mask (boundary cells survive where the mask says the ruin held), carve
  // the interior to air, and scatter rubble on the new floor (the floor layer
  // keeps a rubble block instead of air with probability rubbleChance). Volumes
  // are clamped inside the chunk, exactly like the inlay passes.
  if (s_.openMode) {
    const MaterialId rubbleMat = s_.rubble != kAirMaterial ? s_.rubble : s_.trace;
    const uint64_t erosionSeed = deriveSeed(c.chunkSeed, nameHash("ruins_open"));
    for (int r = 0; r < s_.openRoomsPerChunk; ++r) {
      const int lx = rng.range(0, n - 1), lz = rng.range(0, n - 1);
      if (rng.uniform() >= densityAt(ctx.maps.biome[size_t(lz * n + lx)])) continue;
      const int surf = surfaceAt(lx, lz);
      const int baseY = surf - rng.range(s_.depthMin, s_.depthMax);
      const int w = std::min(rng.range(s_.openMinSize, s_.openMaxSize), n);
      const int d = std::min(rng.range(s_.openMinSize, s_.openMaxSize), n);
      const int hh = s_.openHeight;
      if (baseY < 2 || baseY + hh - 1 > surf - s_.surfaceMargin) continue;
      const int x0 = std::clamp(lx - w / 2, 0, n - w), z0 = std::clamp(lz - d / 2, 0, n - d);
      for (int dz = 0; dz < d; ++dz)
        for (int dx = 0; dx < w; ++dx) {
          const int qx = x0 + dx, qz = z0 + dz;
          const int colSurf = surfaceAt(qx, qz);
          for (int dy = 0; dy < hh; ++dy) {
            const int y = baseY + dy;
            if (y < 2 || y >= height || y > colSurf - s_.surfaceMargin) continue;
            if (!isCrust(ctx.blocks.get(qx, y, qz))) continue;
            const bool boundary = dx == 0 || dz == 0 || dx == w - 1 || dz == d - 1 || dy == hh - 1;
            if (boundary) {
              // Noise erosion mask: a boundary cell is carved only where the mask
              // field has eaten it away; elsewhere the ruined wall still stands.
              const float e = carveNoise3(erosionSeed, float(c.worldX(qx)) * 0.35f, float(y) * 0.35f,
                                          float(c.worldZ(qz)) * 0.35f);
              if (e <= -0.1f) continue;
            }
            if (dy == 0 && rng.uniform() < s_.rubbleChance)
              ctx.blocks.set(qx, y, qz, rubbleMat);  // rubble scattered on the new floor
            else
              ctx.blocks.set(qx, y, qz, kAirMaterial);
          }
        }
    }
  }
}

// ---- features

void OreVeinFeature::place(StageContext& ctx, Rng& rng) const {
  const int n = ctx.chunk.size;
  const int lo = std::max(0, s_.minY), hi = std::min(ctx.blocks.height() - 1, s_.maxY);
  if (lo > hi) return;
  for (int v = 0; v < s_.veinsPerChunk; ++v) {
    int x = rng.range(0, n - 1), y = rng.range(lo, hi), z = rng.range(0, n - 1);
    for (int i = 0; i < s_.size; ++i) {
      if (ctx.blocks.get(x, y, z) == s_.replace) ctx.blocks.set(x, y, z, s_.material);
      const int axis = rng.range(0, 2), dir = rng.range(0, 1) ? 1 : -1;
      if (axis == 0) x = std::clamp(x + dir, 0, n - 1);
      else if (axis == 1) y = std::clamp(y + dir, lo, hi);
      else z = std::clamp(z + dir, 0, n - 1);
    }
  }
}


// ---- silicon forest features (Wafer surface flora)

namespace {

// Eight horizontal fork directions for trace branches.
constexpr int kDir8X[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int kDir8Z[8] = {0, 1, 1, 1, 0, -1, -1, -1};

/// Structure seed for one base position: a pure function of (dimensionSeed, feature, x, z).
/// Two chunks computing the same base get the same structure, whatever chunk "owns" the base.
uint64_t posSeed(uint64_t featSeed, int32_t wx, int32_t wz) {
  return deriveSeed(featSeed, mix64((uint64_t(uint32_t(wx)) << 32) | uint64_t(uint32_t(wz))));
}

/// Per-column placement roll in [0, 1): a pure function of (featureSeed, x, z).
float posRoll(uint64_t featSeed, int32_t wx, int32_t wz) { return unitFloat(latticeHash(featSeed, wx, wz)); }

/// Writes m at chunk-local (x, y, z) when inside the chunk and the cell is still air. Features
/// never overwrite terrain or each other: placement happens at chunk-gen time only, so the
/// first writer wins and every chunk resolves overlaps in the same deterministic order.
bool setIntoAir(StageContext& ctx, int x, int y, int z, MaterialId m) {
  if (m == kAirMaterial || !ctx.blocks.contains(x, y, z)) return false;
  if (ctx.blocks.get(x, y, z) != kAirMaterial) return false;
  ctx.blocks.set(x, y, z, m);
  return true;
}

/// Buried-structure writer: replaces whatever is in the cell (terrain carving for
/// ruins with dy < 0). Still chunk-local and bounds-checked; still deterministic
/// (first writer wins in the same feature order as setIntoAir).
bool setOverwrite(StageContext& ctx, int x, int y, int z, MaterialId m) {
  if (m == kAirMaterial || !ctx.blocks.contains(x, y, z)) return false;
  ctx.blocks.set(x, y, z, m);
  return true;
}

/// Candidate structure bases: the chunk's own columns plus a `reach`-wide margin of the
/// neighbours' columns (deterministic overlap). Each chunk replays its neighbours' border
/// structures from the same position-keyed seeds, so cross-boundary structures agree without
/// any neighbour reads at gen time; only in-chunk columns are ever written. Out-of-chunk bases
/// reuse the nearest in-chunk column for biome weights and surface height (exact for the
/// owning chunk). Known approximation: at a simultaneous chunk+biome border the replaying
/// chunk may see a different dominant biome than the owning chunk, so a border structure can
/// be replayed without its in-owner part (e.g. branch stubs without a trunk). Rare, bounded
/// to `reach` columns of the border, and fully deterministic.
/// fn(wx, wz, bx, bz, lx, lz, surfY): world/lcl base coords, clamped column,
/// surface height at the clamped column.
template <typename Fn>
void forBases(StageContext& ctx, int reach, Fn&& fn) {
  const int n = ctx.chunk.size;
  for (int bz = -reach; bz < n + reach; ++bz)
    for (int bx = -reach; bx < n + reach; ++bx) {
      const int lx = std::clamp(bx, 0, n - 1), lz = std::clamp(bz, 0, n - 1);
      fn(ctx.chunk.worldX(bx), ctx.chunk.worldZ(bz), bx, bz, lx, lz, ctx.maps.height[size_t(lz * n + lx)]);
    }
}

}  // namespace

float blendedDensity(const BiomeDensity& densities, const ColumnMaps& maps, size_t column) {
  if (column >= maps.biome.size()) return 0.f;
  const size_t nb = maps.biomeCount;
  if (nb == 0 || maps.biomeWeight.size() < (column + 1) * nb)
    return densities.at(maps.biome[column]);  // no blending data: dominant biome, weight 1
  float d = 0.f;
  const float* w = maps.biomeWeight.data() + column * nb;
  for (size_t b = 0; b < nb && b < 256; ++b) d += densities.density[b] * w[b];
  return d;
}

// ---- superformula trace-tree canopies (M11 variance) -------------------------
// Implemented here because the position-keyed helpers (posSeed) live in this TU.

const char* traceTreeCanopyArchetypeName(TraceTreeCanopyArchetype a) {
  switch (a) {
    case TraceTreeCanopyArchetype::Spire: return "spire";
    case TraceTreeCanopyArchetype::Parasol: return "parasol";
    case TraceTreeCanopyArchetype::Lobed: return "lobed";
    case TraceTreeCanopyArchetype::Round: return "round";
  }
  return "round";
}

TraceTreeCanopySpec traceTreeCanopyFor(uint64_t featSeed, int32_t wx, int32_t wz) {
  using litho_formula::LithoParams;
  // Independent position-keyed stream: canopy jitter never perturbs the
  // trunk/branch Rng (posSeed(featSeed, wx, wz)) consumed in place() below.
  const uint64_t canopySeed = deriveSeed(featSeed, nameHash("trace_tree_canopy"));
  Rng r(posSeed(canopySeed, wx, wz));
  TraceTreeCanopySpec s;
  const float roll = r.uniform();
  LithoParams& th = s.theta;
  LithoParams& ph = s.phi;
  // Defaults shared by all archetypes: near-sphere evaluators, mild a/b jitter.
  th = LithoParams{1.0, 1.0, 0.0, 2.0, 2.0, 2.0};
  ph = LithoParams{1.0, 1.0, 2.0, 2.2, 2.0, 2.0};
  th.a = r.range(0.9f, 1.1f);
  th.b = r.range(0.9f, 1.1f);
  if (roll < 0.25f) {  // spire: pod-like phi taper, stretched vertically
    s.archetype = TraceTreeCanopyArchetype::Spire;
    ph.n1 = r.range(2.6f, 3.4f);
    ph.n2 = r.range(5.0f, 8.0f);
    ph.n3 = ph.n2;
    s.radiusH = 2;
    s.radiusV = r.range(3, 4);
  } else if (roll < 0.50f) {  // parasol: broad and flat
    s.archetype = TraceTreeCanopyArchetype::Parasol;
    ph.n1 = r.range(1.0f, 1.5f);
    ph.n2 = r.range(1.6f, 2.4f);
    ph.n3 = ph.n2;
    s.radiusH = 3;
    s.radiusV = 1;
  } else if (roll < 0.80f) {  // lobed: theta m = 3..5 lobes
    s.archetype = TraceTreeCanopyArchetype::Lobed;
    th.m = double(r.range(3, 5));
    th.n1 = r.range(1.0f, 1.6f);
    th.n2 = r.range(0.9f, 1.3f);
    th.n3 = th.n2;
    th.b = r.range(0.8f, 1.0f);
    s.radiusH = 3;
    s.radiusV = r.range(2, 3);
  } else {  // round: classic crown with scale jitter
    s.archetype = TraceTreeCanopyArchetype::Round;
    th.n1 = r.range(1.8f, 2.6f);
    ph.n1 = r.range(1.9f, 2.5f);
    s.radiusH = r.range(2, 3);
    s.radiusV = r.range(2, 3);
  }
  // Clamp to the documented maxima: reach() and the place() height guard are
  // sized from these constants, so the stamp can never outgrow them.
  s.radiusH = std::clamp(s.radiusH, 1, kTraceTreeCanopyRadiusHMax);
  s.radiusV = std::clamp(s.radiusV, 1, kTraceTreeCanopyRadiusVMax);
  return s;
}

std::vector<TraceTreeCanopyCell> traceTreeCanopyCells(const TraceTreeCanopySpec& spec) {
  std::vector<TraceTreeCanopyCell> out;
  const int rH = std::clamp(spec.radiusH, 1, kTraceTreeCanopyRadiusHMax);
  const int rV = std::clamp(spec.radiusV, 1, kTraceTreeCanopyRadiusVMax);
  // 1. Dense surface sampling (fixed counts: cost and shape are seed-stable).
  //    The spherical product factors: r1(theta) and r2(phi) are evaluated once
  //    per angle (72 + 36 lithoR calls) and combined with exactly the
  //    arithmetic lithoPoint uses, instead of 72*36 full lithoPoint calls.
  constexpr int kNTheta = 72, kNPhi = 36;
  const double pi = std::acos(-1.0);
  std::array<double, kNTheta> r1{}, cosT{}, sinT{};
  std::array<double, kNPhi> r2{}, cosP{}, sinP{};
  for (int i = 0; i < kNTheta; ++i) {
    const double theta = -pi + 2.0 * pi * (i + 0.5) / kNTheta;
    r1[size_t(i)] = litho_formula::lithoR(theta, spec.theta);
    cosT[size_t(i)] = std::cos(theta);
    sinT[size_t(i)] = std::sin(theta);
  }
  for (int j = 0; j < kNPhi; ++j) {
    const double phi = -pi / 2.0 + pi * (j + 0.5) / kNPhi;
    r2[size_t(j)] = litho_formula::lithoR(phi, spec.phi);
    cosP[size_t(j)] = std::cos(phi);
    sinP[size_t(j)] = std::sin(phi);
  }
  std::vector<litho_formula::Vec3> pts;
  pts.reserve(size_t(kNTheta) * kNPhi);
  double maxRadH = 0.0, maxAbsZ = 0.0;
  for (int i = 0; i < kNTheta; ++i)
    for (int j = 0; j < kNPhi; ++j) {
      litho_formula::Vec3 pt;
      pt.x = r1[size_t(i)] * cosT[size_t(i)] * r2[size_t(j)] * cosP[size_t(j)];
      pt.y = r1[size_t(i)] * sinT[size_t(i)] * r2[size_t(j)] * cosP[size_t(j)];
      pt.z = r2[size_t(j)] * sinP[size_t(j)];
      if (!std::isfinite(pt.x + pt.y + pt.z)) continue;
      pts.push_back(pt);
      maxRadH = std::max(maxRadH, std::sqrt(pt.x * pt.x + pt.y * pt.y));
      maxAbsZ = std::max(maxAbsZ, std::fabs(pt.z));
    }
  if (pts.empty() || !(maxRadH > 0.0) || !(maxAbsZ > 0.0)) return out;  // degenerate: no canopy
  // 2. Normalize per axis into the spec's radii (formula x/y -> horizontal,
  //    formula z -> up; same axis mapping as litho_formula_voxelize) and track
  //    each horizontal column's continuous vertical span.
  const double scaleH = rH / maxRadH, scaleV = rV / maxAbsZ;
  const int w = 2 * rH + 1;
  std::vector<double> colLo(size_t(w) * w, 1e30), colHi(size_t(w) * w, -1e30);
  for (const litho_formula::Vec3& pt : pts) {
    const double gx = pt.x * scaleH, gz = pt.y * scaleH, gy = pt.z * scaleV;
    int cx = int(std::lround(gx)), cz = int(std::lround(gz));
    cx = std::clamp(cx, -rH, rH);
    cz = std::clamp(cz, -rH, rH);
    const size_t c = size_t(cz + rH) * w + size_t(cx + rH);
    if (gy < colLo[c]) colLo[c] = gy;
    if (gy > colHi[c]) colHi[c] = gy;
  }
  // 3. Fill each column's span; the top cell of a column is the glowing tip skin.
  //    The skirt below the crown centre is cut at dy = -1: the canopy is a crown,
  //    not a shell around the trunk, and a deep skirt could land in a later-
  //    processed neighbour tree's trunk column (features are first-writer-wins
  //    in base order, so a canopy cell there would eat the neighbour's trunk).
  out.reserve(96);
  for (int cz = -rH; cz <= rH; ++cz)
    for (int cx = -rH; cx <= rH; ++cx) {
      const size_t c = size_t(cz + rH) * w + size_t(cx + rH);
      if (colLo[c] > colHi[c]) continue;  // column never sampled: outside the silhouette
      const int dyLo = std::max(-1, int(std::lround(colLo[c])));
      const int dyHi = int(std::lround(colHi[c]));
      for (int dy = dyLo; dy <= dyHi; ++dy)
        out.push_back(TraceTreeCanopyCell{cx, std::clamp(dy, -rV, rV), cz, dy == dyHi});
    }
  return out;
}

void TraceTreeFeature::place(StageContext& ctx, Rng&) const {
  // The per-chunk rng is intentionally unused: every decision below is a pure function of the
  // world block position, so all chunks and all --seed runs agree exactly.
  const uint64_t featSeed = deriveSeed(ctx.chunk.dimensionSeed, nameHash("trace_tree"));
  const int top = ctx.blocks.height();
  // Trunk + longest branch rising from the top + canopy crown above the trunk
  // (centre at trunkH+1, extending radiusV further up).
  const int maxH = s_.maxTrunkH + s_.maxBranchLen + 1 + kTraceTreeCanopyRadiusVMax;
  const int n = ctx.chunk.size;
  forBases(ctx, reach(),
            [&](int32_t wx, int32_t wz, int bx, int bz, int lx, int lz, int surfY) {
              if (posRoll(featSeed, wx, wz) >= blendedDensity(s_.biomes, ctx.maps, size_t(lz * n + lx))) return;
              if (surfY < 0 || surfY + maxH >= top) return;
              if (s_.surface != kAirMaterial && ctx.blocks.get(lx, surfY, lz) != s_.surface) return;
              Rng r(posSeed(featSeed, wx, wz));
              const int trunkH = r.range(s_.minTrunkH, s_.maxTrunkH);
              const MaterialId tip = r.uniform() < s_.redTipChance ? s_.tipRed : s_.tipBlue;
              for (int i = 1; i <= trunkH; ++i) setIntoAir(ctx, bx, surfY + i, bz, s_.trunk);
              setIntoAir(ctx, bx, surfY + trunkH + 1, bz, tip);  // glowing tip crowning the trunk
              const int nBr = r.range(s_.minBranches, s_.maxBranches);
              int dirs[8] = {0, 1, 2, 3, 4, 5, 6, 7};
              for (int i = 0; i < nBr; ++i) {  // partial Fisher-Yates: nBr distinct directions
                const int j = i + r.range(0, 7 - i);
                const int t = dirs[i];
                dirs[i] = dirs[j];
                dirs[j] = t;
              }
              for (int b = 0; b < nBr; ++b) {
                const int dx = kDir8X[dirs[b]], dz = kDir8Z[dirs[b]];
                const int len = r.range(s_.minBranchLen, s_.maxBranchLen);
                int x = bx, z = bz, y = surfY + r.range(2, std::max(2, trunkH));
                for (int i = 0; i < len; ++i) {
                  x += dx;
                  z += dz;
                  y += r.range(0, 1);  // fork outward and up
                  setIntoAir(ctx, x, y, z, i + 1 == len ? tip : s_.branch);
                }
              }
              // Superformula canopy (M11): a per-instance Litho Formula volume
              // centred on the crown (trunk top + 1, where the crown tip sits).
              // Branch material fills it; each column's top cell takes the tip
              // material, so the red/blue per-tree glow crowns the canopy.
              // Pure function of (featSeed, wx, wz) — see traceTreeCanopyFor.
              const TraceTreeCanopySpec canopy = traceTreeCanopyFor(featSeed, wx, wz);
              const int crownY = surfY + trunkH + 1;
              for (const TraceTreeCanopyCell& cell : traceTreeCanopyCells(canopy))
                setIntoAir(ctx, bx + cell.dx, crownY + cell.dy, bz + cell.dz,
                           cell.tip ? tip : s_.branch);
            });
}

void CapacitorCanFeature::place(StageContext& ctx, Rng&) const {
  const uint64_t featSeed = deriveSeed(ctx.chunk.dimensionSeed, nameHash("capacitor_cans"));
  const int top = ctx.blocks.height();
  const int n = ctx.chunk.size;
  forBases(ctx, reach(),
            [&](int32_t wx, int32_t wz, int bx, int bz, int lx, int lz, int surfY) {
              if (posRoll(featSeed, wx, wz) >= blendedDensity(s_.biomes, ctx.maps, size_t(lz * n + lx))) return;
              if (surfY < 0 || surfY + s_.maxCanH >= top) return;
              if (s_.surface != kAirMaterial && ctx.blocks.get(lx, surfY, lz) != s_.surface) return;
              Rng r(posSeed(featSeed, wx, wz));
              const int cans = r.range(s_.minCans, s_.maxCans);
              bool used[5][5] = {};  // radius-2 cluster grid
              int placed = 0;
              for (int attempt = 0; attempt < cans * 4 && placed < cans; ++attempt) {
                const int dx = r.range(-2, 2), dz = r.range(-2, 2);
                if (used[dx + 2][dz + 2]) continue;
                used[dx + 2][dz + 2] = true;
                ++placed;
                const int h = r.range(s_.minCanH, s_.maxCanH);
                for (int i = 1; i <= h; ++i) setIntoAir(ctx, bx + dx, surfY + i, bz + dz, s_.can);
              }
            });
}

void ResonatorSpireFeature::place(StageContext& ctx, Rng&) const {
  const uint64_t featSeed = deriveSeed(ctx.chunk.dimensionSeed, nameHash("resonator_spire"));
  const int top = ctx.blocks.height();
  const int n = ctx.chunk.size;
  forBases(ctx, reach(),
            [&](int32_t wx, int32_t wz, int bx, int bz, int lx, int lz, int surfY) {
              if (posRoll(featSeed, wx, wz) >= blendedDensity(s_.biomes, ctx.maps, size_t(lz * n + lx))) return;
              if (surfY < 0) return;
              if (s_.surface != kAirMaterial && ctx.blocks.get(lx, surfY, lz) != s_.surface) return;
              Rng r(posSeed(featSeed, wx, wz));
              const int h = r.range(s_.minH, s_.maxH);
              if (surfY + h >= top) return;
              const int baseH = r.range(0, 1) == 1 ? 3 : 0;  // half the spires get a 2x2 footing
              for (int i = 1; i <= h; ++i)
                if (i <= baseH) {
                  setIntoAir(ctx, bx, surfY + i, bz, s_.crystal);
                  setIntoAir(ctx, bx + 1, surfY + i, bz, s_.crystal);
                  setIntoAir(ctx, bx, surfY + i, bz + 1, s_.crystal);
                  setIntoAir(ctx, bx + 1, surfY + i, bz + 1, s_.crystal);
                } else {
                  setIntoAir(ctx, bx, surfY + i, bz, s_.crystal);
                }
            });
}

// ---- structures (above-ground ruins, monuments, ...)

namespace {

/// Floor division for possibly-negative world coordinates (C++ / truncates toward zero).
int floorDiv(int a, int b) {
  const int q = a / b, r = a % b;
  return r < 0 ? q - 1 : q;
}

/// Round to the nearest multiple of `snap` (snap > 0), halves away from zero.
int32_t snapRound(int32_t v, int snap) {
  const int32_t q = v >= 0 ? (v + snap / 2) / snap : (v - snap / 2) / snap;
  return q * snap;
}

/// Rotate (dx, dz) by rot * 90 degrees clockwise.
void rotXZ(int dx, int dz, int rot, int& rx, int& rz) {
  rx = dx;
  rz = dz;
  for (int k = 0; k < (rot & 3); ++k) {
    const int t = rx;
    rx = -rz;
    rz = t;
  }
}

/// Seed for one lattice cell: a pure function of (featureSeed, ccx, ccz).
uint64_t cellSeed(uint64_t featSeed, int32_t ccx, int32_t ccz) {
  const uint64_t key = (uint64_t(uint32_t(ccx)) << 32) | uint64_t(uint32_t(ccz));
  return deriveSeed(featSeed, mix64(key));
}

}  // namespace

struct StructureFeature::Base {
  int32_t bx = 0, bz = 0;  ///< world base position (after snap)
  int rot = 0;             ///< blueprint rotation (0..3)
  float gateRoll = 0.f;    ///< [0,1) roll for the biome gate
  size_t variant = 0;      ///< index into variants_ (rolled by cell seed)
};

namespace {

// Shared constructor logic: normalize settings, measure all variants.
void initStructureFeature(const StructureSettings& s, const std::vector<StructureFeature::Variant>& variants,
                          int& reach, int& maxDy, int& minDy, uint64_t& decaySalt) {
  int extent = 0;
  maxDy = 0;
  minDy = 0;
  for (const auto& v : variants) {
    for (const auto& c : v.cells) {
      extent = std::max(extent, std::max(std::abs(c.dx), std::abs(c.dz)));
      maxDy = std::max(maxDy, c.dy);
      minDy = std::min(minDy, c.dy);
    }
  }
  reach = extent + (s.snap + 1) / 2;  // blueprint extent, plus the snap drift
  decaySalt = deriveSeed(nameHash(s.seedName.empty() ? "structure" : s.seedName),
                         nameHash("structure_decay"));
}

}  // namespace

StructureFeature::StructureFeature(StructureSettings s, std::vector<BlueprintCell> blueprint, BiomeProbe probe)
    : s_(std::move(s)), probe_(std::move(probe)) {
  s_.cellSize = std::max(8, s_.cellSize);
  s_.density = std::clamp(s_.density, 0.f, 1.f);
  s_.snap = std::max(0, s_.snap);
  if (s_.seedName.empty()) s_.seedName = "structure";
  Variant v;
  v.cells = std::move(blueprint);
  v.weight = 1.f;
  variants_.push_back(std::move(v));
  initStructureFeature(s_, variants_, reach_, maxDy_, minDy_, decaySalt_);
}

StructureFeature::StructureFeature(StructureSettings s, std::vector<Variant> variants, BiomeProbe probe)
    : s_(std::move(s)), variants_(std::move(variants)), probe_(std::move(probe)) {
  s_.cellSize = std::max(8, s_.cellSize);
  s_.density = std::clamp(s_.density, 0.f, 1.f);
  s_.snap = std::max(0, s_.snap);
  if (s_.seedName.empty()) s_.seedName = "structure";
  if (variants_.empty()) {
    variants_.push_back(Variant{});
  }
  initStructureFeature(s_, variants_, reach_, maxDy_, minDy_, decaySalt_);
}

uint64_t StructureFeature::featSeed(uint64_t dimensionSeed) const {
  return deriveSeed(dimensionSeed, nameHash(s_.seedName));
}

std::optional<StructureFeature::Base> StructureFeature::baseForCell(uint64_t featSeed, int32_t ccx,
                                                                  int32_t ccz) const {
  Rng r(cellSeed(featSeed, ccx, ccz));
  if (r.uniform() >= s_.density) return std::nullopt;
  int32_t bx = ccx * s_.cellSize + r.range(0, s_.cellSize - 1);
  int32_t bz = ccz * s_.cellSize + r.range(0, s_.cellSize - 1);
  if (s_.snap > 0) {
    bx = snapRound(bx, s_.snap);
    bz = snapRound(bz, s_.snap);
  }
  Base b;
  b.bx = bx;
  b.bz = bz;
  b.rot = s_.rotate ? r.range(0, 3) : 0;
  b.gateRoll = r.uniform();
  // Variant pool: deterministic per-cell weighted roll.
  if (variants_.size() > 1) {
    float total = 0.f;
    for (const auto& v : variants_) total += v.weight;
    float roll = r.uniform() * total;
    for (size_t i = 0; i < variants_.size(); ++i) {
      roll -= variants_[i].weight;
      if (roll <= 0.f) {
        b.variant = i;
        break;
      }
    }
  }
  // Separation: reject when a neighbouring cell's candidate base is closer than
  // s_.spacing. Pure function of (featSeed, cells): neighbours are re-resolved
  // from their own cell seeds, so every chunk agrees. Ties (and mutual conflicts)
  // break deterministically — the cell with the smaller cellSeed wins.
  if (s_.spacing > 0.f) {
    const int cellR = int(std::ceil(s_.spacing / float(s_.cellSize)));
    const uint64_t myPriority = cellSeed(featSeed, ccx, ccz);
    const float spacing2 = s_.spacing * s_.spacing;
    for (int32_t dcz = -cellR; dcz <= cellR; ++dcz) {
      for (int32_t dcx = -cellR; dcx <= cellR; ++dcx) {
        if (dcx == 0 && dcz == 0) continue;
        const int32_t ncx = ccx + dcx, ncz = ccz + dcz;
        // Re-resolve the neighbour's candidate (density roll + position only;
        // gates don't affect separation — conservative and deterministic).
        Rng nr(cellSeed(featSeed, ncx, ncz));
        if (nr.uniform() >= s_.density) continue;
        int32_t nbx = ncx * s_.cellSize + nr.range(0, s_.cellSize - 1);
        int32_t nbz = ncz * s_.cellSize + nr.range(0, s_.cellSize - 1);
        if (s_.snap > 0) {
          nbx = snapRound(nbx, s_.snap);
          nbz = snapRound(nbz, s_.snap);
        }
        const float dx = float(nbx - bx), dz = float(nbz - bz);
        if (dx * dx + dz * dz >= spacing2) continue;
        // Conflict: lower cellSeed wins; tie-break on cell coords (total order).
        const uint64_t nPriority = cellSeed(featSeed, ncx, ncz);
        if (nPriority < myPriority || (nPriority == myPriority && (ncx < ccx || (ncx == ccx && ncz < ccz)))) {
          return std::nullopt;
        }
      }
    }
  }
  return b;
}

void StructureFeature::onPipelineBuilt(const GeneratorPipeline* pipeline) {
  // Bind the dominant-biome probe to the finished pipeline. The pipeline outlives its
  // features, so the raw pointer is safe.
  probe_ = [pipeline](int32_t wx, int32_t wz) { return pipeline->biomeAt(wx, wz); };
}

void StructureFeature::place(StageContext& ctx, Rng&) const {
  // The per-chunk rng is intentionally unused: every decision is a pure function of the
  // world position (cell-keyed seeds), so all chunks and all --seed runs agree exactly.
  if (variants_.empty() || s_.density <= 0.f) return;
  bool anyCells = false;
  for (const auto& v : variants_)
    if (!v.cells.empty()) anyCells = true;
  if (!anyCells) return;
  const uint64_t fs = featSeed(ctx.chunk.dimensionSeed);
  const int n = ctx.chunk.size, top = ctx.blocks.height();
  const int halo = reach_;
  const int wx0 = ctx.chunk.worldX(0), wz0 = ctx.chunk.worldZ(0);
  const int ccx0 = floorDiv(wx0 - halo, s_.cellSize), ccx1 = floorDiv(wx0 + n - 1 + halo, s_.cellSize);
  const int ccz0 = floorDiv(wz0 - halo, s_.cellSize), ccz1 = floorDiv(wz0 + n - 1 + halo, s_.cellSize);
  const size_t ncols = ctx.maps.biome.size();
  // Height lookup with chunk clamping (same approximation as the tree features for
  // margin-replayed bases): deterministic, no neighbour reads.
  const auto heightAt = [&](int32_t wx, int32_t wz) -> int {
    const int lx = std::clamp(int(wx) - wx0, 0, n - 1);
    const int lz = std::clamp(int(wz) - wz0, 0, n - 1);
    const size_t i = size_t(lz * n + lx);
    return i < ctx.maps.height.size() ? ctx.maps.height[i] : -1;
  };
  for (int ccz = ccz0; ccz <= ccz1; ++ccz)
    for (int ccx = ccx0; ccx <= ccx1; ++ccx) {
      const auto base = baseForCell(fs, ccx, ccz);
      if (!base) continue;
      const std::vector<BlueprintCell>& cells =
          base->variant < variants_.size() ? variants_[base->variant].cells : variants_.front().cells;
      if (cells.empty()) continue;
      uint8_t biome = 0;
      if (probe_) {
        biome = probe_(base->bx, base->bz);
      } else {
        // No bound probe (a hand-driven place() outside Builder::build): dominant biome at
        // the clamped column, the same discipline as the tree features.
        const int lx = std::clamp(int(base->bx) - wx0, 0, n - 1);
        const int lz = std::clamp(int(base->bz) - wz0, 0, n - 1);
        const size_t i = size_t(lz * n + lx);
        biome = i < ncols ? ctx.maps.biome[i] : 0;
      }
      if (base->gateRoll >= s_.biomes.at(biome)) continue;
      // Surface height at the (clamped) base column.
      const int surfY = heightAt(base->bx, base->bz);
      // Rule-based gating: height band, slope, water proximity.
      if (surfY < s_.minHeight || surfY > s_.maxHeight) continue;
      if (s_.maxSlope < 1e8f || s_.minSlope > 0.f) {
        // Terrain gradient under the footprint: max |dh| over the reach radius.
        const int r = std::max(1, reach_);
        float maxDh = 0.f;
        const int hC = surfY;
        maxDh = std::max(maxDh, float(std::abs(heightAt(base->bx + r, base->bz) - hC)));
        maxDh = std::max(maxDh, float(std::abs(heightAt(base->bx - r, base->bz) - hC)));
        maxDh = std::max(maxDh, float(std::abs(heightAt(base->bx, base->bz + r) - hC)));
        maxDh = std::max(maxDh, float(std::abs(heightAt(base->bx, base->bz - r) - hC)));
        const float slope = maxDh / float(r);
        if (slope < s_.minSlope || slope > s_.maxSlope) continue;
      }
      if (s_.nearWater && s_.seaLevel > INT_MIN / 2) {
        bool found = false;
        for (int dz = -s_.waterRadius; dz <= s_.waterRadius && !found; ++dz)
          for (int dx = -s_.waterRadius; dx <= s_.waterRadius && !found; ++dx)
            if (heightAt(base->bx + dx, base->bz + dz) < s_.seaLevel) found = true;
        if (!found) continue;
      }
      // Base Y from the anchor mode.
      int baseY = 0;
      switch (s_.anchor) {
        case AnchorMode::Surface: baseY = surfY + 1; break;
        case AnchorMode::FixedY: baseY = s_.fixedY; break;
        case AnchorMode::Floating: baseY = surfY + 1 + s_.hoverHeight; break;
      }
      if (s_.anchor == AnchorMode::Surface) {
        if (surfY < 0 || baseY + maxDy_ >= top) continue;
        const int lx = std::clamp(int(base->bx) - wx0, 0, n - 1);
        const int lz = std::clamp(int(base->bz) - wz0, 0, n - 1);
        const MaterialId topBlock = ctx.blocks.get(lx, surfY, lz);
        if (s_.surface != kAirMaterial) {
          if (topBlock != s_.surface) continue;
        } else if (topBlock == kAirMaterial) {
          continue;
        }
      } else {
        // FixedY / Floating: the footprint must fit in the column range.
        if (baseY + minDy_ < 0 || baseY + maxDy_ >= top) continue;
      }
      const int lbx = int(base->bx) - wx0, lbz = int(base->bz) - wz0;  // chunk-local, maybe out of range
      for (const auto& cell : cells) {
        int rdx, rdz;
        rotXZ(cell.dx, cell.dz, base->rot, rdx, rdz);
        if (cell.decay > 0.f &&
            unitFloat(latticeHash(decaySalt_, base->bx + rdx, 0, base->bz + rdz)) < cell.decay)
          continue;
        const int y = baseY + cell.dy;
        // Buried cells (dy < 0) overwrite terrain; above-surface cells only fill air,
        // preserving the "structures never overwrite terrain" contract for the classic path.
        if (cell.dy < 0) {
          setOverwrite(ctx, lbx + rdx, y, lbz + rdz, cell.material);
        } else {
          setIntoAir(ctx, lbx + rdx, y, lbz + rdz, cell.material);
        }
      }
    }
}

// B-L6 (P3-2): CANDIDATE bases only — see the header. This shares
// baseForCell + the biome gate with place(); place()'s terrain gates
// (height band / slope / near-water / anchor fit / surface material) need
// a height+surface probe this query does not have, so they are NOT
// applied here and the result over-reports by design.
std::vector<std::array<int32_t, 2>> StructureFeature::basesInRect(uint64_t dimensionSeed, int32_t x0,
                                                  int32_t z0, int32_t x1, int32_t z1) const {
  std::vector<std::array<int32_t, 2>> out;
  bool anyCells = false;
  for (const auto& v : variants_)
    if (!v.cells.empty()) anyCells = true;
  if (!probe_ || !anyCells || s_.density <= 0.f || x1 <= x0 || z1 <= z0) return out;
  const uint64_t fs = featSeed(dimensionSeed);
  const int ccx0 = floorDiv(int(x0) - reach_, s_.cellSize), ccx1 = floorDiv(int(x1) - 1 + reach_, s_.cellSize);
  const int ccz0 = floorDiv(int(z0) - reach_, s_.cellSize), ccz1 = floorDiv(int(z1) - 1 + reach_, s_.cellSize);
  for (int32_t ccz = ccz0; ccz <= ccz1; ++ccz)
    for (int32_t ccx = ccx0; ccx <= ccx1; ++ccx) {
      const auto base = baseForCell(fs, ccx, ccz);
      if (!base) continue;
      const uint8_t biome = probe_(base->bx, base->bz);
      if (base->gateRoll >= s_.biomes.at(biome)) continue;
      // Keep the base when its footprint touches the rect.
      if (base->bx + reach_ < x0 || base->bx - reach_ >= x1 || base->bz + reach_ < z0 ||
          base->bz - reach_ >= z1)
        continue;
      out.push_back({base->bx, base->bz});
    }
  return out;
}

// ---- rivers

RiverCarver::RiverCarver(std::shared_ptr<const NoiseGraph> graph, int heightOutput, RiverSettings s)
    : graph_(std::move(graph)), heightOutput_(heightOutput), s_(std::move(s)) {
  s_.cellSize = std::max(16, s_.cellSize);
  s_.chance = std::clamp(s_.chance, 0.f, 1.f);
  s_.maxSteps = std::max(1, s_.maxSteps);
  s_.stepLen = std::max(1, s_.stepLen);
  s_.width = std::max(1.f, s_.width);
  s_.depth = std::max(1.f, s_.depth);
  s_.freeboard = std::clamp(s_.freeboard, 0.f, s_.depth - 0.5f);
  s_.meander = std::max(0.f, s_.meander);
  s_.momentum = std::clamp(s_.momentum, 0.f, 0.95f);
  s_.minSlope = std::max(1e-4f, s_.minSlope);
  for (float& d : s_.density) d = std::clamp(d, 0.f, 1.f);
  // Replay reach: the longest possible walk (each step displaces at most stepLen +
  // meander blocks), plus the carve half-width, in origin cells, plus one cell of margin.
  const float maxLen = float(s_.maxSteps) * (float(s_.stepLen) + s_.meander) + s_.width;
  reach_ = int(std::ceil(maxLen / float(s_.cellSize))) + 1;
}

void RiverCarver::onPipelineBuilt(const GeneratorPipeline* pipeline) {
  // Bind the dominant-biome probe to the finished pipeline (the river spawn density gate).
  // The pipeline outlives its carvers, so the raw pointer is safe. An explicitly set probe
  // (tests, tools) is left alone.
  if (!probe_) probe_ = [pipeline](int32_t wx, int32_t wz) { return pipeline->biomeAt(wx, wz); };
}

std::vector<RiverPoint> RiverCarver::traceRiver(const NoiseGraph& graph, int heightOutput,
                                               uint64_t dimensionSeed, int32_t cellX, int32_t cellZ,
                                               const RiverSettings& s, const BiomeProbe& probe) {
  std::vector<RiverPoint> pts;
  if (heightOutput < 0 || s.water == kAirMaterial || s.chance <= 0.f) return pts;
  Rng rng(deriveSeed(chunkSeed(dimensionSeed, cellX, cellZ), nameHash("river")));
  // One reused graph context for the walk's point samples (rebinding is epoch-safe).
  GraphContext gctx;
  float sx_ = 0.f, sy_ = 0.f, sz_ = 0.f, sw_ = 0.f, sv_ = 0.f;
  const auto H = [&](float x, float z) {
    sx_ = x;
    sz_ = z;
    gctx.bind({{&sx_, 1}, {&sy_, 1}, {&sz_, 1}, {&sw_, 1}});
    graph.evaluate(heightOutput, gctx, {&sv_, 1});
    return sv_;
  };

  // Start at the highest of 4 candidates in the origin cell: rivers begin in the hills.
  // (Comparisons are NaN-safe: a NaN height never wins and never passes the gates.)
  float sx = 0.f, sz = 0.f, sh = -std::numeric_limits<float>::infinity();
  for (int i = 0; i < 4; ++i) {
    const float cx = float(cellX * s.cellSize) + rng.range(0.f, float(s.cellSize));
    const float cz = float(cellZ * s.cellSize) + rng.range(0.f, float(s.cellSize));
    const float h = H(cx, cz);
    if (h > sh) {
      sh = h;
      sx = cx;
      sz = cz;
    }
  }
  if (!(sh >= s.minStartHeight) || !(sh <= s.maxStartHeight)) return pts;
  const uint8_t startBiome = probe ? probe(int32_t(sx), int32_t(sz)) : 0;
  if (rng.uniform() >= s.chance * s.density[startBiome]) return pts;

  const float phase = rng.range(0.f, 2.f * std::numbers::pi_v<float>);
  constexpr float kGradEps = 2.f;  // central-difference step for the routing gradient
  // Uphill deflection retries: straight, then +-30/60/90 degrees off the downhill heading.
  constexpr float kDeflect[7] = {0.f, 0.523599f, -0.523599f, 1.047198f, -1.047198f, 1.570796f, -1.570796f};
  float x = sx, z = sz, dx = 0.f, dz = 0.f;
  bool haveDir = false;
  pts.reserve(size_t(s.maxSteps));
  for (int step = 0; step < s.maxSteps; ++step) {
    const float h = H(x, z);
    if (!(h >= s.waterTable)) break;  // drained into the sea (or a NaN height)
    const float gx = (H(x + kGradEps, z) - H(x - kGradEps, z)) / (2.f * kGradEps);
    const float gz = (H(x, z + kGradEps) - H(x, z - kGradEps)) / (2.f * kGradEps);
    const float m = std::sqrt(gx * gx + gz * gz);
    if (!(m >= s.minSlope)) break;  // flat: the river ends (a lake basin, in game terms)
    float bx = -gx / m, bz = -gz / m;  // downhill heading
    if (haveDir) {
      bx = bx * (1.f - s.momentum) + dx * s.momentum;
      bz = bz * (1.f - s.momentum) + dz * s.momentum;
      const float bm = std::sqrt(bx * bx + bz * bz);
      if (bm > 1e-6f) {
        bx /= bm;
        bz /= bm;
      }
    }
    const float off = s.meander * std::sin(float(step) * s.meanderFreq + phase);
    bool accepted = false;
    float nx = x, nz = z;
    for (float rot : kDeflect) {
      const float c = std::cos(rot), sn = std::sin(rot);
      const float rx = bx * c - bz * sn, rz = bx * sn + bz * c;
      const float tx = x + rx * float(s.stepLen) + (-rz) * off;
      const float tz = z + rz * float(s.stepLen) + rx * off;
      if (H(tx, tz) <= h + s.uphillTol) {
        nx = tx;
        nz = tz;
        accepted = true;
        break;
      }
    }
    if (!accepted) break;  // boxed in by higher ground: the river ends
    pts.push_back({x, z, h});
    const float ddx = nx - x, ddz = nz - z;
    const float dm = std::sqrt(ddx * ddx + ddz * ddz);
    if (dm > 1e-6f) {
      dx = ddx / dm;
      dz = ddz / dm;
      haveDir = true;
    }
    x = nx;
    z = nz;
  }
  return pts;
}

void RiverCarver::carve(StageContext& ctx) const {
  const ChunkInfo& c = ctx.chunk;
  if (s_.water == kAirMaterial || heightOutput_ < 0 || !graph_) return;
  const int n = c.size, height = ctx.blocks.height();
  const float x0 = float(c.worldX(0)), z0 = float(c.worldZ(0));

  // Replay every origin cell whose river could reach this chunk; each chunk writes only
  // its own columns, so neighbours agree without reading each other (chunk-pure).
  std::vector<std::vector<RiverPoint>> rivers;
  const int ccx0 = floorDiv(c.cx * n, s_.cellSize) - reach_;
  const int ccx1 = floorDiv(c.cx * n + n - 1, s_.cellSize) + reach_;
  const int ccz0 = floorDiv(c.cz * n, s_.cellSize) - reach_;
  const int ccz1 = floorDiv(c.cz * n + n - 1, s_.cellSize) + reach_;
  for (int ccz = ccz0; ccz <= ccz1; ++ccz)
    for (int ccx = ccx0; ccx <= ccx1; ++ccx) {
      auto pts = traceRiver(*graph_, heightOutput_, c.dimensionSeed, ccx, ccz, s_, probe_);
      if (!pts.empty()) rivers.push_back(std::move(pts));
    }
  if (rivers.empty()) return;

  // Carve: cosine cross-section (sloped banks), then water to freeboard below the routing
  // height. Water never rises above the column's pre-carve surface: it stays in the channel.
  const float w2 = (s_.width + 0.5f) * (s_.width + 0.5f);
  const float pi = std::numbers::pi_v<float>;
  for (int lz = 0; lz < n; ++lz)
    for (int lx = 0; lx < n; ++lx) {
      const float wx = x0 + float(lx) + 0.5f, wz = z0 + float(lz) + 0.5f;
      float bestD2 = w2, bestH = 0.f;
      for (const auto& pts : rivers)
        for (const auto& p : pts) {
          const float ddx = wx - p.x, ddz = wz - p.z;
          const float d2 = ddx * ddx + ddz * ddz;
          if (d2 < bestD2) {
            bestD2 = d2;
            bestH = p.h;
          }
        }
      if (bestD2 >= w2) continue;
      const size_t col = size_t(lz * n + lx);
      const int oldS = ctx.maps.height[col];
      if (oldS < 1) continue;
      const float t = std::min(1.f, std::sqrt(bestD2) / s_.width);
      const int cutY = int(std::floor(bestH - s_.depth * 0.5f * (1.f + std::cos(pi * t))));
      if (cutY < 0 || cutY >= oldS) continue;  // below the world, or terrain already lower
      for (int y = cutY + 1; y <= oldS; ++y) ctx.blocks.set(lx, y, lz, kAirMaterial);
      if (s_.bed != kAirMaterial && ctx.blocks.get(lx, cutY, lz) != kAirMaterial)
        ctx.blocks.set(lx, cutY, lz, s_.bed);
      const int waterY = std::min({int(std::floor(bestH - s_.freeboard)), oldS, height - 1});
      for (int y = cutY + 1; y <= waterY; ++y) {
        ctx.blocks.set(lx, y, lz, s_.water);
        ctx.blocks.setState(lx, y, lz, 16);  // fluid level 16 = source (fluid.h kMaxFluidLevel)
      }
      ctx.maps.height[col] = cutY;
    }
}

// ---- lithoform stamping (baked .lthm models scattered as world structures) ----

struct LithoStampFeature::Base {
  int32_t bx = 0, bz = 0;  ///< world base position
  int rot = 0;             ///< model rotation (0..3 quarter-turns)
  float gateRoll = 0.f;    ///< [0,1) roll for the biome gate
};

LithoStampFeature::LithoStampFeature(LithoStampSettings s, std::vector<LithoStampVoxel> cells,
                                     BiomeProbe probe)
    : s_(std::move(s)), cells_(std::move(cells)), probe_(std::move(probe)) {
  s_.cellSize = std::max(16, s_.cellSize);
  s_.density = std::clamp(s_.density, 0.f, 1.f);
  if (s_.seedName.empty()) s_.seedName = "lithoform_stamp";
  for (const auto& c : cells_) {
    reach_ = std::max(reach_, std::max(std::abs(c.dx), std::abs(c.dz)));
    minDy_ = std::min(minDy_, c.dy);
    maxDy_ = std::max(maxDy_, c.dy);
  }
}

uint64_t LithoStampFeature::featSeed(uint64_t dimensionSeed) const {
  return deriveSeed(dimensionSeed, nameHash(s_.seedName));
}

std::optional<LithoStampFeature::Base> LithoStampFeature::baseForCell(uint64_t featSeed, int32_t ccx,
                                                                     int32_t ccz) const {
  Rng r(cellSeed(featSeed, ccx, ccz));
  if (r.uniform() >= s_.density) return std::nullopt;
  Base b;
  b.bx = ccx * s_.cellSize + r.range(0, s_.cellSize - 1);
  b.bz = ccz * s_.cellSize + r.range(0, s_.cellSize - 1);
  b.rot = s_.rotate ? r.range(0, 3) : 0;
  b.gateRoll = r.uniform();
  return b;
}

void LithoStampFeature::onPipelineBuilt(const GeneratorPipeline* pipeline) {
  // Same discipline as StructureFeature: the pipeline outlives its features.
  probe_ = [pipeline](int32_t wx, int32_t wz) { return pipeline->biomeAt(wx, wz); };
}

void LithoStampFeature::place(StageContext& ctx, Rng&) const {
  // The per-chunk rng is intentionally unused: every decision is a pure function of the
  // world position (cell-keyed seeds), so all chunks and all --seed runs agree exactly.
  if (cells_.empty() || s_.density <= 0.f) return;
  const uint64_t fs = featSeed(ctx.chunk.dimensionSeed);
  const int n = ctx.chunk.size, top = ctx.blocks.height();
  const int halo = reach_;
  const int wx0 = ctx.chunk.worldX(0), wz0 = ctx.chunk.worldZ(0);
  const int ccx0 = floorDiv(wx0 - halo, s_.cellSize), ccx1 = floorDiv(wx0 + n - 1 + halo, s_.cellSize);
  const int ccz0 = floorDiv(wz0 - halo, s_.cellSize), ccz1 = floorDiv(wz0 + n - 1 + halo, s_.cellSize);
  const size_t ncols = ctx.maps.biome.size();
  // Vertical anchor: the model's lowest cell sits on the surface by default; with
  // anchorCenter the model's vertical midpoint sits at the base instead.
  const int anchorShift = s_.anchorCenter ? (minDy_ + maxDy_) / 2 : minDy_;
  for (int ccz = ccz0; ccz <= ccz1; ++ccz)
    for (int ccx = ccx0; ccx <= ccx1; ++ccx) {
      const auto base = baseForCell(fs, ccx, ccz);
      if (!base) continue;
      uint8_t biome = 0;
      if (probe_) {
        biome = probe_(base->bx, base->bz);
      } else {
        // No bound probe (a hand-driven place() outside Builder::build): dominant biome at
        // the clamped column, the same discipline as the tree features.
        const int lx = std::clamp(int(base->bx) - wx0, 0, n - 1);
        const int lz = std::clamp(int(base->bz) - wz0, 0, n - 1);
        const size_t i = size_t(lz * n + lx);
        biome = i < ncols ? ctx.maps.biome[i] : 0;
      }
      if (base->gateRoll >= s_.biomes.at(biome)) continue;
      // Surface: the clamped column's height map (same approximation as StructureFeature
      // for margin-replayed bases); the model then only writes air.
      const int lx = std::clamp(int(base->bx) - wx0, 0, n - 1);
      const int lz = std::clamp(int(base->bz) - wz0, 0, n - 1);
      const size_t i = size_t(lz * n + lx);
      const int surfY = i < ctx.maps.height.size() ? ctx.maps.height[i] : -1;
      if (surfY < 0) continue;
      const int baseY = surfY + 1 + s_.yOffset - anchorShift;
      if (baseY + minDy_ < 0 || baseY + maxDy_ >= top) continue;
      const int lbx = int(base->bx) - wx0, lbz = int(base->bz) - wz0;  // chunk-local, maybe out of range
      for (const auto& cell : cells_) {
        int rdx, rdz;
        rotXZ(cell.dx, cell.dz, base->rot, rdx, rdz);
        setIntoAir(ctx, lbx + rdx, baseY + cell.dy, lbz + rdz, cell.material);
      }
    }
}

}  // namespace litholattice::procgen
