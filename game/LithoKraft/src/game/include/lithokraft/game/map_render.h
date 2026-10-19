// Shared overview-map rendering core for the `genmap` lithoscript command.
// Decoupled from the binding on purpose: in-game map items will use this same
// pattern later (a 256x256 canvas with a zoom+granularity chosen for 256px output).
// The core renders biome layout (one biome sample per output pixel, at the pixel's
// center block -- never per-block iteration at coarse granularities) plus structure
// markers. No console, no files, no PNG here: the caller PNG-encodes with the
// engine's clean-room encodePngRgb. Deterministic for a fixed source.
// LF line endings.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lithokraft {

// ---- zoom levels (JD 2026-10-11: 7 levels, doubling from 256 to 16K blocks across) ----
// JD said "8 levels" but listed these 7 sizes ending at 16K; the size list wins.

inline constexpr int kMapZoomMin = 0;
inline constexpr int kMapZoomMax = 6;

/// Region width in blocks at a zoom level, or -1 for an invalid zoom.
inline int mapRegionBlocks(int zoom) {
  return (zoom >= kMapZoomMin && zoom <= kMapZoomMax) ? (256 << zoom) : -1;
}

/// Granularities: blocks per pixel. Required; the binding rejects anything else.
inline constexpr int kMapGranularities[] = {8, 16, 32, 64};
inline bool validMapGranularity(int g) {
  for (int v : kMapGranularities)
    if (v == g) return true;
  return false;
}

/// Output pixels per side (region / granularity), or -1 for an invalid zoom/granularity.
inline int mapOutputPixels(int zoom, int granularity) {
  const int region = mapRegionBlocks(zoom);
  return (region > 0 && granularity > 0) ? region / granularity : -1;
}

/// Output cap: the core refuses anything larger (the performance guard). With the
/// restricted granularity set the biggest combo is zoom 6 + granularity 8 = 2048px
/// (warned, not refused); the cap future-proofs finer granularities.
inline constexpr int kMapMaxOutputPx = 2048;

// Zoom/granularity -> output px (fast vs warned):
//
//   zoom | region |  g8   |  g16  |  g32  |  g64
//   -----+--------+-------+-------+-------+------
//     0  |   256  |  32   |  16   |   8   |   4
//     1  |   512  |  64   |  32   |  16   |   8
//     2  |  1024  | 128   |  64   |  32   |  16
//     3  |  2048  | 256   | 128   |  64   |  32
//     4  |  4096  | 512   | 256   | 128   |  64
//     5  |  8192  | 1024! | 512   | 256   | 128
//     6  | 16384  | 2048! | 1024! | 512   | 256
//
// ! = warned: allowed, but the render takes a while (the biome sampler runs one
// climate evaluation per output pixel; indicative times on a fast desktop:
// 256px ~1s, 512px ~2s, 1024px ~7s, 2048px ~27s). Anything over 2048px is refused.
// Sweet spots for the future 256x256 map-item canvas: zoom 4 + g16, zoom 5 + g32,
// zoom 6 + g64 (each renders exactly 256x256, ~1s or less).

enum class MapSizeVerdict { Ok, Warn, Refused };

/// Performance guard: validates the zoom/granularity combo. Ok at or below 512px
/// output; Warn above 512px (allowed, but slow -- the message says so and suggests
/// a coarser granularity); Refused past kMapMaxOutputPx or for invalid inputs.
/// `message` describes the verdict (empty when Ok).
MapSizeVerdict checkMapSize(int zoom, int granularity, std::string& message);

struct OverviewMapSpec {
  int64_t centerX = 0, centerZ = 0;  ///< world block coords of the map center
  int zoom = 2;                      ///< kMapZoomMin..kMapZoomMax
  int granularity = 16;              ///< blocks per pixel (validMapGranularity)
};

/// One structure marker: world base position + marker color.
struct MapMarker {
  int32_t x = 0, z = 0;
  uint8_t r = 255, g = 255, b = 255;
};

/// Data sources the core needs. The lithoscript binding wires the Wafer's pipeline
/// (batched GeneratorPipeline::sampleBiomes + the pipeline's StructureFeatures);
/// in-game map items will wire their own dimension later.
struct OverviewMapSource {
  /// Batch biome query: out[i] = biome-table index at (xs[i], zs[i]).
  std::function<void(const int32_t* xs, const int32_t* zs, size_t n, uint8_t* out)> sampleBiomes;
  /// Markers whose footprint touches [x0, x1) x [z0, z1). May be empty (no structures).
  std::function<std::vector<MapMarker>(int32_t x0, int32_t z0, int32_t x1, int32_t z1)> structures;
  /// Biome-table index -> RGB. The game fills its biomes; unset entries stay black.
  std::array<std::array<uint8_t, 3>, 256> biomeColors{};
};

struct OverviewMap {
  int width = 0, height = 0;
  std::vector<uint8_t> rgb;  ///< top-first RGB, width*height*3
};

/// Renders the overview map into `out`. On success returns true; `message` carries the
/// large-map warning when the verdict is Warn, else empty. Returns false + `message`
/// when the spec is invalid or the size guard refuses.
bool renderOverviewMap(const OverviewMapSpec& spec, const OverviewMapSource& src,
                       OverviewMap& out, std::string& message);

/// The Wafer's biome palette (trace_forest .. dead_zones) for the map core.
std::array<std::array<uint8_t, 3>, 256> waferMapPalette();

/// Builds a palette for an arbitrary dimension from its biome names: Wafer names get
/// the Wafer colors, unknown names get a neutral gray. Keeps genmap sensible on
/// non-Wafer dimensions without coupling the map core to any dimension.
std::array<std::array<uint8_t, 3>, 256> mapPaletteForBiomes(const std::vector<std::string>& names);

/// Marker color for a Wafer structure feature name (transformer_ruin, dead_memory_bank,
/// scribe_monument); white for anything unrecognized.
std::array<uint8_t, 3> structureMarkerColor(const std::string& featureName);

}  // namespace lithokraft
