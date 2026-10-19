// Silica per-biome shade cache + scatter density (workstream 5). LF line endings.
#include "lithokraft/game/silica_biome.h"

#include <array>
#include <mutex>
#include <unordered_map>
#include "lithokraft/game/wafer_biomes.h"
#include "litholattice/procgen/pipeline.h"
#include "litholattice/world/column_generator.h"

namespace lithokraft {
namespace {

int64_t chunkKey(int cx, int cz) {
  return (int64_t(uint32_t(cx)) << 32) | uint64_t(uint32_t(cz));
}

}  // namespace

struct SilicaShadeCache::Impl {
  std::shared_ptr<const litholattice::ColumnGenerator> gen;
  std::mutex mutex;
  std::unordered_map<int64_t, SilicaShade> cache;  // chunkKey -> blended shade

  SilicaShade blend(int cx, int cz) {
    // The sanctioned sibling query: regenerate the chunk's column maps
    // (deterministic: same seed -> same weights) and average the per-column
    // weight vectors over the chunk. Cache miss only.
    litholattice::procgen::ColumnMaps maps;
    if (gen) gen->generate(cx, cz, &maps);
    float w[kWaferBiomeCount]{};
    const size_t n = maps.biome.size();
    if (n > 0) {
      const float inv = 1.f / float(n);
      for (size_t i = 0; i < n; ++i)
        for (int b = 0; b < kWaferBiomeCount; ++b)
          w[b] += biomeWeightAt(maps, i, WaferBiome(b)) * inv;
    }
    // No maps at all (n == 0): weights stay zero -> silicaShadeForWeights falls
    // back to the untinted default at density 0 (nothing scatters, nothing tints).
    return silicaShadeForWeights(w, kWaferBiomeCount);
  }
};

SilicaShadeCache::SilicaShadeCache(std::shared_ptr<const litholattice::ColumnGenerator> waferGen)
    : impl_(std::make_unique<Impl>()) {
  impl_->gen = std::move(waferGen);
}

SilicaShadeCache::~SilicaShadeCache() = default;

SilicaShade SilicaShadeCache::shade(int cx, int cz) {
  const int64_t k = chunkKey(cx, cz);
  bool hit = false;
  SilicaShade v{};
  {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    if (const auto it = impl_->cache.find(k); it != impl_->cache.end()) {
      v = it->second;
      hit = true;
    }
  }
  if (!hit) {
    // Cache miss: blend without holding the lock; a duplicate miss on another
    // worker just blends twice and emplaces once (emplace keeps the first).
    const SilicaShade fresh = impl_->blend(cx, cz);
    std::lock_guard<std::mutex> lk(impl_->mutex);
    v = impl_->cache.emplace(k, fresh).first->second;
  }
  return v;
}

float silicaScatterDensity(const litholattice::procgen::ColumnMaps& maps, size_t column) {
  float w[kWaferBiomeCount]{};
  for (int b = 0; b < kWaferBiomeCount; ++b) w[b] = biomeWeightAt(maps, column, WaferBiome(b));
  return silicaShadeForWeights(w, kWaferBiomeCount).density;
}

}  // namespace lithokraft
