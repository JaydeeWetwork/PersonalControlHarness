// plant_texture_gen: materialize the engine's procedural plant textures
// (engine/src/procgen/shrubs.h) as pack assets (wiring pass 2026-10-13,
// Team B item B4 -- the sprite->worldgen binding's asset bridge).
//
//   plant_texture_gen <assetsDir>
//
// For every texture in the unified plant set [0, numPlantTexturesTotal()),
// plus the two seeded spawn shrubs (kSeededShrubs below):
//   <assetsDir>/packs/pack_16/textures/blocks/<name>.png   (16x16)
//   <assetsDir>/packs/pack_32/textures/blocks/<name>.png   (32x32)
//   <assetsDir>/packs/pack_64/textures/blocks/<name>.png   (64x64)
//   <assetsDir>/packs/pack_32/textures/ui/icons/<name>.png (64x64)
//   <assetsDir>/packs/pack_64/textures/ui/icons/<name>.png (64x64)
//
// Formula-family textures are re-rendered at each size from their
// canonical ShrubParams via generateShrubSprite (the set is seed-
// invariant; per-plant variation arrives via index/tint/size at
// runtime). The three legacy textures exist only as 16x16 pixels, so
// larger sizes are exact integer nearest-neighbour upscales -- the
// pixels are identical, never resampled. Output is deterministic:
// the same engine build always writes byte-identical files.
//
// Build (against the tree's own build dir, no CMake target -- a kept
// authoring tool like texture_gen's manual runs):
//   g++ -std=c++20 -O2 tools/plant_texture_gen.cpp -o /tmp/plant_texture_gen \
//     -I engine/src/procgen/include -I engine/src/assets/include \
//     -I engine/src/subdiv/include -I engine/src/core/include \
//     build-linux/src/procgen/liblitholattice_procgen.a \
//     build-linux/src/assets/liblitholattice_assets.a \
//     build-linux/src/subdiv/liblitholattice_subdiv.a \
//     build-linux/src/core/liblitholattice_core.a -lz -lpthread
// (adjust lib paths to the build dir's actual layout; link errors name
//  whatever else the local build splits out.)
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "litholattice/assets/image.h"
#include "litholattice/procgen/shrubs.h"

namespace fs = std::filesystem;
using namespace litholattice;

namespace {

bool writePng(const fs::path& path, int size, const std::vector<uint8_t>& rgba) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const Image img(size, size, rgba);
  std::string err;
  if (!img.savePng(path, &err)) {
    std::fprintf(stderr, "FAIL %s: %s\n", path.string().c_str(), err.c_str());
    return false;
  }
  std::printf("wrote %s (%dx%d)\n", path.string().c_str(), size, size);
  return true;
}

// --- seeded spawn shrubs (JD directive 2026-10-13) ---------------------------
// Two Litho Formula sprites defined by SEED alone: ShrubParams defaults
// (density 0.6, noiseAmount 0.4, a=b=1, n1=n2=n3=1, m=0 -- the round
// silhouette) with p.seed set. The seed paints only the colour speckle
// (shrubs.h): both sprites share one alpha mask and differ in colour.
// Seeds 311 / 314 are JD's and his neighbour Chris's door numbers; the game
// plants one of each beside the Wafer spawn (game dimensions.cpp,
// waferSetup). NOT members of the engine's canonical formula family (that
// set is seed-invariant by design); like the family entries they are
// re-rendered exactly at each size via generateShrubSprite.
struct SeededShrub {
  const char* name;
  uint64_t seed;
};
constexpr SeededShrub kSeededShrubs[] = {
    {"formula_shrub_311", 311},
    {"formula_shrub_314", 314},
};

procgen::ShrubParams seededShrubParams(uint64_t seed) {
  procgen::ShrubParams p;  // header defaults, exactly as specified
  p.seed = seed;
  return p;
}

// The texture's pixels at `size`: formula entries re-render exactly;
// legacy entries upscale by integer nearest-neighbour from 16x16.
std::vector<uint8_t> pixelsAt(uint32_t index, int size) {
  using namespace litholattice::procgen;
  if (index >= kFirstFormulaTextureIndex) {
    const uint32_t j = index - kFirstFormulaTextureIndex;
    const ShrubParams* p = formulaShrubTextureParams(j);
    if (!p) return {};
    return generateShrubSprite(*p, size);
  }
  const uint8_t* src = plantTexturePixels(index);
  if (!src) return {};
  if (size == kPlantTextureSize)
    return std::vector<uint8_t>(src, src + kPlantTextureBytes);
  if (size % kPlantTextureSize != 0) return {};  // integer upscales only
  const int f = size / kPlantTextureSize;
  std::vector<uint8_t> out(size_t(size) * size * 4);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      const uint8_t* s = src + (size_t(y / f) * kPlantTextureSize + (x / f)) * 4;
      uint8_t* d = out.data() + (size_t(y) * size + x) * 4;
      d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
    }
  return out;
}

}  // namespace

// Write one texture's five pack files; pixel source is `at(size)`.
template <typename At>
int emitTexture(const fs::path& assets, const char* name, At&& at) {
  int fails = 0;
  const std::string file = std::string(name) + ".png";
  for (const auto& [pack, size] : {std::pair{"pack_16", 16}, {"pack_32", 32}, {"pack_64", 64}}) {
    const auto px = at(size);
    if (px.empty() || !writePng(assets / "packs" / pack / "textures" / "blocks" / file, size, px)) ++fails;
  }
  for (const char* pack : {"pack_32", "pack_64"}) {
    const auto px = at(64);
    if (px.empty() || !writePng(assets / "packs" / pack / "textures" / "ui" / "icons" / file, 64, px)) ++fails;
  }
  return fails;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: plant_texture_gen <assetsDir>\n");
    return 2;
  }
  const fs::path assets = argv[1];
  int fails = 0;
  const uint32_t n = procgen::numPlantTexturesTotal();
  for (uint32_t i = 0; i < n; ++i) {
    const char* name = procgen::plantTextureNameAny(i);
    if (!name) { ++fails; continue; }
    fails += emitTexture(assets, name, [i](int size) { return pixelsAt(i, size); });
  }
  for (const SeededShrub& s : kSeededShrubs) {
    const procgen::ShrubParams p = seededShrubParams(s.seed);
    fails += emitTexture(assets, s.name,
                         [&p](int size) { return procgen::generateShrubSprite(p, size); });
  }
  std::printf("plant_texture_gen: %u textures + %zu seeded shrubs, %d failure(s)\n", n,
              sizeof(kSeededShrubs) / sizeof(kSeededShrubs[0]), fails);
  return fails ? 1 : 0;
}
