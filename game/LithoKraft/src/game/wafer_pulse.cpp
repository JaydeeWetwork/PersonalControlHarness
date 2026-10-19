// WaferPulseCache: per-chunk weight-blended ground-pulse params.
// LF line endings.
#include "lithokraft/game/wafer_pulse.h"

#include <array>
#include <mutex>
#include <unordered_map>
#include "lithokraft/game/wafer_biomes.h"
#include "litholattice/render/ground_pulse.h"
#include "litholattice/render/ground_traces.h"
#include "litholattice/world/column_generator.h"
#include "litholattice/procgen/pipeline.h"

namespace lithokraft {
namespace {

// WaferBiome (the generator JSON's "biomes" order) -> GroundPulseBiome (the
// pulse's param table). DeadZones maps to None, whose params are the zero
// vector: it contributes nothing to the blend, so the pulse fades out smoothly
// approaching a Dead Zone instead of cutting off.
constexpr litholattice::GroundPulseBiome pulseBiome(WaferBiome b) {
  switch (b) {
    case WaferBiome::TraceForest: return litholattice::GroundPulseBiome::TraceForest;
    case WaferBiome::DicingPlains: return litholattice::GroundPulseBiome::DicingPlains;
    case WaferBiome::CapacitorGroves: return litholattice::GroundPulseBiome::CapacitorGroves;
    case WaferBiome::ResonatorSpires: return litholattice::GroundPulseBiome::ResonatorSpires;
    case WaferBiome::DeadZones: return litholattice::GroundPulseBiome::None;
  }
  return litholattice::GroundPulseBiome::SiliconFlats;  // unreachable; keeps compilers quiet
}

int64_t chunkKey(int cx, int cz) {
  return (int64_t(uint32_t(cx)) << 32) | uint64_t(uint32_t(cz));
}

}  // namespace

struct WaferPulseCache::Impl {
  std::shared_ptr<const litholattice::ColumnGenerator> gen;
  std::mutex mutex;
  // chunkKey -> blended pulse (freq, sharp, amp, tintR, G, B) followed by
  // blended traces (density, speed, amp, dirMix, tintR, G, B): one weight
  // blend feeds both effects (P1-RENDER H1 wired the trace half).
  std::unordered_map<int64_t, std::array<float, 13>> cache;

  std::array<float, 13> blend(int cx, int cz) {
    // The sanctioned sibling query: regenerate the chunk's column maps
    // (deterministic: same seed -> same weights) and average the per-column
    // weight vectors over the chunk. Cache miss only; meshes hit the cache.
    litholattice::procgen::ColumnMaps maps;
    if (gen) gen->generate(cx, cz, &maps);
    float w[litholattice::kGroundPulseBiomeCount]{};
    const size_t n = maps.biome.size();
    if (n > 0) {
      const float inv = 1.f / float(n);
      for (size_t i = 0; i < n; ++i)
        for (int b = 0; b < kWaferBiomeCount; ++b)
          w[size_t(pulseBiome(WaferBiome(b)))] += biomeWeightAt(maps, i, WaferBiome(b)) * inv;
    }
    // No maps at all (n == 0): all weights stay zero -> the blend functions
    // fall back to the Silicon Flats default. No blending data (old
    // generator): biomeWeightAt one-hots the dominant biome per column, so
    // the chunk average is the dominant-biome fraction - a spatial blend
    // for free.
    const litholattice::GroundPulseParams p = litholattice::blendPulseParams(w);
    const litholattice::GroundTraceParams t = litholattice::blendTraceParams(w);
    return {p.freqHz, p.sharpness, p.amplitude, p.tintR, p.tintG, p.tintB,
            t.density, t.speed, t.amplitude, t.dirMix, t.tintR, t.tintG, t.tintB};
  }

  std::array<float, 13> lookup(int cx, int cz) {
    const int64_t k = chunkKey(cx, cz);
    {
      std::lock_guard<std::mutex> lk(mutex);
      if (const auto it = cache.find(k); it != cache.end()) return it->second;
    }
    // Cache miss: regenerate the chunk's column maps (deterministic) and
    // blend. Done without holding the lock; a duplicate miss on another
    // worker just blends twice and emplaces once (emplace keeps the first).
    const std::array<float, 13> fresh = blend(cx, cz);
    std::lock_guard<std::mutex> lk(mutex);
    return cache.emplace(k, fresh).first->second;
  }
};

WaferPulseCache::WaferPulseCache(std::shared_ptr<const litholattice::ColumnGenerator> waferGen)
    : impl_(std::make_unique<Impl>()) {
  impl_->gen = std::move(waferGen);
}

WaferPulseCache::~WaferPulseCache() = default;

void WaferPulseCache::params(int cx, int cz, float p0[4], float p1[4]) {
  const std::array<float, 13> v = impl_->lookup(cx, cz);
  p0[0] = v[0]; p0[1] = v[1]; p0[2] = v[2]; p0[3] = 0.f;
  p1[0] = v[3]; p1[1] = v[4]; p1[2] = v[5]; p1[3] = 0.f;
}

void WaferPulseCache::traceParams(int cx, int cz, float t0[4], float t1[4]) {
  const std::array<float, 13> v = impl_->lookup(cx, cz);
  t0[0] = v[6]; t0[1] = v[7]; t0[2] = v[8]; t0[3] = v[9];
  t1[0] = v[10]; t1[1] = v[11]; t1[2] = v[12]; t1[3] = 0.f;
}

}  // namespace lithokraft
