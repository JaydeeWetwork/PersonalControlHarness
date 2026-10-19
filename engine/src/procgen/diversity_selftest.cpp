// Ecosystem diversity self-test implementations. See diversity_selftest.h.
#include "litholattice/procgen/diversity_selftest.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "litholattice/procgen/diversity.h"
#include "litholattice/procgen/generator_loader.h"
#include "litholattice/procgen/seed.h"

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

std::optional<MaterialId> divMaterial(std::string_view name) {
  if (name == "rock") return MaterialId{1};
  if (name == "turf") return MaterialId{2};
  if (name == "soil") return MaterialId{3};
  if (name == "ore") return MaterialId{4};
  if (name == "tuft") return MaterialId{5};
  if (name == "flower") return MaterialId{6};
  if (name == "pebble") return MaterialId{7};
  if (name == "snow") return MaterialId{8};
  if (name == "sand") return MaterialId{9};
  if (name == "water") return MaterialId{10};
  if (name == "leaf") return MaterialId{11};
  return std::nullopt;
}

uint64_t hashBuffer(const ColumnBuffer& b) {
  uint64_t h = 0;
  for (int y = 0; y < b.height(); ++y)
    for (int z = 0; z < b.size(); ++z)
      for (int x = 0; x < b.size(); ++x)
        h = mix64(h ^ (uint64_t(b.get(x, y, z)) << 8 | b.state(x, y, z)));
  return h;
}

ChunkInfo divChunk(int cx, int cz, uint64_t dimSeed, int height = 128) {
  ChunkInfo c;
  c.cx = cx;
  c.cz = cz;
  c.size = 16;
  c.height = height;
  c.dimensionSeed = dimSeed;
  c.chunkSeed = chunkSeed(dimSeed, cx, cz);
  return c;
}

// Flat-topped test world: constant height 64, 3 biomes on a fast temperature noise so
// borders are guaranteed inside a 16x16 chunk. `extra` is spliced into the JSON.
std::string divGenText(const std::string& extra) {
  std::string text = R"JSON({
  "graph": {"nodes": {
      "t": {"type": "scale_bias", "scale": 0.5, "bias": 0.5,
            "input": {"type": "noise", "noise": "opensimplex2s", "frequency": 0.45}},
      "h": {"type": "constant", "value": 64}},
    "outputs": {"temperature": "t", "height": "h"}},
  "climate": {"temperature": "temperature"},
  "biomes": [
    {"name": "cold", "climate": {"temperature": 0.15}, "surface": {"top": "rock", "depth": 0}},
    {"name": "mild", "climate": {"temperature": 0.5}, "surface": {"top": "turf", "filler": "soil", "depth": 2}},
    {"name": "hot", "climate": {"temperature": 0.85}, "surface": {"top": "sand", "depth": 0}}],
  "terrain": {"type": "heightmap", "output": "height", "material": "rock"})JSON";
  if (!extra.empty()) {
    text += ",\n";
    text += extra;
  }
  text += "}";
  return text;
}

int countMaterial(const ColumnBuffer& b, MaterialId m) {
  int n = 0;
  for (int y = 0; y < b.height(); ++y)
    for (int z = 0; z < b.size(); ++z)
      for (int x = 0; x < b.size(); ++x)
        if (b.get(x, y, z) == m) ++n;
  return n;
}

void groundCoverChecks(Checker& check) {
  const std::string extra = R"JSON("features": [
    {"name": "gc", "type": "groundcover",
     "entries": [
       {"material": "tuft", "surface": "turf", "min_height": 1, "max_height": 1,
        "biomes": {"mild": 0.6}},
       {"material": "flower", "min_height": 1, "max_height": 2,
        "biomes": {"mild": 0.15}, "tint": "biome"},
       {"material": "pebble", "biomes": {"cold": 0.3, "hot": 0.3}}]}
  ])JSON";
  std::string err;
  const auto gen = loadGenerator(divGenText(extra), dimensionSeed(11, "divtest"), divMaterial, err);
  check(gen != nullptr, "groundcover: generator JSON loads" + (err.empty() ? std::string() : " (" + err + ")"));
  if (!gen) return;
  ColumnMaps maps;
  const ColumnBuffer b0 = gen->generate(divChunk(0, 0, dimensionSeed(11, "divtest")), &maps);
  const int tufts = countMaterial(b0, MaterialId{5});
  const int flowers = countMaterial(b0, MaterialId{6});
  const int pebbles = countMaterial(b0, MaterialId{7});
  check(tufts > 0, "groundcover: tufts placed (" + std::to_string(tufts) + ")");
  check(flowers > 0, "groundcover: flowers placed (" + std::to_string(flowers) + ")");
  check(pebbles > 0, "groundcover: pebbles placed (" + std::to_string(pebbles) + ")");
  // Covers sit on the surface only: nothing buried below y=64.
  bool sane = true;
  for (int z = 0; z < 16 && sane; ++z)
    for (int x = 0; x < 16 && sane; ++x)
      for (int y = 0; y < 64; ++y) {
        const MaterialId m = b0.get(x, y, z);
        if (m == 5 || m == 6 || m == 7) sane = false;
      }
  check(sane, "groundcover: nothing buried below the surface");
  // Tint: flower state byte carries the biome index somewhere.
  bool tinted = false;
  for (int z = 0; z < 16 && !tinted; ++z)
    for (int x = 0; x < 16 && !tinted; ++x)
      for (int y = 65; y < 67; ++y)
        if (b0.get(x, y, z) == 6 && b0.state(x, y, z) < 3) tinted = true;
  check(tinted, "groundcover: biome tint written to flower state bytes");
  // Determinism: same chunk twice, identical buffers.
  const ColumnBuffer b1 = gen->generate(divChunk(0, 0, dimensionSeed(11, "divtest")), nullptr);
  check(hashBuffer(b0) == hashBuffer(b1), "groundcover: deterministic across runs");
  // Neighbour chunk agrees (position-keyed, no RNG shift).
  const ColumnBuffer bn = gen->generate(divChunk(1, 0, dimensionSeed(11, "divtest")), nullptr);
  check(countMaterial(bn, MaterialId{5}) > 0, "groundcover: neighbour chunk also covered");
  // Bad config rejected.
  std::string badErr;
  const bool bad = !loadGenerator(divGenText(R"JSON("features": [
    {"name": "gc", "type": "groundcover", "entries": [{"biomes": {"mild": 1}}]}])JSON"),
                                  dimensionSeed(11, "divtest"), divMaterial, badErr);
  check(bad, "groundcover: entry without material rejected (" + badErr + ")");
}

void rockFormationChecks(Checker& check) {
  const std::string extra = R"JSON("features": [
    {"name": "rocks", "type": "rock_formation", "material": "rock",
     "min_radius": 2, "max_radius": 4, "min_height": 3, "max_height": 9,
     "density": 0.08, "biomes": {"cold": 1.0, "mild": 0.4}}
  ])JSON";
  std::string err;
  const auto gen = loadGenerator(divGenText(extra), dimensionSeed(12, "divtest"), divMaterial, err);
  check(gen != nullptr, "rocks: generator JSON loads" + (err.empty() ? std::string() : " (" + err + ")"));
  if (!gen) return;
  const ColumnBuffer b0 = gen->generate(divChunk(0, 0, dimensionSeed(12, "divtest")), nullptr);
  // Formations add rock above the flat y=64 surface.
  int above = 0;
  for (int z = 0; z < 16; ++z)
    for (int x = 0; x < 16; ++x)
      for (int y = 65; y < 128; ++y)
        if (b0.get(x, y, z) == 1) ++above;
  check(above > 0, "rocks: formations rise above the surface (" + std::to_string(above) + " blocks)");
  const ColumnBuffer b1 = gen->generate(divChunk(0, 0, dimensionSeed(12, "divtest")), nullptr);
  check(hashBuffer(b0) == hashBuffer(b1), "rocks: deterministic across runs");
  // Spire config: tall and thin.
  std::string err2;
  const auto spires = loadGenerator(divGenText(R"JSON("features": [
    {"name": "spires", "type": "rock_formation", "material": "rock",
     "min_radius": 1, "max_radius": 1, "min_height": 10, "max_height": 16,
     "density": 0.05, "biomes": {"hot": 1.0}}])JSON"),
                                    dimensionSeed(12, "divtest"), divMaterial, err2);
  check(spires != nullptr, "rocks: spire profile loads");
}

void transitionChecks(Checker& check) {
  const std::string withRule = R"JSON("biome_transitions": [
    {"a": "cold", "b": "hot", "result": "mild", "width": 2}
  ])JSON";
  std::string err;
  const auto genT = loadGenerator(divGenText(withRule), dimensionSeed(13, "divtest"), divMaterial, err);
  check(genT != nullptr, "transitions: generator JSON loads" + (err.empty() ? std::string() : " (" + err + ")"));
  const auto genP = loadGenerator(divGenText(""), dimensionSeed(13, "divtest"), divMaterial, err);
  check(genP != nullptr, "transitions: plain generator loads for comparison");
  if (!genT || !genP) return;
  ColumnMaps mT, mP;
  genT->generate(divChunk(3, -2, dimensionSeed(13, "divtest")), &mT);
  genP->generate(divChunk(3, -2, dimensionSeed(13, "divtest")), &mP);
  int changed = 0, mildBands = 0;
  for (size_t i = 0; i < mT.biome.size(); ++i) {
    if (mT.biome[i] != mP.biome[i]) ++changed;
    if (mT.biome[i] == 1) ++mildBands;  // "mild" is biome index 1
  }
  check(changed > 0, "transitions: bands rewrite some columns (" + std::to_string(changed) + ")");
  check(mildBands > 0, "transitions: transition biome appears in the map");
  // Every rewritten column must have been cold (0) or hot (2) before, never already mild.
  bool valid = true;
  for (size_t i = 0; i < mT.biome.size(); ++i)
    if (mT.biome[i] != mP.biome[i] && mP.biome[i] == 1) valid = false;
  check(valid, "transitions: only border columns are rewritten");
  // Determinism, including across the chunk border (probe path).
  ColumnMaps mT2;
  genT->generate(divChunk(3, -2, dimensionSeed(13, "divtest")), &mT2);
  check(mT.biome == mT2.biome, "transitions: deterministic across runs");
  // Bad biome rejected.
  std::string badErr;
  const bool bad = !loadGenerator(divGenText(R"JSON("biome_transitions": [
    {"a": "cold", "b": "nope", "result": "mild"}])JSON"),
                                  dimensionSeed(13, "divtest"), divMaterial, badErr);
  check(bad, "transitions: unknown biome rejected (" + badErr + ")");
}

void worleyOreChecks(Checker& check) {
  // worleyF1 unit properties.
  const float a = WorleyOreVeinFeature::worleyF1(12345, 1.5f, 2.5f, 3.5f);
  const float b = WorleyOreVeinFeature::worleyF1(12345, 1.5f, 2.5f, 3.5f);
  check(a == b, "worley: F1 deterministic for identical inputs");
  check(std::isfinite(a) && a >= 0.f && a < 2.f, "worley: F1 finite and in range");
  const float c = WorleyOreVeinFeature::worleyF1(999, 1.5f, 2.5f, 3.5f);
  check(c != a, "worley: F1 varies with seed");
  bool varied = false;
  float lo = 2.f, hi = 0.f;
  for (int i = 0; i < 200; ++i) {
    const float v = WorleyOreVeinFeature::worleyF1(77, float(i) * 0.37f, 20.f, float(i) * 0.61f);
    lo = std::min(lo, v);
    hi = std::max(hi, v);
    if (v < 0.32f) varied = true;
  }
  check(varied && lo < 0.2f && hi > 0.4f, "worley: field dips below vein threshold somewhere");

  const std::string extra = R"JSON("features": [
    {"name": "copper", "type": "worley_ore", "material": "ore", "replace": "rock",
     "min_y": 8, "max_y": 56, "frequency": 0.09, "threshold": 0.34}
  ])JSON";
  std::string err;
  const auto gen = loadGenerator(divGenText(extra), dimensionSeed(14, "divtest"), divMaterial, err);
  check(gen != nullptr, "worley_ore: generator JSON loads" + (err.empty() ? std::string() : " (" + err + ")"));
  if (!gen) return;
  const ColumnBuffer b0 = gen->generate(divChunk(0, 0, dimensionSeed(14, "divtest")), nullptr);
  const int ores = countMaterial(b0, MaterialId{4});
  check(ores > 0, "worley_ore: veins placed (" + std::to_string(ores) + " blocks)");
  // Ore confined to the y band: none above the surface or below min_y.
  bool placed = true;
  for (int z = 0; z < 16 && placed; ++z)
    for (int x = 0; x < 16 && placed; ++x)
      for (int y = 0; y < 128 && placed; ++y)
        if (b0.get(x, y, z) == 4 && (y < 8 || y > 64)) placed = false;
  check(placed, "worley_ore: veins confined to the y band, none floating");
  const ColumnBuffer b1 = gen->generate(divChunk(0, 0, dimensionSeed(14, "divtest")), nullptr);
  check(hashBuffer(b0) == hashBuffer(b1), "worley_ore: deterministic across runs");
  // Clustered variant loads.
  std::string err2;
  const auto clustered = loadGenerator(divGenText(R"JSON("features": [
    {"name": "gold", "type": "worley_ore", "material": "ore", "replace": "rock",
     "min_y": 4, "max_y": 32, "frequency": 0.12, "threshold": 0.3,
     "cluster_frequency": 0.02, "cluster_threshold": 0.1}])JSON"),
                                       dimensionSeed(14, "divtest"), divMaterial, err2);
  check(clustered != nullptr, "worley_ore: clustered variant loads");
}

void faunaChecks(Checker& check) {
  const std::string extra = R"JSON("fauna": {
    "mild": [{"mob": "lithokraft:skitter", "weight": 1.0, "pack": [2, 5]},
             {"mob": "lithokraft:mite", "weight": 0.25}],
    "hot": [{"mob": "lithokraft:dustwurm", "weight": 2.0, "pack": [1, 1]}]
  })JSON";
  std::string err;
  const auto gen = loadGenerator(divGenText(extra), dimensionSeed(15, "divtest"), divMaterial, err);
  check(gen != nullptr, "fauna: generator JSON loads" + (err.empty() ? std::string() : " (" + err + ")"));
  if (!gen) return;
  const auto& mild = gen->faunaForBiome(1);
  check(mild.size() == 2, "fauna: mild table has 2 entries");
  check(mild[0].mob == "lithokraft:skitter" && mild[0].minPack == 2 && mild[0].maxPack == 5,
        "fauna: pack range parsed");
  check(mild[1].minPack == 1 && mild[1].maxPack == 4, "fauna: pack defaults to [1, 4]");
  check(gen->faunaForBiome(0).empty(), "fauna: cold table empty (no section)");
  check(gen->faunaForBiome(77).empty(), "fauna: out-of-range biome empty");
  // faunaAt agrees with the biome map at generated columns.
  ColumnMaps maps;
  gen->generate(divChunk(0, 0, dimensionSeed(15, "divtest")), &maps);
  bool agree = true;
  for (int i = 0; i < 16 && agree; ++i) {
    const int32_t wx = i * 3 - 20, wz = i * 5 - 11;
    const auto at = gen->faunaAt(wx, wz);
    const auto& tbl = gen->faunaForBiome(gen->biomeAt(wx, wz));
    agree = at.size() == tbl.size() && (at.empty() || at[0].mob == tbl[0].mob);
  }
  check(agree, "fauna: faunaAt matches faunaForBiome(biomeAt)");
  // Unknown biome rejected.
  std::string badErr;
  const bool bad = !loadGenerator(divGenText(R"JSON("fauna": {"nope": []})JSON"),
                                  dimensionSeed(15, "divtest"), divMaterial, badErr);
  check(bad, "fauna: unknown biome rejected (" + badErr + ")");
}

void paletteChecks(Checker& check) {
  // Dimension palettes are JSON-only: two fully different profiles build with no code.
  const std::string cosmic = R"JSON({
  "graph": {"nodes": {
      "d": {"type": "noise", "noise": "opensimplex2s", "frequency": 0.02},
      "h": {"type": "constant", "value": 96}},
    "outputs": {"density": "d", "height": "h"}},
  "climate": {},
  "biomes": [{"name": "void", "surface": {"top": "rock", "depth": 0}}],
  "terrain": {"type": "density", "output": "density", "material": "rock", "min_y": 40, "max_y": 160},
  "fauna": {"void": [{"mob": "lithokraft:voidmote", "weight": 1.0}]},
  "features": [
    {"name": "islets", "type": "rock_formation", "material": "rock",
     "min_radius": 3, "max_radius": 6, "min_height": 2, "max_height": 5,
     "density": 0.05, "biomes": {"void": 1.0}}]
  })JSON";
  const std::string underwafer = R"JSON({
  "graph": {"nodes": {"h": {"type": "constant", "value": 48}},
    "outputs": {"height": "h"}},
  "climate": {},
  "biomes": [{"name": "sear", "surface": {"top": "rock", "depth": 0}}],
  "terrain": {"type": "heightmap", "output": "height", "material": "rock"},
  "carvers": [{"type": "worm_cave", "chance": 0.5, "min_length": 60, "max_length": 120,
               "min_radius": 2.0, "max_radius": 4.0, "min_y": 8, "max_y": 40}],
  "features": [
    {"name": "sear_ore", "type": "worley_ore", "material": "ore", "replace": "rock",
     "min_y": 4, "max_y": 44, "frequency": 0.1, "threshold": 0.36}],
  "snowcap": {"snow": "snow", "base_line": 200}
  })JSON";
  std::string err;
  const auto cosmicGen = loadGenerator(cosmic, dimensionSeed(16, "cosmic"), divMaterial, err);
  check(cosmicGen != nullptr, "palettes: cosmic (void islands) profile loads" +
        (err.empty() ? std::string() : " (" + err + ")"));
  const auto underGen = loadGenerator(underwafer, dimensionSeed(16, "underwafer"), divMaterial, err);
  check(underGen != nullptr, "palettes: underwafer (lava tubes) profile loads" +
        (err.empty() ? std::string() : " (" + err + ")"));
  if (!cosmicGen || !underGen) return;
  const ColumnBuffer c0 = cosmicGen->generate(divChunk(0, 0, dimensionSeed(16, "cosmic"), 192), nullptr);
  const ColumnBuffer c1 = cosmicGen->generate(divChunk(0, 0, dimensionSeed(16, "cosmic"), 192), nullptr);
  check(hashBuffer(c0) == hashBuffer(c1), "palettes: cosmic deterministic");
  const ColumnBuffer u0 = underGen->generate(divChunk(0, 0, dimensionSeed(16, "underwafer")), nullptr);
  const int ores = countMaterial(u0, MaterialId{4});
  check(ores > 0, "palettes: underwafer has worley ore (" + std::to_string(ores) + ")");
  check(cosmicGen->faunaForBiome(0).size() == 1, "palettes: cosmic fauna hook present");
}

void weatherChecks(Checker& check) {
  // Snow caps: flat world at y=64; snow line at 50 -> snow everywhere; at 200 -> none.
  const std::string snow = R"JSON("snowcap": {"snow": "snow", "base_line": 50,
    "latitude_falloff": 0.0, "noise_amp": 0.0})JSON";
  std::string err;
  const auto snowy = loadGenerator(divGenText(snow), dimensionSeed(17, "divtest"), divMaterial, err);
  check(snowy != nullptr, "snowcap: generator JSON loads" + (err.empty() ? std::string() : " (" + err + ")"));
  if (snowy) {
    const ColumnBuffer b = snowy->generate(divChunk(0, 0, dimensionSeed(17, "divtest")), nullptr);
    int snowTops = 0;
    for (int z = 0; z < 16; ++z)
      for (int x = 0; x < 16; ++x)
        if (b.get(x, 64, z) == 8) ++snowTops;
    check(snowTops == 256, "snowcap: every column capped (" + std::to_string(snowTops) + "/256)");
    const ColumnBuffer b2 = snowy->generate(divChunk(0, 0, dimensionSeed(17, "divtest")), nullptr);
    check(hashBuffer(b) == hashBuffer(b2), "snowcap: deterministic");
  }
  const std::string noSnow = R"JSON("snowcap": {"snow": "snow", "base_line": 200,
    "latitude_falloff": 0.0, "noise_amp": 0.0, "min_height": 0})JSON";
  const auto bare = loadGenerator(divGenText(noSnow), dimensionSeed(17, "divtest"), divMaterial, err);
  if (bare) {
    const ColumnBuffer b = bare->generate(divChunk(0, 0, dimensionSeed(17, "divtest")), nullptr);
    check(countMaterial(b, MaterialId{8}) == 0, "snowcap: high snow line -> no snow");
  }
  // Latitude falloff: snow line drops with |z|.
  const std::string lat = R"JSON("snowcap": {"snow": "snow", "base_line": 120,
    "latitude_falloff": 0.001, "noise_amp": 0.0, "min_height": 0})JSON";
  const auto latGen = loadGenerator(divGenText(lat), dimensionSeed(17, "divtest"), divMaterial, err);
  if (latGen) {
    const ColumnBuffer eq = latGen->generate(divChunk(0, 0, dimensionSeed(17, "divtest")), nullptr);
    const ColumnBuffer pole = latGen->generate(divChunk(0, 6250, dimensionSeed(17, "divtest")), nullptr);
    int eqSnow = 0, poleSnow = 0;
    for (int z = 0; z < 16; ++z)
      for (int x = 0; x < 16; ++x) {
        if (eq.get(x, 64, z) == 8) ++eqSnow;
        if (pole.get(x, 64, z) == 8) ++poleSnow;
      }
    check(eqSnow == 0 && poleSnow == 256, "snowcap: latitude falloff (equator bare, pole capped)");
  }
  // Sand drifts: desert-only rule with a permissive threshold paints sand tops.
  const std::string drift = R"JSON("sand_drifts": {"sand": "sand", "frequency": 0.05,
    "threshold": -1.0, "depth": 2, "biomes": {"hot": 1.0, "mild": 0.0, "cold": 0.0}})JSON";
  const auto sandy = loadGenerator(divGenText(drift), dimensionSeed(18, "divtest"), divMaterial, err);
  check(sandy != nullptr, "sand_drifts: generator JSON loads" + (err.empty() ? std::string() : " (" + err + ")"));
  if (sandy) {
    ColumnMaps maps;
    const ColumnBuffer b = sandy->generate(divChunk(0, 0, dimensionSeed(18, "divtest")), &maps);
    int drifted = 0, driftedHot = 0;
    for (int z = 0; z < 16; ++z)
      for (int x = 0; x < 16; ++x) {
        const size_t col = size_t(z * 16 + x);
        if (b.get(x, 64, z) == 9 && b.get(x, 63, z) == 9) {
          ++drifted;
          if (maps.biome[col] == 2) ++driftedHot;  // "hot" is biome index 2
        }
      }
    check(drifted > 0, "sand_drifts: drift patches painted (" + std::to_string(drifted) + ")");
    check(drifted == driftedHot, "sand_drifts: confined to the hot biome");
    const ColumnBuffer b2 = sandy->generate(divChunk(0, 0, dimensionSeed(18, "divtest")), nullptr);
    check(hashBuffer(b) == hashBuffer(b2), "sand_drifts: deterministic");
  }
}

}  // namespace

int runDiversitySelfTest() {
  std::printf("Diversity self-test (ecosystem & environment)\n");
  Checker check;
  groundCoverChecks(check);
  rockFormationChecks(check);
  transitionChecks(check);
  worleyOreChecks(check);
  faunaChecks(check);
  paletteChecks(check);
  weatherChecks(check);
  const int fails = check.fails();
  std::printf(fails ? "DIVERSITY SELFTEST FAILED (%d)\n" : "DIVERSITY SELFTEST OK\n", fails);
  return fails;
}

}  // namespace litholattice::procgen
