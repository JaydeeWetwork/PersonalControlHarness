// Biome scouting: the `locatebiome` lithoscript command backend. Spiral search for the
// nearest column whose dominant biome matches, using the pipeline's biome selector
// (GeneratorPipeline::biomeAt) -- no chunk generation, so the search is fast and never
// touches the world. Deterministic: same pipeline/seed + same origin = same result.
// LF line endings.
#pragma once

#include <cstdint>
#include <string>

namespace litholattice {
class ColumnGenerator;
}  // namespace litholattice

namespace lithokraft {

/// Result of locateBiome().
struct BiomeLocateResult {
  bool found = false;
  int x = 0, z = 0;      ///< nearest matching column (world block coords)
  double distance = 0;   ///< blocks from the search origin to (x, z)
  std::string message;   ///< human-readable report (set for both found and not-found)
};

/// Finds the nearest column whose dominant biome-table index is `biomeIndex`, searching
/// outward from (px, pz) in square-spiral ring order over an 8-block sample grid, then
/// refining the best sample to the nearest matching column in its 48-block neighborhood.
/// Searches out to `maxDistanceBlocks` (default 4096); beyond that reports not-found.
/// The message reads e.g. "trace_forest: nearest at (1234, -567), 312 blocks away".
BiomeLocateResult locateBiome(const litholattice::ColumnGenerator& gen, uint8_t biomeIndex,
                             double px, double pz, int maxDistanceBlocks = 4096);

}  // namespace lithokraft
