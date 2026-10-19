// Shared overview-map rendering core. See map_render.h.
// LF line endings.
#include "lithokraft/game/map_render.h"

#include <algorithm>
#include <cstdio>

namespace lithokraft {

MapSizeVerdict checkMapSize(int zoom, int granularity, std::string& message) {
  const int region = mapRegionBlocks(zoom);
  if (region <= 0) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "zoom must be %d-%d (got %d)", kMapZoomMin, kMapZoomMax, zoom);
    message = buf;
    return MapSizeVerdict::Refused;
  }
  if (granularity <= 0) {
    message = "granularity must be positive";
    return MapSizeVerdict::Refused;
  }
  const int px = region / granularity;
  if (px > kMapMaxOutputPx) {
    char buf[192];
    std::snprintf(buf, sizeof buf,
                  "refused: zoom %d + granularity %d would render %dx%d px (cap %dpx); "
                  "use a coarser granularity",
                  zoom, granularity, px, px, kMapMaxOutputPx);
    message = buf;
    return MapSizeVerdict::Refused;
  }
  if (px > 512) {
    char buf[192];
    std::snprintf(buf, sizeof buf,
                  "large map (%dx%d px); generation may take a while -- "
                  "a coarser granularity is faster",
                  px, px);
    message = buf;
    return MapSizeVerdict::Warn;
  }
  message.clear();
  return MapSizeVerdict::Ok;
}

bool renderOverviewMap(const OverviewMapSpec& spec, const OverviewMapSource& src,
                       OverviewMap& out, std::string& message) {
  if (!src.sampleBiomes) {
    message = "no biome sampler";
    return false;
  }
  std::string sizeMsg;
  const MapSizeVerdict verdict = checkMapSize(spec.zoom, spec.granularity, sizeMsg);
  if (verdict == MapSizeVerdict::Refused) {
    message = sizeMsg;
    return false;
  }
  message = (verdict == MapSizeVerdict::Warn) ? sizeMsg : std::string();

  const int region = mapRegionBlocks(spec.zoom);
  const int g = spec.granularity;
  const int px = region / g;
  out.width = px;
  out.height = px;
  out.rgb.assign(size_t(px) * px * 3, 0);
  const int64_t x0 = spec.centerX - region / 2;
  const int64_t z0 = spec.centerZ - region / 2;

  // Tile the sampling (256px tiles): bounded temp memory and cache-friendly batches
  // no matter how large the output is.
  constexpr int kTile = 256;
  std::vector<int32_t> xs, zs;
  std::vector<uint8_t> biomes;
  xs.reserve(size_t(kTile) * kTile);
  zs.reserve(size_t(kTile) * kTile);
  biomes.reserve(size_t(kTile) * kTile);
  for (int tz = 0; tz < px; tz += kTile) {
    for (int tx = 0; tx < px; tx += kTile) {
      const int tw = std::min(kTile, px - tx), th = std::min(kTile, px - tz);
      xs.clear();
      zs.clear();
      for (int j = 0; j < th; ++j)
        for (int i = 0; i < tw; ++i) {
          // One sample per pixel, at the pixel's center block: no per-block
          // iteration at coarse granularities (g=64 samples 1 of 4096 blocks).
          xs.push_back(int32_t(x0 + int64_t(tx + i) * g + g / 2));
          zs.push_back(int32_t(z0 + int64_t(tz + j) * g + g / 2));
        }
      biomes.assign(xs.size(), 0);
      src.sampleBiomes(xs.data(), zs.data(), xs.size(), biomes.data());
      for (size_t k = 0; k < xs.size(); ++k) {
        const int ix = tx + int(k % size_t(tw)), iz = tz + int(k / size_t(tw));
        const auto& c = src.biomeColors[biomes[k]];
        uint8_t* p = out.rgb.data() + (size_t(iz) * px + ix) * 3;
        p[0] = c[0];
        p[1] = c[1];
        p[2] = c[2];
      }
    }
  }

  // Structure markers: one bright pixel per base, plotted over the biome layout.
  if (src.structures) {
    const int32_t rx0 = int32_t(x0), rz0 = int32_t(z0);
    const int32_t rx1 = int32_t(x0 + region), rz1 = int32_t(z0 + region);
    for (const MapMarker& m : src.structures(rx0, rz0, rx1, rz1)) {
      if (m.x < rx0 || m.x >= rx1 || m.z < rz0 || m.z >= rz1) continue;
      const int ix = int((int64_t(m.x) - x0) / g), iz = int((int64_t(m.z) - z0) / g);
      if (ix < 0 || ix >= px || iz < 0 || iz >= px) continue;
      uint8_t* p = out.rgb.data() + (size_t(iz) * px + ix) * 3;
      p[0] = m.r;
      p[1] = m.g;
      p[2] = m.b;
    }
  }
  return true;
}

std::array<std::array<uint8_t, 3>, 256> waferMapPalette() {
  std::array<std::array<uint8_t, 3>, 256> pal{};
  pal[0] = {24, 110, 130};    // trace_forest: deep teal
  pal[1] = {178, 186, 194};   // dicing_plains: light silicon
  pal[2] = {150, 110, 60};    // capacitor_groves: bronze
  pal[3] = {210, 210, 228};   // resonator_spires: pale quartz
  pal[4] = {70, 70, 76};      // dead_zones: dark grey
  return pal;
}

std::array<std::array<uint8_t, 3>, 256> mapPaletteForBiomes(const std::vector<std::string>& names) {
  const auto wafer = waferMapPalette();
  const char* waferNames[] = {"trace_forest", "dicing_plains", "capacitor_groves", "resonator_spires",
                              "dead_zones"};
  std::array<std::array<uint8_t, 3>, 256> pal{};
  for (auto& c : pal) c = {96, 96, 96};  // unknown biome: neutral gray
  for (size_t i = 0; i < names.size() && i < 256; ++i)
    for (size_t w = 0; w < 5; ++w)
      if (names[i] == waferNames[w]) pal[i] = wafer[w];
  return pal;
}

std::array<uint8_t, 3> structureMarkerColor(const std::string& featureName) {
  if (featureName == "transformer_ruin") return {255, 140, 0};    // orange
  if (featureName == "dead_memory_bank") return {255, 70, 70};    // red
  if (featureName == "scribe_monument") return {255, 240, 160};   // pale yellow
  return {255, 255, 255};
}

}  // namespace lithokraft
