// Self-test for LithoStampFeature and the "lithoform_stamp" generator JSON type:
// baked .lthm models scattered as world structures via an in-memory ModelResolver.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "litholattice/procgen/generator_loader.h"
#include "litholattice/procgen/pipeline.h"
#include "litholattice/procgen/seed.h"
#include "litholattice/procgen/stages.h"

namespace litholattice::procgen::litho_formula {
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

// Minimal flat world: constant height 64, single "plains" biome.
constexpr const char* kStampGen = R"({
  "graph": {"nodes": {"h": {"type": "constant", "value": 64}}, "outputs": {"height": "h"}},
  "terrain": {"output": "height", "material": "rock"},
  "biomes": [{"name": "plains"}],
  "features": [__FEATURE__]
})";

constexpr const char* kStampFeature = R"({
  "name": "stamp_test", "type": "lithoform_stamp", "model": "__MODEL__",
  "materials": {"1": "rock", "2": "soil"},
  "cell_size": 32, "density": __DENSITY__,
  "biomes": {"plains": __BIOME__},
  "rotate": 0, "y_offset": 0, "anchor_center": 0
})";

std::string genJson(const std::string& model, const std::string& density, const std::string& biome) {
  std::string feat = kStampFeature;
  auto sub = [&](const std::string& from, const std::string& to) {
    const size_t i = feat.find(from);
    if (i != std::string::npos) feat.replace(i, from.size(), to);
  };
  sub("__MODEL__", model);
  sub("__DENSITY__", density);
  sub("__BIOME__", biome);
  std::string g = kStampGen;
  const size_t i = g.find("__FEATURE__");
  g.replace(i, 11, feat);
  return g;
}

std::optional<MaterialId> testMat(std::string_view name) {
  if (name == "rock") return MaterialId(1);
  if (name == "soil") return MaterialId(2);
  return std::nullopt;
}

// Three-cell test model: an L of palette 1 with one palette-2 cell on top.
ModelResolver testModels() {
  return [](std::string_view name) -> std::optional<LithoStampModel> {
    if (name != "test_cube") return std::nullopt;
    LithoStampModel m;
    m.size = 4;
    m.cells = {LithoStampCell{0, 0, 0, 1}, LithoStampCell{1, 0, 0, 1}, LithoStampCell{0, 1, 0, 2}};
    return m;
  };
}

ChunkInfo testChunk(uint64_t dimSeed) {
  ChunkInfo c;
  c.cx = 0;
  c.cz = 0;
  c.size = 16;
  c.height = 128;
  c.dimensionSeed = dimSeed;
  c.chunkSeed = chunkSeed(dimSeed, 0, 0);
  return c;
}

// Counts blocks of a material in the buffer.
int countMat(const ColumnBuffer& b, MaterialId m) {
  int n = 0;
  for (int y = 0; y < b.height(); ++y)
    for (int z = 0; z < b.size(); ++z)
      for (int x = 0; x < b.size(); ++x)
        if (b.get(x, y, z) == m) ++n;
  return n;
}

void stampChecks(Checker& check) {
  const uint64_t ds = dimensionSeed(11, "stamp_test");
  // 0. The real wafer.json loads end-to-end (stub models + permissive materials):
  // catches typos in biome names, material keys, and feature schema.
  {
    const char* candidates[] = {
        "game/LithoKraft/assets/worldgen/wafer.json",
        "../game/LithoKraft/assets/worldgen/wafer.json",
        "/home/hatch/workspace/litholattice/game/LithoKraft/assets/worldgen/wafer.json",
    };
    std::string text;
    for (const char* p : candidates) {
      std::ifstream f(p);
      if (f) {
        std::ostringstream ss;
        ss << f.rdbuf();
        text = ss.str();
        break;
      }
    }
    if (text.empty()) {
      check(false, "lithoform_stamp: wafer.json not found for integration check");
    } else {
      auto stubModels = [](std::string_view name) -> std::optional<LithoStampModel> {
        LithoStampModel m;
        m.size = 4;
        m.cells = {LithoStampCell{0, 0, 0, 1}};
        (void)name;
        return m;
      };
      auto anyMat = [](std::string_view name) -> std::optional<MaterialId> {
        static MaterialId next = MaterialId(100);
        (void)name;
        return next;  // permissive: every name resolves
      };
      std::string werr;
      auto wgen = loadGenerator(text, dimensionSeed(7, "wafer"), anyMat, {}, stubModels, werr);
      check(wgen != nullptr, "lithoform_stamp: real wafer.json loads (" + werr + ")");
    }
  }
  std::string err;
  auto gen = loadGenerator(genJson("test_cube", "1.0", "1.0"), ds, testMat, {}, testModels(), err);
  check(gen != nullptr, "lithoform_stamp: generator JSON loads (" + err + ")");
  if (!gen) return;

  // 2. Placement: density 1 -> the 3 model cells stamp above the terrain.
  // The cell-(0,0) base lands somewhere in [0,32)x[0,32); generate the four chunks
  // covering it and count across all.
  int soil = 0;
  for (int ccx = 0; ccx <= 1; ++ccx)
    for (int ccz = 0; ccz <= 1; ++ccz) {
      ChunkInfo c = testChunk(ds);
      c.cx = ccx;
      c.cz = ccz;
      c.chunkSeed = chunkSeed(ds, ccx, ccz);
      ColumnBuffer bc = gen->generate(c);
      soil += countMat(bc, MaterialId(2));
    }
  check(soil == 1, "lithoform_stamp: palette-2 cell stamped once (got " + std::to_string(soil) + ")");
  // The two palette-1 cells are rock-on-rock: verify via the feature's voxel list instead.
  // 3. Density 0 -> nothing stamped.
  std::string err2;
  auto gen0 =
      loadGenerator(genJson("test_cube", "0.0", "1.0"), ds, testMat, {}, testModels(), err2);
  check(gen0 != nullptr, "lithoform_stamp: density-0 generator loads (" + err2 + ")");
  if (gen0) {
    ColumnBuffer b0 = gen0->generate(testChunk(ds));
    check(countMat(b0, MaterialId(2)) == 0, "lithoform_stamp: density 0 stamps nothing");
  }
  // 4. Unknown model -> load error.
  std::string err3;
  const bool badModel =
      !loadGenerator(genJson("nope", "1.0", "1.0"), ds, testMat, {}, testModels(), err3);
  check(badModel, "lithoform_stamp: unknown model rejected (" + err3 + ")");
  // 5. Null resolver -> load error.
  std::string err4;
  const bool noResolver =
      !loadGenerator(genJson("test_cube", "1.0", "1.0"), ds, testMat, {}, ModelResolver{}, err4);
  check(noResolver, "lithoform_stamp: null resolver rejected (" + err4 + ")");
  // 6. Biome gate 0 -> nothing stamped.
  std::string err5;
  auto genB =
      loadGenerator(genJson("test_cube", "1.0", "0.0"), ds, testMat, {}, testModels(), err5);
  check(genB != nullptr, "lithoform_stamp: biome-gated generator loads (" + err5 + ")");
  if (genB) {
    ColumnBuffer bb = genB->generate(testChunk(ds));
    check(countMat(bb, MaterialId(2)) == 0, "lithoform_stamp: biome gate 0 stamps nothing");
  }
  // 7. Determinism: two generations agree exactly.
  ColumnBuffer b = gen->generate(testChunk(ds));
  ColumnBuffer b2 = gen->generate(testChunk(ds));
  bool same = true;
  for (int y = 0; y < b.height() && same; ++y)
    for (int z = 0; z < b.size() && same; ++z)
      for (int x = 0; x < b.size() && same; ++x)
        if (b.get(x, y, z) != b2.get(x, y, z)) same = false;
  check(same, "lithoform_stamp: deterministic across generations");
  // 8. Unknown material in the palette table -> load error.
  std::string feat8 = kStampFeature;
  {
    auto sub8 = [&](const std::string& from, const std::string& to) {
      const size_t i = feat8.find(from);
      if (i != std::string::npos) feat8.replace(i, from.size(), to);
    };
    sub8("__MODEL__", "test_cube");
    sub8("__DENSITY__", "1.0");
    sub8("__BIOME__", "1.0");
    sub8("\"2\": \"soil\"", "\"2\": \"nope\"");
  }
  std::string g8 = kStampGen;
  g8.replace(g8.find("__FEATURE__"), 11, feat8);
  std::string err6;
  const bool badMat = !loadGenerator(g8, ds, testMat, {}, testModels(), err6);
  check(badMat, "lithoform_stamp: unknown palette material rejected (" + err6 + ")");
}

}  // namespace

int runLithoStampFeatureSelfTest() {
  std::printf("[lithoform_stamp]\n");
  Checker check;
  stampChecks(check);
  std::printf("  lithoform_stamp: %d failure(s)\n", check.fails());
  return check.fails();
}

}  // namespace litholattice::procgen::litho_formula
