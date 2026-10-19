// lithokraft_silica_selftest: headless Silica workstream tests (no window, no GL).
// Workstream 5 (JD-locked 2026-10-11). Exit code = failure count. LF line endings.
//
// Everything runs against a TEST-LOCAL BlockRegistry (never the global one):
// the registry is built with exactly the rows silica_registry.inc specifies, so
// the snippets the coordinator appends to blocks.cpp/blocks.h are verified here
// before they land.
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "lithokraft/content/silica.h"
#include "lithokraft/game/silica_biome.h"
#include "litholattice/procgen/pipeline.h"
#include "litholattice/sim/random_ticks.h"
#include "litholattice/world/chunk.h"

namespace {
int g_fails = 0;
void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++g_fails;
}

using namespace litholattice;
using namespace lithokraft;

// Test-local registry mirroring blocks.cpp's layout: row 0 = Air, then the real
// soil ids, then the two Silica rows EXACTLY as silica_registry.inc specifies.
BlockRegistry makeSilicaTestRegistry() {
  BlockRegistry r;
  auto def = [](std::string_view id, std::string_view name, uint32_t flags, Shape shape,
                std::string_view tex) {
    return BlockDef{name, flags, shape, tex, {}, 0, 0, 0, 0.8f, false, 0, id};
  };
  constexpr uint32_t kSoil = kSolid | kOpaque;
  r.add(def("lithokraft:silicon", "Wafer Silicon", kSoil, Shape::Cube, "silicon"));  // 1
  for (int i = 2; i <= 43; ++i)
    r.add(def("test:pad" + std::to_string(i), "Pad", kSoil, Shape::Cube, "pad"));
  r.add(def("lithokraft:dead_silicon", "Dead Silicon", kSoil, Shape::Cube, "wafer_ground_dead"));  // 44
  r.add(def("test:pad45", "Pad", kSoil, Shape::Cube, "pad"));                                      // 45
  r.add(def("test:pad46", "Pad", kSoil, Shape::Cube, "pad"));                                      // 46
  r.add(def("lithokraft:scribe_silicon", "Scribe Silicon", kSoil, Shape::Cube,
            "wafer_ground_dicing"));  // 47
  r.add(def("lithokraft:dense_silicon", "Dense Silicon", kSoil, Shape::Cube,
            "wafer_ground_capacitor"));  // 48
  r.add(def("lithokraft:flecked_silicon", "Flecked Silicon", kSoil, Shape::Cube,
            "wafer_ground_resonator"));                                                    // 49
  for (int i = 50; i <= 52; ++i)
    r.add(def("test:pad" + std::to_string(i), "Pad", kSoil, Shape::Cube, "pad"));
  // --- the silica_registry.inc rows (must stay in sync with that file) ---
  r.add(def("lithokraft:silica_grass", "Silica Grass", kTransparent | kAttachable | kRandomTicks,
            Shape::Cross, "silica_grass"));  // 53
  r.add(def("lithokraft:tilled_silicon", "Tilled Silicon", kSoil, Shape::Cube,
            "silica_tilled"));  // 54
  return r;
}

ChunkMap makeWorld() {
  ChunkMap w;
  auto col = std::make_unique<ChunkColumn>();
  col->cx = 0;
  col->cz = 0;
  w.insert(std::move(col));
  return w;
}

struct Drop {
  uint32_t item;
  uint16_t count;
};

// Soil patch at y=10, x/z in [6..10]; returns a world with mature grass at (8,11,8).
ChunkMap makeGrassPatch(const BlockRegistry& reg, BlockId soil) {
  ChunkMap w = makeWorld();
  w.setRegistry(&reg);
  for (int x = 6; x <= 10; ++x)
    for (int z = 6; z <= 10; ++z) w.set(x, 10, z, soil);
  w.set(8, 11, 8, kSilicaGrass);
  w.setState(8, 11, 8, withAttach(makeState(kSilicaStageMature), 1));
  return w;
}

const IWorldSimulation::DirtyFn kNoDirty = [](int, int, int) {};

}  // namespace

int main() {
  const BlockRegistry reg = makeSilicaTestRegistry();
  const SilicaLightQuery kDay = [](int, int, int) { return uint8_t(64); };
  const SilicaLightQuery kDark = [](int, int, int) { return uint8_t(0); };

  // --- 1. registry rows (the silica_registry.inc snippets, verified pre-hookup)
  {
    bool ok = reg.idOf(kSilicaGrass) == "lithokraft:silica_grass" &&
              reg.def(kSilicaGrass).name == "Silica Grass" &&
              reg.def(kSilicaGrass).shape == Shape::Cross &&
              reg.def(kSilicaGrass).tex == "silica_grass" &&
              reg.hasAll(kSilicaGrass, kTransparent | kAttachable | kRandomTicks) &&
              !reg.has(kSilicaGrass, kSolid) &&  // ground cover, never solid
              reg.idOf(kSilicaTilledSoil) == "lithokraft:tilled_silicon" &&
              reg.def(kSilicaTilledSoil).name == "Tilled Silicon" &&
              reg.hasAll(kSilicaTilledSoil, kSolid | kOpaque) &&
              reg.findById("lithokraft:silica_grass") == std::optional<BlockId>(kSilicaGrass) &&
              kSilicaGrass == 53 && kSilicaTilledSoil == 54 &&
              kItemSilicaSeed >= 65536u;  // outside the BlockId range
    check(ok, "registry: silica rows match silica_registry.inc (ids, flags, shape, names)");
  }
  // --- 2. tick budget: grass opts into random ticks; tilled soil does not
  {
    const std::vector<uint8_t> tickable = randomTickableBlocks(reg);
    check(tickable.size() > kSilicaTilledSoil && tickable[kSilicaGrass] != 0 &&
              tickable[kSilicaTilledSoil] == 0 && tickable[kWaferSilicon] == 0,
          "tick budget: silica grass is kRandomTicks (engine picks it); soil never ticks");
  }
  // --- 3. spread onto valid soils; dead silicon / tilled / occupied refuse
  {
    bool ok = true;
    for (BlockId soil : {kWaferSilicon, kScribeSilicon, kDenseSilicon, kFleckedSilicon}) {
      ChunkMap w = makeGrassPatch(reg, kWaferSilicon);
      w.set(9, 10, 8, soil);  // the spread target's soil (rnd=0 picks neighbour (9,11,8))
      int dirty = 0;
      // rnd=0: growth bits (0&3)==0 would grow, but stage is mature -> spread path;
      // (0>>3)&7==0 -> spread attempt; neighbour (0>>6)&7=0 -> (9,11,8).
      ok = ok && silicaOnRandomTick(w, reg, 8, 11, 8, 0, [&](int, int, int) { ++dirty; }, kDay);
      ok = ok && w.get(9, 11, 8) == kSilicaGrass &&
           silicaGrowthStage(w.getState(9, 11, 8)) == kSilicaStageSprout && dirty == 1;
    }
    for (BlockId badSoil : {kDeadSilicon, kSilicaTilledSoil}) {
      ChunkMap w = makeGrassPatch(reg, kWaferSilicon);
      w.set(9, 10, 8, badSoil);
      int dirty = 0;
      ok = ok && !silicaOnRandomTick(w, reg, 8, 11, 8, 0, [&](int, int, int) { ++dirty; }, kDay);
      ok = ok && w.get(9, 11, 8) == kAir && dirty == 0;
    }
    {  // occupied target cell: no spread
      ChunkMap w = makeGrassPatch(reg, kWaferSilicon);
      w.set(9, 11, 8, kWaferSilicon);
      int dirty = 0;
      ok = ok && !silicaOnRandomTick(w, reg, 8, 11, 8, 0, [&](int, int, int) { ++dirty; }, kDay);
      ok = ok && w.get(9, 11, 8) == kWaferSilicon && dirty == 0;
    }
    check(ok, "spread: onto silicon/scribe/dense/flecked; refuses dead_silicon, tilled, occupied");
  }
  // --- 4. light gate: dark -> dormant
  {
    ChunkMap w = makeGrassPatch(reg, kWaferSilicon);
    int dirty = 0;
    bool changed =
        silicaOnRandomTick(w, reg, 8, 11, 8, 0, [&](int, int, int) { ++dirty; }, kDark);
    check(!changed && dirty == 0 && w.get(8, 11, 8) == kSilicaGrass,
          "light gate: no growth/spread in the dark");
  }
  // --- 5. growth stages advance 1/4 of ticks, deterministic
  {
    ChunkMap w = makeGrassPatch(reg, kWaferSilicon);
    w.setState(8, 11, 8, withAttach(makeState(0), 1));
    int dirty = 0;
    bool grew =
        silicaOnRandomTick(w, reg, 8, 11, 8, 0, [&](int, int, int) { ++dirty; }, kDay);
    bool ok = grew && silicaGrowthStage(w.getState(8, 11, 8)) == 1 && dirty == 1;
    ok = ok && !silicaOnRandomTick(w, reg, 8, 11, 8, 1, [&](int, int, int) { ++dirty; }, kDay) &&
         silicaGrowthStage(w.getState(8, 11, 8)) == 1;  // rnd=1: (1&3)!=0, no growth
    check(ok, "growth: stage advances on (rnd&3)==0, holds otherwise");
  }
  // --- 6. uproot: soil gone -> grass pops, sheds seeds through the drop sink
  {
    ChunkMap w = makeGrassPatch(reg, kWaferSilicon);
    w.set(8, 10, 8, kAir);
    std::vector<Drop> drops;
    int dirty = 0;
    bool changed = silicaOnRandomTick(
        w, reg, 8, 11, 8, 0, [&](int, int, int) { ++dirty; }, kDay,
        [&](uint32_t item, uint16_t n, int, int, int) { drops.push_back({item, n}); });
    bool ok = changed && w.get(8, 11, 8) == kAir && dirty == 1 && drops.size() == 1 &&
              drops[0].item == kItemSilicaSeed && drops[0].count >= 2;  // was mature: 2-3
    check(ok, "uproot: unsupported grass pops and sheds its seeds");
  }
  // --- 7. till / plant / harvest round-trip (the farmable loop)
  {
    ChunkMap w = makeGrassPatch(reg, kWaferSilicon);
    int dirty = 0;
    auto d = [&](int, int, int) { ++dirty; };
    bool ok = silicaTill(w, 8, 10, 8, d) && w.get(8, 10, 8) == kSilicaTilledSoil;
    ok = ok && !silicaTill(w, 8, 10, 8, d);  // already tilled: not tillable again
    w.set(6, 10, 6, kDeadSilicon);
    ok = ok && !silicaTill(w, 6, 10, 6, d);  // dead zones stay dead
    w.set(8, 11, 8, kAir);  // clear the patch's pre-planted grass: start the loop from seed
    ok = ok && silicaCanPlant(w, 8, 11, 8) && !silicaCanPlant(w, 7, 11, 8);  // untilled refuses
    silicaDoPlant(w, 8, 11, 8, d);
    ok = ok && w.get(8, 11, 8) == kSilicaGrass &&
         silicaGrowthStage(w.getState(8, 11, 8)) == kSilicaStageSprout;
    ok = ok && silicaHarvest(w, 8, 11, 8, 0, d) == 0 &&  // sprout: nothing to reap
         w.get(8, 11, 8) == kSilicaGrass;
    // Force-grow to mature with favourable ticks (rnd=0 always grows).
    for (int i = 0; i < 8 && silicaGrowthStage(w.getState(8, 11, 8)) < kSilicaStageMature; ++i)
      silicaOnRandomTick(w, reg, 8, 11, 8, 0, d, kDay);
    ok = ok && silicaGrowthStage(w.getState(8, 11, 8)) == kSilicaStageMature;
    const int seeds = silicaHarvest(w, 8, 11, 8, 0, d);
    ok = ok && seeds >= 2 && seeds <= 3 && w.get(8, 11, 8) == kAir;
    ok = ok && silicaCanPlant(w, 8, 11, 8);  // the loop closes: replant on the same soil
    check(ok, "farm loop: till -> plant -> grow -> harvest (2-3 seeds) -> replant");
  }
  // --- 8. drop rates sane across the rnd space
  {
    long matureSum = 0, youngMax = 0;
    bool bounds = true;
    for (uint32_t r = 0; r < 4096; ++r) {
      const int m = silicaDropCount(r, kSilicaStageMature);
      const int y2 = silicaDropCount(r, kSilicaStageMature - 1);
      const int y0 = silicaDropCount(r, 0);
      bounds = bounds && m >= 2 && m <= 3 && y2 >= 1 && y2 <= 2 && y0 >= 0 && y0 <= 1;
      matureSum += m;
      if (y0 > youngMax) youngMax = y0;
    }
    const double mean = double(matureSum) / 4096.0;
    check(bounds && mean >= 2.0 && mean <= 3.0 && youngMax <= 1,
          "drops: mature 2-3 (mean in range), young 0-1");
  }
  // --- 9. determinism: identical (world, rnd) -> identical outcome
  {
    auto run = [&]() {
      ChunkMap w = makeGrassPatch(reg, kWaferSilicon);
      silicaOnRandomTick(w, reg, 8, 11, 8, 1234567, kNoDirty, kDay);
      std::string snap;
      for (int x = 6; x <= 10; ++x)
        for (int y = 10; y <= 12; ++y)
          for (int z = 6; z <= 10; ++z) {
            snap.push_back(char(w.get(x, y, z)));
            const CellState s = w.getState(x, y, z);
            snap.push_back(char(s & 0xff));
            snap.push_back(char(s >> 8));
          }
      return snap;
    };
    check(run() == run(), "determinism: same rnd twice -> identical world");
  }
  // --- 10. per-biome tint/density from the weight vector
  {
    const float forest[5] = {1, 0, 0, 0, 0};
    const float plains[5] = {0.1f, 0.6f, 0.1f, 0.1f, 0.1f};
    const float grove[5] = {0, 0, 0.8f, 0.1f, 0.1f};
    const float spire[5] = {0, 0, 0.1f, 0.9f, 0};
    const float dead[5] = {0, 0, 0, 0, 1};
    const SilicaShade f = silicaShadeForWeights(forest, 5), p = silicaShadeForWeights(plains, 5),
                      g = silicaShadeForWeights(grove, 5), s = silicaShadeForWeights(spire, 5),
                      dd = silicaShadeForWeights(dead, 5), nn = silicaShadeForWeights(nullptr, 0);
    bool ok = f.tint == 0xFF8FE3B0 && f.density == 1.f && p.tint == 0xFFD9D9E2 && p.density == 0.6f &&
              g.tint == 0xFFE8A34C && s.tint == 0xFFA87FE8 && dd.density == 0.f && nn.density == 0.f &&
              f.tint != p.tint && p.tint != g.tint && g.tint != s.tint;  // one tint per biome
    check(ok, "biome shade: per-biome tint, density = dominant weight, 0 in Dead Zones");
  }
  // --- 11. scatter density from a ColumnMaps (the decoration pass's query)
  {
    procgen::ColumnMaps maps;
    maps.biomeCount = 5;
    maps.biome = {4, 0};  // column 0: dead, column 1: forest
    maps.biomeWeight = {0, 0, 0, 0, 1,   // dead
                        1, 0, 0, 0, 0};  // forest
    bool ok = silicaScatterDensity(maps, 0) == 0.f && silicaScatterDensity(maps, 1) == 1.f;
    // No blending data: falls back to the dominant biome.
    procgen::ColumnMaps bare;
    bare.biome = {2};
    ok = ok && silicaScatterDensity(bare, 0) == 1.f;
    check(ok, "scatter density: weight-driven, 0 in Dead Zones, dominant fallback");
  }
  // --- 12. shade cache: deterministic, thread-safe shape
  {
    SilicaShadeCache cache(nullptr);  // no generator: blends the empty maps -> density 0
    const SilicaShade a = cache.shade(3, -7), b = cache.shade(3, -7);
    check(a.tint == b.tint && a.density == b.density && a.density == 0.f,
          "shade cache: same chunk twice -> same shade (deterministic)");
  }
  // --- 13. non-grass cells never tick as grass
  {
    ChunkMap w = makeGrassPatch(reg, kWaferSilicon);
    int dirty = 0;
    check(!silicaOnRandomTick(w, reg, 8, 10, 8, 0, [&](int, int, int) { ++dirty; }, kDay) &&
              dirty == 0,
          "dispatch guard: silica tick on a soil cell is a no-op");
  }

  std::printf(g_fails ? "SILICA SELFTEST FAILED (%d)\n" : "SILICA SELFTEST OK\n", g_fails);
  return g_fails;
}
