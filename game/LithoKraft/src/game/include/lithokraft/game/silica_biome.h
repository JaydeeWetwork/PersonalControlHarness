// Silica per-biome tint + density from the Wafer biome weights (JD-locked
// 2026-10-11, workstream 5). LF line endings.
//
// The biome sibling's contract (game/wafer_biomes.h): the Wafer uses the
// "blended" biome selector, so every generated column carries a weight vector --
// ColumnMaps::biomeWeight, row-major `columns * biomeCount`, indexed by
// WaferBiome (TraceForest=0 .. DeadZones=4). Sibling systems query it
// deterministically through ColumnGenerator::generate(cx, cz, &maps).
//
// The weights are worldgen-static per column, so the blend happens ONCE per
// chunk and is cached: SilicaShadeCache averages the chunk's columns into one
// {tint, density} via silicaShadeForWeights (tint = dominant biome's tint,
// density = dominant weight, 0 in Dead Zones). Thread-safe: mesh workers and
// the decoration pass can share one instance.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include "lithokraft/content/silica.h"

namespace litholattice {
class ColumnGenerator;
namespace procgen {
struct ColumnMaps;
}  // namespace procgen
}  // namespace litholattice

namespace lithokraft {

/// Per-chunk weight-blended silica tint + density, cached. One per Wafer.
/// Thread-safe; deterministic (same seed -> same weights -> same shade).
class SilicaShadeCache {
 public:
  explicit SilicaShadeCache(std::shared_ptr<const litholattice::ColumnGenerator> waferGen);
  ~SilicaShadeCache();

  /// Blended shade for chunk column (cx, cz). Pure function of the worldgen
  /// weights (cached after the first query).
  SilicaShade shade(int cx, int cz);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Scatter density (0..1) at one worldgen column: the dominant biome's weight,
/// 0 in Dead Zones. The decoration pass scales silica tuft placement by this.
float silicaScatterDensity(const litholattice::procgen::ColumnMaps& maps, size_t column);

}  // namespace lithokraft
