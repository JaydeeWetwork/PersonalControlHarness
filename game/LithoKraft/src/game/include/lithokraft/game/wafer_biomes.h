// The Wafer's surface biomes: ids, the per-column biome weight contract, and the code
// surface rules that JSON material names cannot express (rules placing block ids directly,
// e.g. kDeadSilicon).
//
// Biome ids are the index into the generator JSON's "biomes" list
// (assets/worldgen/wafer.json), written per column into ColumnMaps::biome by the biome
// selector. The Wafer uses the "blended" selector, so every column also carries a weight
// vector: ColumnMaps::biomeWeight is row-major `columns * biomeCount` floats, and the
// weight of biome b at column (x, z) is
//
//   maps.biomeWeight[(z * kChunkSize + x) * maps.biomeCount + b]
//
// Weights are >= 0 and sum to 1 per column; ColumnMaps::biome holds the dominant
// (nearest) biome. Sibling workstreams query all of this deterministically through
// ColumnGenerator::generate(cx, cz, &maps):
//
//   procgen::ColumnMaps maps;
//   gen->generate(cx, cz, &maps);
//   const size_t i = size_t(z * kChunkSize + x);
//   WaferBiome dominant = WaferBiome(maps.biome[i]);
//   float deadW = lithokraft::biomeWeightAt(maps, i, WaferBiome::DeadZones);
//
//   - Tree sibling (decoration density): feature density scales with the weights via
//     procgen::blendedDensity(densities, maps, column) -- already weight-aware.
//   - Pulse sibling (energized ground): interpolate per-biome pulse parameters
//     (waveform morph, frequency, tint channels, ...) with blendLerp() below.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>
#include "litholattice/procgen/pipeline.h"

namespace lithokraft {

/// Wafer biome ids. ORDER MUST MATCH the "biomes" list in assets/worldgen/wafer.json.
enum class WaferBiome : uint8_t {
  TraceForest = 0,      ///< dense circuit-trees, energized, red/blue glow
  DicingPlains = 1,     ///< vast flats, die-grid scribe lines, sparse first-spawn territory
  CapacitorGroves = 2,  ///< dense squat cylinders, maze-like
  ResonatorSpires = 3,  ///< tall quartz spires
  DeadZones = 4,        ///< grey, unpowered, no glow
};
inline constexpr int kWaferBiomeCount = 5;
inline constexpr std::string_view kWaferBiomeNames[kWaferBiomeCount] = {
    "trace_forest", "dicing_plains", "capacitor_groves", "resonator_spires", "dead_zones"};

/// Die-grid scribe spacing on the Dicing Plains, in blocks (1-wide scored lines).
inline constexpr int kScribeSpacing = 32;

/// Weight of one biome at one column (see the layout above). Falls back to the dominant
/// biome (weight 1) when the maps carry no blending data. Deterministic.
inline float biomeWeightAt(const litholattice::procgen::ColumnMaps& maps, size_t column, WaferBiome b) {
  const size_t nb = maps.biomeCount, bi = size_t(b);
  const bool have = nb > 0 && bi < nb && column < maps.biome.size() &&
                    maps.biomeWeight.size() >= (column + 1) * nb;
  if (!have)
    return (column < maps.biome.size() && WaferBiome(maps.biome[column]) == b) ? 1.f : 0.f;
  return maps.biomeWeight[column * nb + bi];
}

/// Interpolates one float per biome id by the column's blend weights -- the primitive the
/// pulse sibling uses for waveform/frequency/tint interpolation across biome borders.
/// `perBiome` has kWaferBiomeCount entries. Falls back to the dominant biome with no
/// blending data. Deterministic.
inline float blendLerp(const litholattice::procgen::ColumnMaps& maps, size_t column, const float* perBiome) {
  const size_t nb = maps.biomeCount;
  const bool have =
      nb > 0 && column < maps.biome.size() && maps.biomeWeight.size() >= (column + 1) * nb;
  if (!have) return perBiome[column < maps.biome.size() ? size_t(maps.biome[column]) : 0];
  const float* w = maps.biomeWeight.data() + column * nb;
  float v = 0.f, wsum = 0.f;
  for (size_t b = 0; b < nb && b < size_t(kWaferBiomeCount); ++b) {
    v += perBiome[b] * w[b];
    wsum += w[b];
  }
  return wsum > 0.f ? v / wsum : perBiome[size_t(maps.biome[column])];
}

/// The Wafer's code surface rules, appended after the JSON LayeredSurface in the pipeline:
///  - WaferPaletteSurface: per-biome ground tops by BlockId (TraceForest -> silicon,
///    DicingPlains -> kScribeSilicon/49, CapacitorGroves -> kDenseSilicon/50,
///    ResonatorSpires -> kFleckedSilicon/51, DeadZones -> kDeadSilicon/46 + stone-run
///    remap). Weight-dithered, so borders fade.
///  - DicingPlains: score the die grid -- 1-wide, 1-deep lines every kScribeSpacing blocks.
///  - ResonatorSpires: sparse kResonatorCrystal accents on the silicon surface.
/// All three are weight-aware: at blend borders they apply with probability = the biome
/// weight (seeded hash of the column position), so edges fade instead of cutting.
/// Deterministic: no RNG, everything derives from world coordinates and the weights.
std::vector<std::unique_ptr<litholattice::procgen::ISurfaceRule>> waferSurfaceRules();

}  // namespace lithokraft
