// Scout self-test: `locatebiome` / `genmap` command parsing, the spiral biome
// search (locateBiome), the overview-map core (zoom table, granularity rules,
// performance guard, rendering, markers), and the Wafer structure framework
// integration (determinism, chunk-local safety, never-overwrite). Headless.
// LF line endings.
#include "lithokraft/game/scout_selftest.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "litholattice/console/concmd.h"
#include "litholattice/core/json.h"
#include "litholattice/mcp/mcp_senses.h"  // encodePngRgb
#include "litholattice/procgen/stages.h"
#include "litholattice/world/column_generator.h"
#include "lithokraft/content/blocks.h"
#include "lithokraft/game/dimensions.h"  // builtinWorldgenSource
#include "lithokraft/game/game_commands.h"
#include "lithokraft/game/map_render.h"
#include "lithokraft/game/scout.h"
#include "lithokraft/game/wafer_biomes.h"
#include "litho_stamp_models.h"  // model::makeLithoStampModelResolver (G-M4)

namespace lithokraft {
namespace {

int fails = 0;
void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++fails;
}

// Reads the IHDR dimensions out of raw PNG bytes (signature + len + "IHDR").
bool pngDims(const std::vector<uint8_t>& png, int& w, int& h) {
  if (png.size() < 24) return false;
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  for (int i = 0; i < 8; ++i)
    if (png[i] != sig[i]) return false;
  if (png[12] != 'I' || png[13] != 'H' || png[14] != 'D' || png[15] != 'R') return false;
  w = (png[16] << 24) | (png[17] << 16) | (png[18] << 8) | png[19];
  h = (png[20] << 24) | (png[21] << 16) | (png[22] << 8) | png[23];
  return true;
}

/// The real Wafer generator (built-in JSON + code surface rules), fixed seed.
std::shared_ptr<const litholattice::ColumnGenerator> makeWaferGen(uint64_t seed) {
  const char* json = builtinWorldgenSource("wafer");
  if (!json) return nullptr;
  std::string err;
  // The ModelResolver is REQUIRED (P2b-2, review G-M4): wafer.json's
  // lithoform_stamp features fail creation without one. Production
  // (dimensions.cpp) passes makeLithoStampModelResolver(); the test must too.
  auto gen = litholattice::ColumnGenerator::create(json, seed, "wafer", blockRegistry(), err,
                                                   waferSurfaceRules(),
                                                   model::makeLithoStampModelResolver());
  if (!gen) std::printf("  wafer generator failed: %s\n", err.c_str());
  return gen;
}

// -- command parsing -----------------------------------------------------------
void testCommandParsing() {
  using litholattice::console::ConsoleRegistry;
  ConsoleRegistry r;
  r.setCheatsAllowed(true);  // locatebiome/genmap are cheat-gated like give/kill
  std::vector<std::string> biomeSeen;
  std::vector<GenMapRequest> mapSeen;
  std::vector<std::string> printed;
  r.setOutput([&](const std::string& line) { printed.push_back(line); });
  GameCommandBindings b;
  b.locateBiome = [&](const std::string& name) {
    biomeSeen.push_back(name);
    return std::string("ok");
  };
  b.genmap = [&](const GenMapRequest& req) {
    mapSeen.push_back(req);
    return std::string("ok");
  };
  registerGameCommands(r, std::move(b));

  auto run = [&](const char* line) {
    biomeSeen.clear();
    mapSeen.clear();
    printed.clear();
    r.executeLine(line);
  };
  auto printedHas = [&](const char* sub) {
    for (const auto& l : printed)
      if (l.find(sub) != std::string::npos) return true;
    return false;
  };

  run("locatebiome");
  check(biomeSeen.empty() && printedHas("usage: locatebiome"), "locatebiome: no args -> usage");
  run("locatebiome trace_forest");
  check(biomeSeen.size() == 1 && biomeSeen[0] == "trace_forest" && printedHas("ok"),
        "locatebiome: biome name passed to the binding");
  run("locatebiome a b");
  check(biomeSeen.empty() && printedHas("usage:"), "locatebiome: too many args -> usage");

  run("genmap");
  check(mapSeen.empty() && printedHas("usage: genmap"), "genmap: no args -> usage");
  run("genmap out.png player 2 16");
  check(mapSeen.size() == 1 && mapSeen[0].path == "out.png" && mapSeen[0].center == "player" &&
            mapSeen[0].zoom == 2 && mapSeen[0].granularity == 16 && printedHas("ok"),
        "genmap: path/center/zoom/granularity parsed");
  run("genmap out.png 1024,-256 6 64");
  check(mapSeen.size() == 1 && mapSeen[0].center == "1024,-256" && mapSeen[0].zoom == 6 &&
            mapSeen[0].granularity == 64,
        "genmap: explicit x,z center parsed");
  run("genmap out.png player 7 16");
  check(mapSeen.empty() && printedHas("zoom must be 0-6"), "genmap: zoom 7 rejected");
  run("genmap out.png player -1 16");
  check(mapSeen.empty() && printedHas("zoom must be 0-6"), "genmap: negative zoom rejected");
  run("genmap out.png player 2 12");
  check(mapSeen.empty() && printedHas("granularity must be one of 8, 16, 32, 64"),
        "genmap: granularity 12 rejected");
  run("genmap out.png player 2 7");
  check(mapSeen.empty() && printedHas("granularity must be one of 8, 16, 32, 64"),
        "genmap: granularity 7 rejected");
  run("genmap out.png player two 16");
  check(mapSeen.empty() && printedHas("zoom must be 0-6"), "genmap: non-numeric zoom rejected");
  run("genmap out.png player 2 16 extra");
  check(mapSeen.empty() && printedHas("usage: genmap"), "genmap: too many args -> usage");
}

// -- locateBiome ----------------------------------------------------------------
void testLocateBiome() {
  auto gen = makeWaferGen(12345);
  check(gen != nullptr, "locate: wafer generator builds");
  if (!gen) return;

  // Known seed + known origin -> pinned coords (regression net). Seed 12345,
  // origin (0, 0); captured from the deterministic implementation.
  const int pinnedX[] = {45, 0, 7, 39, -52};
  const int pinnedZ[] = {-25, 0, 7, -13, 76};
  for (uint8_t b = 0; b < 5; ++b) {
    const BiomeLocateResult r = locateBiome(*gen, b, 0.0, 0.0);
    std::printf("  locate: biome %u -> (%d, %d) dist %.0f\n", b, r.x, r.z, r.distance);
    check(r.found, "locate: biome found");
    check(r.found && r.x == pinnedX[b] && r.z == pinnedZ[b], "locate: pinned coords for known seed");
    if (r.found) {
      // Self-consistency: the reported column really is that biome.
      check(gen->pipeline().biomeAt(r.x, r.z) == b, "locate: reported column has the biome");
      // Determinism: same seed + same origin = same result.
      const BiomeLocateResult r2 = locateBiome(*gen, b, 0.0, 0.0);
      check(r2.x == r.x && r2.z == r.z, "locate: deterministic across runs");
      // The reported column is at least as close as its own sample cell's neighbors:
      // nothing matching within (dist - 8) blocks.
      const BiomeLocateResult near = locateBiome(*gen, b, 0.0, 0.0, int(r.distance) - 8);
      check(!near.found || near.distance >= r.distance - 1.0, "locate: no closer column just inside");
    }
  }

  // Unknown biome index: clean not-found, not a crash.
  const BiomeLocateResult no = locateBiome(*gen, 99, 0.0, 0.0);
  check(!no.found && no.message.find("within") != std::string::npos,
        "locate: unknown biome -> not-found message");

  // A different origin finds a (possibly) different, still-valid answer.
  const BiomeLocateResult moved = locateBiome(*gen, 4, 1500.0, -800.0);
  check(moved.found && gen->pipeline().biomeAt(moved.x, moved.z) == 4,
        "locate: search from (1500, -800) finds dead_zones");
}

// -- map core -------------------------------------------------------------------
void testMapCore() {
  // Zoom table: 256 << zoom, 7 levels.
  const int regions[] = {256, 512, 1024, 2048, 4096, 8192, 16384};
  bool zoomOk = true;
  for (int z = 0; z <= 6; ++z) zoomOk &= mapRegionBlocks(z) == regions[z];
  zoomOk &= mapRegionBlocks(-1) == -1 && mapRegionBlocks(7) == -1;
  check(zoomOk, "map: zoom table 0..6 -> 256..16384 blocks, invalid -> -1");

  bool gOk = validMapGranularity(8) && validMapGranularity(16) && validMapGranularity(32) &&
             validMapGranularity(64);
  gOk &= !validMapGranularity(7) && !validMapGranularity(12) && !validMapGranularity(0) &&
         !validMapGranularity(-8);
  check(gOk, "map: granularity 8/16/32/64 only");

  check(mapOutputPixels(0, 8) == 32 && mapOutputPixels(6, 8) == 2048 && mapOutputPixels(6, 64) == 256 &&
            mapOutputPixels(4, 16) == 256 && mapOutputPixels(0, 64) == 4 && mapOutputPixels(7, 16) == -1,
        "map: output px = region / granularity (zoom6+g8=2048, zoom0+g64=4)");

  std::string msg;
  check(checkMapSize(2, 16, msg) == MapSizeVerdict::Ok && msg.empty(), "map: zoom2+g16 ok");
  check(checkMapSize(4, 16, msg) == MapSizeVerdict::Ok, "map: zoom4+g16 (512px) ok, no warning");
  check(checkMapSize(6, 8, msg) == MapSizeVerdict::Warn && !msg.empty(),
        "map: zoom6+g8 (2048px) warned, not refused");
  check(checkMapSize(5, 8, msg) == MapSizeVerdict::Warn, "map: zoom5+g8 (1024px) warned");
  check(checkMapSize(6, 16, msg) == MapSizeVerdict::Warn, "map: zoom6+g16 (1024px) warned");
  check(checkMapSize(6, 64, msg) == MapSizeVerdict::Ok, "map: zoom6+g64 (256px) fast, no warning");
  check(checkMapSize(6, 4, msg) == MapSizeVerdict::Refused && msg.find("cap") != std::string::npos,
        "map: zoom6+g4 (4096px) refused with cap message");
  check(checkMapSize(9, 16, msg) == MapSizeVerdict::Refused, "map: bad zoom refused");
  check(checkMapSize(2, 0, msg) == MapSizeVerdict::Refused, "map: bad granularity refused");

  // Rendering: fake biome sampler (stripes) + one marker.
  OverviewMapSource src;
  src.sampleBiomes = [](const int32_t* xs, const int32_t* zs, size_t n, uint8_t* out) {
    for (size_t i = 0; i < n; ++i) out[i] = uint8_t(((xs[i] + zs[i]) / 64) % 5);
  };
  src.structures = [](int32_t, int32_t, int32_t, int32_t) {
    return std::vector<MapMarker>{{4, 4, 255, 0, 0}};
  };
  src.biomeColors = waferMapPalette();

  OverviewMapSpec spec;
  spec.centerX = 0;
  spec.centerZ = 0;
  spec.zoom = 0;  // 256 blocks
  spec.granularity = 32;  // -> 8x8 px
  OverviewMap m1, m2;
  std::string warn;
  check(renderOverviewMap(spec, src, m1, warn) && warn.empty() && m1.width == 8 && m1.height == 8 &&
            m1.rgb.size() == 8 * 8 * 3,
        "map: zoom0+g32 renders 8x8");
  check(renderOverviewMap(spec, src, m2, warn) && m1.rgb == m2.rgb, "map: deterministic render");
  // Marker at world (4,4): x0 = -128, pixel = (4+128)/32 = 4.
  {
    const uint8_t* p = m1.rgb.data() + (size_t(4) * 8 + 4) * 3;
    check(p[0] == 255 && p[1] == 0 && p[2] == 0, "map: structure marker plotted at the right pixel");
  }
  // A biome pixel far from the marker has the palette color of its sample.
  {
    // pixel (0,0) samples block (-128+16, -128+16) = (-112,-112): (-224/64)%5 -> C++ negative %:
    // use the same formula as the fake sampler.
    const int32_t sx = -128 + 16, sz = -128 + 16;
    const uint8_t b = uint8_t(((sx + sz) / 64) % 5);
    const auto& c = waferMapPalette()[b];
    const uint8_t* p = m1.rgb.data();
    check(p[0] == c[0] && p[1] == c[1] && p[2] == c[2], "map: biome pixel has the palette color");
  }
  // PNG validity: IHDR dims match the map dims.
  {
    std::vector<uint8_t> png;
    int pw = 0, ph = 0;
    check(litholattice::mcp::encodePngRgb(m1.width, m1.height, m1.rgb.data(), png) &&
              pngDims(png, pw, ph) && pw == 8 && ph == 8,
          "map: PNG signature + IHDR valid, 8x8 dims match");
  }
  // Failure paths.
  {
    OverviewMapSource noSrc;
    OverviewMap m;
    check(!renderOverviewMap(spec, noSrc, m, warn), "map: missing sampler -> false");
    OverviewMapSpec bad = spec;
    bad.zoom = 9;
    check(!renderOverviewMap(bad, src, m, warn), "map: bad zoom -> false");
    bad = spec;
    bad.zoom = 6;
    bad.granularity = 4;
    check(!renderOverviewMap(bad, src, m, warn) && warn.find("cap") != std::string::npos,
          "map: over-cap combo refused with message");
  }
  // Palette / marker colors.
  {
    const auto pal = waferMapPalette();
    check(pal[0][0] == 24 && pal[4][2] == 76, "map: wafer palette entries");
  {
    const auto wpal = waferMapPalette();
    const auto dpal = mapPaletteForBiomes({"trace_forest", "dicing_plains", "mystery_biome"});
    check(dpal[0] == wpal[0] && dpal[1] == wpal[1], "map: dimension palette keeps wafer colors");
    check(dpal[2] == std::array<uint8_t, 3>{96, 96, 96}, "map: unknown biome -> gray");
  }
    const auto mc = structureMarkerColor("transformer_ruin");
    check(mc[0] == 255 && mc[1] == 140 && mc[2] == 0, "map: transformer_ruin marker is orange");
    const auto mc2 = structureMarkerColor("nope");
    check(mc2[0] == 255 && mc2[1] == 255 && mc2[2] == 255, "map: unknown structure -> white");
  }
}

// -- structures -------------------------------------------------------------------
void testStructures() {
  const uint64_t seed = 777;
  auto gen = makeWaferGen(seed);
  check(gen != nullptr, "structures: wafer generator builds");
  if (!gen) return;
  const uint64_t dimSeed = gen->dimensionSeed();

  // Collect the three structure features.
  struct Found {
    std::string name;
    const litholattice::procgen::StructureFeature* f = nullptr;
  };
  std::vector<Found> found;
  gen->pipeline().visitFeatures([&](std::string_view name, const litholattice::procgen::IFeature& f) {
    if (const auto* s = dynamic_cast<const litholattice::procgen::StructureFeature*>(&f))
      found.push_back({std::string(name), s});
  });
  check(found.size() == 3, "structures: 3 structure features in the wafer pipeline");
  for (const auto& f : found)
    std::printf("  structures: '%s' reach=%d blueprint=%zu\n", f.name.c_str(), f.f->reach(),
                f.f->blueprint().size());

  // Determinism: two identical generators -> identical base sets.
  auto gen2 = makeWaferGen(seed);
  bool detOk = gen2 != nullptr;
  gen->pipeline().visitFeatures([&](std::string_view name, const litholattice::procgen::IFeature& f) {
    const auto* s = dynamic_cast<const litholattice::procgen::StructureFeature*>(&f);
    if (!s) return;
    const auto* s2 = [&]() -> const litholattice::procgen::StructureFeature* {
      const litholattice::procgen::StructureFeature* r = nullptr;
      gen2->pipeline().visitFeatures([&](std::string_view n2, const litholattice::procgen::IFeature& f2) {
        if (n2 == name) r = dynamic_cast<const litholattice::procgen::StructureFeature*>(&f2);
      });
      return r;
    }();
    if (!s2) {
      detOk = false;
      return;
    }
    detOk &= s->basesInRect(dimSeed, -2048, -2048, 2048, 2048) ==
             s2->basesInRect(dimSeed, -2048, -2048, 2048, 2048);
  });
  check(detOk, "structures: same seed -> identical bases (basesInRect deterministic)");

  // Every type places at least one base in a 4096-block square.
  for (const auto& f : found) {
    const auto bases = f.f->basesInRect(dimSeed, -2048, -2048, 2048, 2048);
    std::printf("  structures: '%s' bases in 4K square: %zu\n", f.name.c_str(), bases.size());
    check(!bases.empty(), (std::string("structures: '") + f.name + "' places bases").c_str());
  }

  // Chunk-local safety + never-overwrite: generate a 2x2 chunk area with and
  // without the structure features; every differing cell was air before, and
  // every differing cell is within reach of a structure base.
  std::string jsonText(builtinWorldgenSource("wafer"));
  litholattice::Json parsed;
  std::string jerr;
  bool jok = litholattice::Json::parse(jsonText, parsed, &jerr);
  check(jok, "structures: wafer JSON parses");
  if (!jok) return;
  // Strip ONLY structure features for the comparison generator: tree features
  // (trace_trees, capacitor_clusters, resonator_spires_feat) also place blocks,
  // and the attribution check below only knows structure bases. Keeping trees
  // in both pipelines means differing cells are structure-only, which is what
  // this test is about. (2026-10-11: trees were wired into wafer.json after
  // this test was written; stripping everything broke attribution.)
  for (auto& [k, v] : parsed.obj)
    if (k == "features") {
      litholattice::Json kept = litholattice::Json::array();
      for (auto& f : v.arr) {
        const litholattice::Json* t = f.get("type");
        if (t && t->type == litholattice::Json::Type::String && t->str == "structure") continue;
        kept.arr.push_back(f);
      }
      v = std::move(kept);
    }
  const std::string stripped = parsed.dump();
  std::string err;
  auto genNoStruct = litholattice::ColumnGenerator::create(
      stripped, seed, "wafer", blockRegistry(), err, waferSurfaceRules(),
      model::makeLithoStampModelResolver());  // same resolver as makeWaferGen (G-M4 latent site)
  check(genNoStruct != nullptr, "structures: feature-stripped generator builds");
  if (!genNoStruct) return;

  // All bases in the test area (for attribution). Find a base first, then test
  // the 2x2 chunks around it so the area actually contains structures.
  std::vector<std::array<int32_t, 2>> seedBases;
  gen->pipeline().visitFeatures([&](std::string_view, const litholattice::procgen::IFeature& f) {
    if (const auto* s = dynamic_cast<const litholattice::procgen::StructureFeature*>(&f))
      for (const auto& b : s->basesInRect(dimSeed, -512, -512, 512, 512)) seedBases.push_back(b);
  });
  check(!seedBases.empty(), "structures: bases found for the chunk-safety area");
  if (seedBases.empty()) return;
  const auto floorDiv16 = [](int32_t v) { return v >= 0 ? v / 16 : (v - 15) / 16; };
  const int ccx = floorDiv16(seedBases[0][0]), ccz = floorDiv16(seedBases[0][1]);

  std::vector<std::array<int32_t, 2>> allBases;
  std::vector<int> allReach;
  gen->pipeline().visitFeatures([&](std::string_view, const litholattice::procgen::IFeature& f) {
    if (const auto* s = dynamic_cast<const litholattice::procgen::StructureFeature*>(&f)) {
      for (const auto& b : s->basesInRect(dimSeed, (ccx - 1) * 16 - 32, (ccz - 1) * 16 - 32,
                                         (ccx + 1) * 16 + 32, (ccz + 1) * 16 + 32)) {
        allBases.push_back(b);
        allReach.push_back(s->reach());
      }
    }
  });
  bool safetyOk = true;
  int diffCells = 0;
  for (int cz = ccz - 1; cz <= ccz; ++cz)
    for (int cx = ccx - 1; cx <= ccx; ++cx) {
      // Compare at the ColumnBuffer level (pipeline().generate), like the world
      // does before packing sub-chunks: full per-cell comparison via get().
      auto mkInfo = [&](int x, int z) {
        litholattice::procgen::ChunkInfo info;
        info.cx = x;
        info.cz = z;
        info.size = litholattice::kChunkSize;
        info.height = litholattice::kWorldHeight;
        info.dimensionSeed = dimSeed;
        info.chunkSeed = litholattice::procgen::chunkSeed(dimSeed, x, z);
        return info;
      };
      litholattice::procgen::ColumnMaps mapsWith, mapsWithout;
      const auto with = gen->pipeline().generate(mkInfo(cx, cz), &mapsWith);
      const auto without = genNoStruct->pipeline().generate(mkInfo(cx, cz), &mapsWithout);
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
          for (int y = 0; y < litholattice::kWorldHeight; ++y) {
            const auto a = with.get(x, y, z), b = without.get(x, y, z);
            if (a == b) continue;
            ++diffCells;
            if (b != litholattice::procgen::kAirMaterial) safetyOk = false;  // never-overwrite
            // Attribution: within reach of some base.
            const int32_t wx = cx * 16 + x, wz = cz * 16 + z;
            bool near = false;
            for (size_t i = 0; i < allBases.size(); ++i)
              if (std::abs(wx - allBases[i][0]) <= allReach[i] &&
                  std::abs(wz - allBases[i][1]) <= allReach[i]) {
                near = true;
                break;
              }
            if (!near) safetyOk = false;
          }
      // Determinism: regenerate -> identical.
      const auto with2 = gen->pipeline().generate(mkInfo(cx, cz), nullptr);
      for (int z = 0; z < 16 && safetyOk; ++z)
        for (int x = 0; x < 16 && safetyOk; ++x)
          for (int y = 0; y < litholattice::kWorldHeight; ++y)
            if (with.get(x, y, z) != with2.get(x, y, z)) safetyOk = false;
    }
  std::printf("  structures: %zu bases, %d structure cells in 2x2 chunks\n", allBases.size(), diffCells);
  check(diffCells > 0, "structures: structures actually placed blocks in the test area");
  check(safetyOk, "structures: never-overwrite + attribution + red-gen determinism");
}

}  // namespace

int runScoutSelfTest() {
  fails = 0;
  std::printf("scout selftest (locatebiome/genmap/structures):\n");
  testCommandParsing();
  testLocateBiome();
  testMapCore();
  testStructures();
  std::printf("%s\n", fails ? "SCOUT SELFTEST: FAILED" : "SCOUT SELFTEST: OK");
  return fails;
}

}  // namespace lithokraft
