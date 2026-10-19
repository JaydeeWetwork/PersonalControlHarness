// Wafer ground-pulse biome wiring: per-chunk weight-blended pulse params from
// the biome system's per-column weight vector (JD 2026-10-11: biome blending).
//
// The biome sibling's contract (game/wafer_biomes.h): the Wafer uses the
// "blended" biome selector, so every generated column carries a weight vector -
// ColumnMaps::biomeWeight, row-major `columns * biomeCount`, indexed by
// WaferBiome (TraceForest=0 .. DeadZones=4). Sibling systems query it
// deterministically through ColumnGenerator::generate(cx, cz, &maps).
//
// The weights are worldgen-static per column, so the blend happens ONCE per
// chunk and is cached: WaferPulseCache blends the chunk's columns into one
// param set (waveform/frequency/tint by weight; Dead Zones contribute zero, so
// the pulse fades out smoothly approaching them) and the mesher bakes it into
// each ChunkMesh (MeshContext::pulseParams). Thread-safe: the mesher calls it
// on worker threads.
#pragma once
#include <cstdint>
#include <memory>

namespace litholattice {
class ColumnGenerator;
}

namespace lithokraft {

/// Per-chunk weight-blended ground-pulse params, cached. One per Wafer.
/// Thread-safe.
class WaferPulseCache {
 public:
  explicit WaferPulseCache(std::shared_ptr<const litholattice::ColumnGenerator> waferGen);
  ~WaferPulseCache();

  /// Blended params for chunk column (cx, cz) as two shader-ready vec4s:
  /// p0 = (freqHz, sharpness, amplitude, 0), p1 = (tintR, tintG, tintB, 0).
  /// Pure function of the worldgen weights (cached after the first query).
  void params(int cx, int cz, float p0[4], float p1[4]);

  /// Blended ground-trace params for the same chunk column (same weight
  /// vector, same cache entry): t0 = (density, speed, amplitude, dirMix),
  /// t1 = (tintR, tintG, tintB, 0). See render/ground_traces.h.
  void traceParams(int cx, int cz, float t0[4], float t1[4]);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace lithokraft
