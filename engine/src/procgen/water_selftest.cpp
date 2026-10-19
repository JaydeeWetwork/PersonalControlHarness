// Headless checks for lakes & oceans (water.h): depth defaults as settings (no engine
// caps since the 2026-10-13 parameterization), the configured min-solid-below loader
// check, flat still water, determinism, border continuity, shoreline placement, loader rules.
#include "litholattice/procgen/water.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "litholattice/procgen/generator_loader.h"
#include "litholattice/procgen/pipeline.h"
#include "litholattice/procgen/seed.h"
#include "litholattice/procgen/stages.h"
// NOTE: procgen is core-only (no world types); the fluid.h seam (kMaxFluidLevel = 16,
// FluidState.level in the state byte) is documented in water.h. This pins our side.
namespace {
inline constexpr uint8_t kFluidMaxLevel = 16;  // mirrors litholattice/world/fluid.h
}

namespace litholattice::procgen {
namespace {

class Checker {
 public:
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }

 private:
  int fails_ = 0;
};

constexpr MaterialId kRock = 1, kWater = 2, kSand = 3, kGravel = 4;

std::optional<MaterialId> waterTestMaterial(std::string_view name) {
  if (name == "rock") return kRock;
  if (name == "water") return kWater;
  if (name == "sand") return kSand;
  if (name == "gravel") return kGravel;
  return std::nullopt;
}

ChunkInfo waterChunk(int cx, int cz, uint64_t dimSeed) {
  ChunkInfo c;
  c.cx = cx;
  c.cz = cz;
  c.size = 16;
  c.height = 768;  // kWorldHeight: the real column height
  c.dimensionSeed = dimSeed;
  c.chunkSeed = chunkSeed(dimSeed, cx, cz);
  return c;
}

bool buffersEqual(const ColumnBuffer& a, const ColumnBuffer& b) {
  if (a.size() != b.size() || a.height() != b.height()) return false;
  for (int z = 0; z < a.size(); ++z)
    for (int x = 0; x < a.size(); ++x)
      for (int y = 0; y < a.height(); ++y)
        if (a.get(x, y, z) != b.get(x, y, z) || a.state(x, y, z) != b.state(x, y, z)) return false;
  return true;
}

void constantsChecks(Checker& check) {
  // Semantics moved (2026-10-13 parameterization): 196/98/60 are DEFAULTS of
  // independent settings fields, not engine caps. The pins below assert the
  // defaults -- LithoKraft's values (JD 2026-10-11) -- not a limit.
  check(kDefaultOceanMaxDepth == 196, "default ocean maxDepth == 196 (JD 2026-10-11, a default not a cap)");
  check(kDefaultLakeMaxDepth == 98, "default lake maxDepth == 98 (JD 2026-10-11, a default not a cap)");
  check(OceanSettings{}.maxDepth == 196, "OceanSettings default maxDepth is 196");
  check(LakeSettings{}.maxDepth == 98, "LakeSettings default maxDepth is 98 (independent field, not ocean/2)");
  check(kDefaultMinSolidBelowOcean == 60, "default min_solid_below == 60");
  check(OceanSettings{}.minSolidBelow == 60, "OceanSettings default minSolidBelow is 60 (explicit parameter)");
  check(kDefaultMinSolidBelowOcean == kDefaultSeaLevel - kDefaultOceanMaxDepth,
        "at the defaults, 256 - 196 = 60 still holds as data");
  check(OceanSettings{}.minDepth == -1, "OceanSettings minDepth defaults to unset (-1 = derive maxDepth/4)");
  check(kStillWaterLevel == 16, "kStillWaterLevel == 16 (source)");
  check(kStillWaterLevel == kFluidMaxLevel, "worldgen water level matches fluid.h kMaxFluidLevel");
  check(kDefaultSeaLevel == kGroundLevel, "default sea level is the reference ground plane");
}

void depthFormulaChecks(Checker& check) {
  check(OceanFillRule::depthForUnit(0.f, 196, -1) == 49, "depthForUnit(0, unset min) == 196/4 == 49 (legacy derive)");
  check(OceanFillRule::depthForUnit(1.f, 196, -1) == 196, "depthForUnit(1) == 196 (configured max reachable)");
  check(OceanFillRule::depthForUnit(-2.f, 196, -1) == 49, "depthForUnit clamps below 0");
  check(OceanFillRule::depthForUnit(2.f, 196, -1) == 196, "depthForUnit clamps above 1");
  check(OceanFillRule::depthForUnit(0.f, 300, 10) == 10, "depthForUnit(0, explicit min 10) == 10");
  check(OceanFillRule::depthForUnit(1.f, 300, 10) == 300, "depthForUnit(1, max 300) == 300 (above the old cap)");
  check(OceanFillRule::depthForUnit(0.5f, 300, 10) == 10 + int(0.5f * 290.f),
        "depthForUnit interpolates [minDepth, maxDepth]");
  bool mono = true, bounded = true;
  int prev = -1;
  for (int k = 0; k <= 100; ++k) {
    const int d = OceanFillRule::depthForUnit(k / 100.f, 196, -1);
    mono = mono && d >= prev;
    bounded = bounded && d >= 49 && d <= 196;
    prev = d;
  }
  check(mono, "depthForUnit monotone non-decreasing on [0, 1]");
  check(bounded, "depthForUnit stays in [49, 196] at the default settings");
}

void clampChecks(Checker& check) {
  // Semantics moved: construction no longer clamps to a cap. A settings value
  // above the old caps survives intact; only sanity floors remain.
  OceanSettings os;
  os.maxDepth = 500;
  check(OceanFillRule(os).settings().maxDepth == 500, "OceanSettings maxDepth 500 is kept (no cap clamp)");
  LakeSettings ls;
  ls.maxDepth = 500;
  check(LakeFillRule(ls).settings().maxDepth == 500, "LakeSettings maxDepth 500 is kept (no cap clamp)");
  OceanSettings zero;
  zero.maxDepth = 0;
  check(OceanFillRule(zero).settings().maxDepth == 1, "OceanSettings maxDepth 0 sanity-floors to 1");
  LakeSettings zeroLake;
  zeroLake.maxDepth = 0;
  check(LakeFillRule(zeroLake).settings().maxDepth == 1, "LakeSettings maxDepth 0 sanity-floors to 1");
  OceanSettings badMin;
  badMin.maxDepth = 100;
  badMin.minDepth = 250;
  check(OceanFillRule(badMin).settings().minDepth == 100, "explicit minDepth clamps into [0, maxDepth] (sanity)");
  OceanSettings noWater;
  noWater.water = kAirMaterial;
  check(OceanFillRule(noWater).settings().water == kAirMaterial, "ocean rule without water material no-ops");
}

// Builds the standard ocean test generator: constant-height rock terrain, one biome.
std::unique_ptr<const GeneratorPipeline> oceanGen(const std::string& heightNode, uint64_t seed,
                                                  const std::string& waterJson, std::string& err) {
  const std::string text = std::string(R"({"graph": {"nodes": {"h": )") + heightNode +
                       R"(}, "outputs": {"height": "h"}},)"
                       R"("terrain": {"output": "height", "material": "rock"},)"
                       R"("biomes": [{"name": "ocean"}],)" + waterJson + "}";
  return loadGenerator(text, seed, waterTestMaterial, err);
}

void oceanCapChecks(Checker& check) {
  // Terrain far deeper than the configured depth: the floor must be raised so liquid
  // depth <= the configured 196, the surface stays flat at sea level, and >= the
  // configured 60 solid layers remain below. (Same numbers as the old cap test --
  // they are now the generator's chosen settings, which is the point.)
  std::string err;
  const uint64_t seed = dimensionSeed(11, "oceantest");
  auto gen = oceanGen(R"({"type": "constant", "value": 0})", seed,
                     R"("oceans": {"sea_level": 256, "max_depth": 196, "water": "water",)"
                     R"( "biomes": {"ocean": 1.0}})",
                     err);
  check(gen != nullptr, "ocean generator loads" + (err.empty() ? std::string() : " (" + err + ")"));
  if (!gen) return;
  ColumnMaps maps;
  const ColumnBuffer b = gen->generate(waterChunk(0, 0, seed), &maps);
  const int n = b.size();
  int minFloor = 1 << 30, maxDepthFound = 0;
  bool ok = true, waterOk = true, solidOk = true, stateOk = true;
  for (int z = 0; z < n; ++z) {
    for (int x = 0; x < n; ++x) {
      const size_t i = size_t(z * n + x);
      const int floorY = maps.height[i];
      minFloor = std::min(minFloor, floorY);
      if (floorY < 60) ok = false;              // >= the configured 60 solid below
      if (floorY < 256 - 196) ok = false;             // configured depth arithmetically
      const int depth = 256 - floorY;
      if (depth > 196) ok = false;
      maxDepthFound = std::max(maxDepthFound, depth);
      int topWater = -1;
      for (int y = 0; y < b.height(); ++y) {
        const MaterialId m = b.get(x, y, z);
        if (m == kWater) {
          topWater = y;
          if (b.state(x, y, z) != kStillWaterLevel) stateOk = false;
        } else if (m != kAirMaterial && y > floorY) {
          waterOk = false;  // solid above the floor inside the water column
        }
        if (y < floorY && (m == kAirMaterial || m == kWater)) solidOk = false;
      }
      if (topWater != 256) waterOk = false;  // flat surface at sea level
      if (floorY >= 256) waterOk = false;    // every column holds water here
    }
  }
  check(ok, "ocean: floor >= sea - 196 everywhere (configured depth enforced by the raise)");
  check(minFloor >= 60, "ocean: >= 60 solid layers below the deepest floor (the configured min_solid_below)");
  check(waterOk, "ocean: flat water surface at sea level 256, water only above the floor");
  check(solidOk, "ocean: solid from y 0 to the floor (raise added solid, never removed)");
  check(stateOk, "ocean: all water is still (state 16)");
  std::printf("    ocean deepest-case: min floor y = %d, max liquid depth = %d\n", minFloor, maxDepthFound);
}

void oceanShallowChecks(Checker& check) {
  // Terrain already above the cap floor: no raise, water just fills to sea level.
  std::string err;
  const uint64_t seed = dimensionSeed(12, "oceantest");
  auto gen = oceanGen(R"({"type": "constant", "value": 200})", seed,
                      R"("oceans": {"sea_level": 256, "max_depth": 196, "water": "water",)"
                      R"( "biomes": {"ocean": 1.0}})",
                      err);
  check(gen != nullptr, "shallow ocean generator loads");
  if (!gen) return;
  ColumnMaps maps;
  const ColumnBuffer b = gen->generate(waterChunk(0, 0, seed), &maps);
  bool ok = true;
  for (int z = 0; z < 16; ++z)
    for (int x = 0; x < 16; ++x) {
      const size_t i = size_t(z * 16 + x);
      const int floorY = maps.height[i];
      if (floorY < 200) ok = false;  // max(200, floorTarget): never lowered
      if (256 - floorY > 196) ok = false;
      if (b.get(x, 256, z) != kWater) ok = false;
      if (b.get(x, 200, z) == kAirMaterial) ok = false;
    }
  check(ok, "ocean: shallow terrain keeps its floor, water fills to a flat 256");
}

void lakeFlatChecks(Checker& check) {
  // Rolling terrain crossing the water table: every lake surface must sit at exactly the
  // table (still water), and solid terrain must be untouched.
  std::string err;
  const uint64_t seed = dimensionSeed(13, "laketest");
  const std::string text =
      R"({"graph": {"nodes": {"h": {"type": "scale_bias", "scale": 1, "bias": 150,)"
      R"( "input": {"type": "wave", "axis": "x", "function": "sin", "frequency": 0.05,)"
      R"( "amplitude": 40, "phase": 0}}}, "outputs": {"height": "h"}},)"
      R"("terrain": {"output": "height", "material": "rock"},)"
      R"("biomes": [{"name": "plains"}],)"
      R"("lakes": {"water_table": 160, "max_depth": 98, "water": "water", "threshold": 0.0,)"
      R"( "biomes": {"plains": 1.0}}})";
  auto gen = loadGenerator(text, seed, waterTestMaterial, err);
  check(gen != nullptr, "lake generator loads" + (err.empty() ? std::string() : " (" + err + ")"));
  if (!gen) return;
  int lakeCols = 0;
  bool flat = true, solidKept = true, stateOk = true;
  for (int cx = 0; cx < 2; ++cx) {  // two adjacent chunks: the surface must not seam
    ColumnMaps maps;
    const ColumnBuffer b = gen->generate(waterChunk(cx, 0, seed), &maps);
    for (int z = 0; z < 16; ++z)
      for (int x = 0; x < 16; ++x) {
        const size_t i = size_t(z * 16 + x);
        const int h = maps.height[i];
        for (int y = 0; y <= h; ++y)
          if (b.get(x, y, z) != kRock) solidKept = false;  // lakes never touch solid
        int topWater = -1;
        for (int y = h + 1; y < b.height(); ++y) {
          if (b.get(x, y, z) == kWater) {
            topWater = y;
            if (b.state(x, y, z) != kStillWaterLevel) stateOk = false;
          }
        }
        if (topWater >= 0) {
          ++lakeCols;
          if (topWater != 160) flat = false;
          if (160 - h > 98) flat = false;
        }
      }
  }
  check(lakeCols > 0, "lake: water formed (depressions below the table in the mask)");
  check(flat, "lake: every lake surface is exactly at the water table (still water)");
  check(solidKept, "lake: solid terrain untouched (no cliffs introduced)");
  check(stateOk, "lake: all water is still (state 16)");
}

void lakeCapChecks(Checker& check) {
  // Pit 200 below the table: exactly the configured 98 of water, air above, never more.
  std::string err;
  const uint64_t seed = dimensionSeed(14, "laketest");
  auto gen = oceanGen(R"({"type": "constant", "value": 56})", seed,
                      R"("lakes": {"water_table": 256, "max_depth": 98, "water": "water", "threshold": 0.0,)"
                      R"( "biomes": {"ocean": 1.0}})",
                      err);
  check(gen != nullptr, "deep-pit lake generator loads");
  if (!gen) return;
  ColumnMaps maps;
  const ColumnBuffer b = gen->generate(waterChunk(0, 0, seed), &maps);
  bool ok = true;
  int cols = 0;
  for (int z = 0; z < 16; ++z)
    for (int x = 0; x < 16; ++x) {
      int topWater = -1, depth = 0;
      for (int y = 57; y < b.height(); ++y) {
        if (b.get(x, y, z) == kWater) {
          topWater = y;
          ++depth;
        }
      }
      if (topWater < 0) {
        ok = false;
        continue;
      }
      ++cols;
      if (topWater != 56 + 98) ok = false;  // exactly the configured 98: 57..154
      if (depth != 98) ok = false;
    }
  check(cols == 256, "lake cap: every column holds lake water");
  check(ok, "lake: 200-deep pit holds exactly the configured 98 water (57..154), air above");
}

void determinismChecks(Checker& check) {
  std::string err;
  const uint64_t seed = dimensionSeed(15, "dettest");
  const std::string text =
      R"({"graph": {"nodes": {"h": {"type": "scale_bias", "scale": 1, "bias": 220,)"
      R"( "input": {"type": "wave", "axis": "x", "function": "sin", "frequency": 0.05,)"
      R"( "amplitude": 40, "phase": 0}}}, "outputs": {"height": "h"}},)"
      R"("terrain": {"output": "height", "material": "rock"},)"
      R"("biomes": [{"name": "ocean"}],)"
      R"("oceans": {"sea_level": 256, "water": "water", "biomes": {"ocean": 1.0}},)"
      R"("lakes": {"water_table": 190, "water": "water", "threshold": 0.0, "biomes": {"ocean": 1.0}},)"
      R"("shoreline": {"band": 2, "depth": 2, "default": {"top": "sand", "filler": "gravel", "depth": 2}}})";
  auto genA = loadGenerator(text, seed, waterTestMaterial, err);
  auto genB = loadGenerator(text, seed, waterTestMaterial, err);
  check(genA && genB, "determinism generators load");
  if (!genA || !genB) return;
  const ColumnBuffer a = genA->generate(waterChunk(3, -2, seed));
  const ColumnBuffer b = genB->generate(waterChunk(3, -2, seed));
  check(buffersEqual(a, b), "determinism: same chunk twice is byte-identical (materials + states)");
  const ColumnBuffer c = genA->generate(waterChunk(4, -2, seed));
  check(!buffersEqual(a, c), "sanity: neighbouring chunks differ");
}

void borderChecks(Checker& check) {
  // Two adjacent chunks through a code-built pipeline: the fill is a pure function of
  // world position, so no column is water on one side of the seam and wrongly dry on the
  // other, and the noise-driven floor relief has no step at the border.
  const uint64_t seed = dimensionSeed(16, "bordertest");
  GraphDesc desc;
  std::string err;
  if (!GraphDesc::parse(R"({"nodes": {"h": {"type": "constant", "value": 0}},)"
                        R"( "outputs": {"height": "h"}})",
                        desc, err)) {
    check(false, "border graph parses (" + err + ")");
    return;
  }
  auto graph = NoiseGraph::compile(desc, seed, err);
  check(graph != nullptr, "border graph compiles");
  if (!graph) return;
  GeneratorPipeline::Builder b(graph);
  b.terrain(std::make_unique<HeightmapTerrain>(graph, graph->outputIndex("height"), kRock, 0));
  b.biomes(std::make_unique<MultiNoiseBiomeSelector>(std::vector<BiomePoint>{{"ocean"}}));
  OceanSettings os;
  os.water = kWater;
  os.seaLevel = 256;
  os.maxDepth = 196;
  os.seed = seed;
  os.biomes.density.fill(0.f);
  os.biomes.density[0] = 1.f;
  auto rule = std::make_unique<OceanFillRule>(os);
  const OceanFillRule* rulePtr = rule.get();
  b.surface(std::move(rule));
  auto pipeline = b.build();
  check(pipeline != nullptr, "border pipeline builds");
  if (!pipeline) return;

  ColumnMaps mA, mB;
  const ColumnBuffer A = pipeline->generate(waterChunk(0, 0, seed), &mA);
  const ColumnBuffer B = pipeline->generate(waterChunk(1, 0, seed), &mB);
  bool fillOk = true;
  for (int z = 0; z < 16; ++z) {
    for (int cx = 0; cx < 2; ++cx) {
      const ColumnBuffer& buf = cx == 0 ? A : B;
      const ColumnMaps& maps = cx == 0 ? mA : mB;
      for (int x = 0; x < 16; ++x) {
        const size_t i = size_t(z * 16 + x);
        const int floorY = maps.height[i];
        const bool expectWater = floorY >= 0 && floorY < 256;
        const bool hasWater = buf.get(x, 256, z) == kWater;
        if (expectWater != hasWater) fillOk = false;
        if (hasWater) {
          int top = -1;
          for (int y = floorY + 1; y < buf.height(); ++y)
            if (buf.get(x, y, z) == kWater) top = y;
          if (top != 256) fillOk = false;
        }
        if (256 - floorY > 196) fillOk = false;
      }
    }
  }
  check(fillOk, "border: fill == pure function of position across the chunk seam (flat 256, configured depth holds)");

  // The relief field itself: adjacent world columns (including the seam pair 15|16) differ
  // by small steps -- the water system adds no cliffs of its own.
  bool smooth = true;
  for (int z = 0; z < 16; ++z)
    for (int wx = 0; wx < 31; ++wx) {
      const int a = rulePtr->floorTarget(wx, z, 256), c = rulePtr->floorTarget(wx + 1, z, 256);
      if (std::abs(a - c) > 12) smooth = false;
      const int d = rulePtr->floorTarget(wx, z, 256), e = rulePtr->floorTarget(wx, z + 1, 256);
      if (std::abs(d - e) > 12) smooth = false;
      if (rulePtr->floorTarget(wx, z, 256) < 256 - 196) smooth = false;
    }
  check(smooth, "border: ocean floor relief is smooth across the seam and never below 256 - 196");
}

void shorelineChecks(Checker& check) {
  // Slope crossing sea level: the rim follows the waterline, water and high ground untouched.
  std::string err;
  const uint64_t seed = dimensionSeed(17, "shoretest");
  const std::string text =
      R"({"graph": {"nodes": {"h": {"type": "scale_bias", "scale": 1, "bias": 240,)"
      R"( "input": {"type": "wave", "axis": "x", "function": "sin", "frequency": 0.05,)"
      R"( "amplitude": 30, "phase": 0}}}, "outputs": {"height": "h"}},)"
      R"("terrain": {"output": "height", "material": "rock"},)"
      R"("biomes": [{"name": "plains"}],)"
      R"("oceans": {"sea_level": 256, "water": "water", "biomes": {"plains": 1.0}},)"
      R"("shoreline": {"band": 2, "depth": 2, "default": {"top": "sand", "filler": "gravel", "depth": 2}}})";
  auto gen = loadGenerator(text, seed, waterTestMaterial, err);
  check(gen != nullptr, "shoreline generator loads" + (err.empty() ? std::string() : " (" + err + ")"));
  if (!gen) return;
  ColumnMaps maps;
  const ColumnBuffer b = gen->generate(waterChunk(0, 0, seed), &maps);
  bool rimOk = true, highOk = true, waterOk = true;
  int rimCols = 0;
  for (int z = 0; z < 16; ++z)
    for (int x = 0; x < 16; ++x) {
      const size_t i = size_t(z * 16 + x);
      const int h = maps.height[i];
      if (h >= 256 - 2 && h <= 256 + 2) {  // the rim band around the waterline
        ++rimCols;
        if (b.get(x, h, z) != kSand) rimOk = false;
      } else if (h > 256 + 2) {
        if (b.get(x, h, z) != kRock) highOk = false;  // high ground untouched
      }
      for (int y = h + 1; y < b.height(); ++y) {
        const MaterialId m = b.get(x, y, z);
        if (m != kAirMaterial && m != kWater) waterOk = false;  // nothing but water above solid
        if (m == kWater && b.state(x, y, z) != kStillWaterLevel) waterOk = false;
      }
    }
  check(rimCols > 0, "shoreline: rim columns exist along the waterline");
  check(rimOk, "shoreline: rim band [254, 258] rewritten to sand");
  check(highOk, "shoreline: ground above the band untouched");
  check(waterOk, "shoreline: water cells untouched by the shore rule");
}

void parameterizationChecks(Checker& check) {
  // Relational proof (a): a generator that states LithoKraft's values explicitly
  // (max_depth 196 / 98) generates byte-identically to the engine-default generator
  // over a synthetic region -- the parameterization did not move default output.
  std::string err;
  const uint64_t seed = dimensionSeed(21, "paramtest");
  const std::string defJson =
      R"("oceans": {"sea_level": 256, "water": "water", "biomes": {"ocean": 1.0}},)"
      R"("lakes": {"water_table": 200, "water": "water", "threshold": 0.0, "biomes": {"ocean": 1.0}})";
  const std::string expJson =
      R"("oceans": {"sea_level": 256, "max_depth": 196, "min_solid_below": 60, "water": "water", "biomes": {"ocean": 1.0}},)"
      R"("lakes": {"water_table": 200, "max_depth": 98, "water": "water", "threshold": 0.0, "biomes": {"ocean": 1.0}})";
  auto genDef = oceanGen(R"({"type": "constant", "value": 120})", seed, defJson, err);
  auto genExp = oceanGen(R"({"type": "constant", "value": 120})", seed, expJson, err);
  check(genDef && genExp, "param: default and explicit-196/98 generators both load");
  if (genDef && genExp) {
    bool same = true;
    const int coords[][2] = {{0, 0}, {2, 1}, {-1, 3}};
    for (const auto& cc : coords) {
      const ColumnBuffer a = genDef->generate(waterChunk(cc[0], cc[1], seed));
      const ColumnBuffer b = genExp->generate(waterChunk(cc[0], cc[1], seed));
      same = same && buffersEqual(a, b);
    }
    check(same, "param: explicit 196/98 == engine default, byte-identical over a synthetic region");
  }

  // Relational proof (b): max_depth 300 (above the old 196 cap) LOADS and generates
  // deeper water than the 196 configuration on the same synthetic field.
  // depth_frequency 1/16 makes the relief sweep most of its range inside a few
  // chunks, so the deep end of the field is actually sampled by the region below.
  auto gen300 = oceanGen(R"({"type": "constant", "value": 0})", seed,
                         R"("oceans": {"sea_level": 400, "max_depth": 300, "depth_frequency": 0.0625, "water": "water", "biomes": {"ocean": 1.0}})", err);
  check(gen300 != nullptr, "param: ocean max_depth 300 generator loads (previously cap-rejected)" +
                               (err.empty() ? std::string() : " (" + err + ")"));
  auto gen196 = oceanGen(R"({"type": "constant", "value": 0})", seed,
                         R"("oceans": {"sea_level": 400, "max_depth": 196, "depth_frequency": 0.0625, "water": "water", "biomes": {"ocean": 1.0}})", err);
  check(gen196 != nullptr, "param: ocean max_depth 196 generator at sea 400 loads");
  if (gen300 && gen196) {
    ColumnBuffer b300(16, 768), b196(16, 768);
    int maxD300 = 0, maxD196 = 0;
    bool deeperOrEqual = true, floorOk = true;
    for (int cz = 0; cz < 3; ++cz)
      for (int cx = 0; cx < 3; ++cx) {
        ColumnMaps m300, m196;
        b300 = gen300->generate(waterChunk(cx, cz, seed), &m300);
        b196 = gen196->generate(waterChunk(cx, cz, seed), &m196);
        for (size_t i = 0; i < m300.height.size(); ++i) {
          const int d300 = 400 - m300.height[i], d196 = 400 - m196.height[i];
          maxD300 = std::max(maxD300, d300);
          maxD196 = std::max(maxD196, d196);
          if (d300 < d196) deeperOrEqual = false;
          if (m300.height[i] < 400 - 300) floorOk = false;
        }
      }
    check(deeperOrEqual, "param: depth-300 floors are never shallower than depth-196 floors (same field)");
    check(maxD300 > 196 && maxD196 <= 196,
          "param: depth-300 generates water deeper than 196 (max " + std::to_string(maxD300) + " vs " +
              std::to_string(maxD196) + ")");
    check(floorOk, "param: depth-300 floors stay >= sea_level - 300 (the configured depth, arithmetically)");
    check(!buffersEqual(b300, b196), "param: depth-300 output actually differs from depth-196 output");
  }

  // min_depth as data: an explicit min_depth raises the shallowest floor targets
  // versus the derived maxDepth/4 behaviour on the same relief field.
  OceanSettings base;
  base.maxDepth = 196;
  base.seed = seed;
  base.biomes.density.fill(0.f);
  base.biomes.density[0] = 1.f;
  OceanSettings withMin = base;
  withMin.minDepth = 10;
  const OceanFillRule derived(base), explicitMin(withMin);
  int maxDerived = -1, maxExplicit = -1;
  for (int wz = 0; wz < 64; ++wz)
    for (int wx = 0; wx < 64; ++wx) {
      maxDerived = std::max(maxDerived, derived.floorTarget(wx, wz, 400));
      maxExplicit = std::max(maxExplicit, explicitMin.floorTarget(wx, wz, 400));
    }
  check(maxExplicit > maxDerived,
        "param: explicit min_depth 10 raises the shallow relief (max floor target " + std::to_string(maxExplicit) +
            " > derived " + std::to_string(maxDerived) + ")");
}

void loaderChecks(Checker& check) {
  std::string err;
  const uint64_t seed = dimensionSeed(18, "loadertest");
  const std::string base =
      R"({"graph": {"nodes": {"h": {"type": "constant", "value": 200}}, "outputs": {"height": "h"}},)"
      R"("terrain": {"output": "height", "material": "rock"},)"
      R"("biomes": [{"name": "ocean"}]})";
  const auto tryLoad = [&](const std::string& extra) {
    std::string e;
    auto g = loadGenerator(base.substr(0, base.size() - 1) + "," + extra + "}", seed, waterTestMaterial, e);
    return std::pair<bool, std::string>(g != nullptr, e);
  };
  {
    const auto [ok, e] = tryLoad(R"("oceans": {"water": "water", "biomes": {"ocean": 1.0}})");
    check(ok, "loader: minimal oceans section loads" + (e.empty() ? std::string() : " (" + e + ")"));
  }
  {
    // Semantics moved: 197 was REJECTED by the old hard cap; as a settings value it
    // loads. (Sea 300 keeps sea - depth >= the default min_solid_below 60; at the
    // default sea 256 a 197 depth is rejected by that DATA rule, not by a cap --
    // see the min_solid_below checks in parameterizationChecks/loaderChecks below.)
    const auto [ok, e] =
        tryLoad(R"("oceans": {"sea_level": 300, "max_depth": 197, "water": "water", "biomes": {"ocean": 1.0}})");
    check(ok, "loader: ocean max_depth 197 loads (was cap-rejected before parameterization)" +
                  (e.empty() ? std::string() : " (" + e + ")"));
  }
  {
    const auto [ok, e] =
        tryLoad(R"("oceans": {"sea_level": 200, "max_depth": 196, "water": "water", "biomes": {"ocean": 1.0}})");
    check(!ok && e.find("60") != std::string::npos,
          "loader: sea 200 / depth 196 rejected by the DEFAULT min_solid_below 60 (" + e + ")");
  }
  {
    // ...and the same pair loads when the data lowers min_solid_below: the rule is data now.
    const auto [ok, e] = tryLoad(
        R"("oceans": {"sea_level": 200, "max_depth": 196, "min_solid_below": 4, "water": "water", "biomes": {"ocean": 1.0}})");
    check(ok, "loader: sea 200 / depth 196 loads with min_solid_below 4 (the 60 is a parameter)" +
                  (e.empty() ? std::string() : " (" + e + ")"));
  }
  {
    const auto [ok, e] = tryLoad(R"("oceans": {"min_depth": 250, "max_depth": 196, "water": "water", "biomes": {"ocean": 1.0}})");
    check(!ok, "loader: min_depth > max_depth rejected as a sanity error (" + e + ")");
  }
  {
    const auto [ok, e] = tryLoad(R"("lakes": {"max_depth": 300, "water": "water", "biomes": {"ocean": 1.0}})");
    check(ok, "loader: lake max_depth 300 loads (was cap-rejected at 98)" +
                  (e.empty() ? std::string() : " (" + e + ")"));
  }
  {
    const auto [ok, e] = tryLoad(R"("oceans": {"water": "water", "biomes": {"nope": 1.0}})");
    check(!ok, "loader: unknown biome in oceans rejected (" + e + ")");
  }
  {
    const auto noWater = [](std::string_view n) -> std::optional<MaterialId> {
      if (n == "rock") return kRock;
      return std::nullopt;
    };
    std::string e;
    auto g = loadGenerator(base.substr(0, base.size() - 1) + R"(,"oceans": {"biomes": {"ocean": 1.0}}})", seed,
                           noWater, e);
    check(g == nullptr, "loader: oceans without a water block rejected (" + e + ")");
  }
  {
    const auto [ok, e] = tryLoad(R"("lakes": {"threshold": 1.5, "water": "water", "biomes": {"ocean": 1.0}})");
    check(!ok, "loader: lake threshold 1.5 rejected (" + e + ")");
  }
  {
    const auto [ok, e] = tryLoad(R"("shoreline": {"band": 3, "default": {"top": "sand"}})");
    check(ok, "loader: shoreline section loads" + (e.empty() ? std::string() : " (" + e + ")"));
  }
}

}  // namespace

int runWaterSelfTest() {
  std::printf("[water] lakes & oceans\n");
  Checker check;
  constantsChecks(check);
  depthFormulaChecks(check);
  clampChecks(check);
  oceanCapChecks(check);
  oceanShallowChecks(check);
  lakeFlatChecks(check);
  lakeCapChecks(check);
  determinismChecks(check);
  parameterizationChecks(check);
  borderChecks(check);
  shorelineChecks(check);
  loaderChecks(check);
  std::printf("[water] %s (%d failures)\n", check.fails() == 0 ? "OK" : "FAIL", check.fails());
  return check.fails();
}

}  // namespace litholattice::procgen
