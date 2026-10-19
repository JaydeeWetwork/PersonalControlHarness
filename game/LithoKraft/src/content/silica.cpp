// Silica grass simulation: growth, random-tick spread, and the till/plant/harvest
// farming loop (JD-locked 2026-10-11, workstream 5). LF line endings.
//
// Everything here is deterministic in (world state, rnd): the engine hands each
// random tick a per-pick `rnd` (deterministic per tick+section+pick), and every
// decision below derives from it. Spread and growth NEVER run on the per-tick
// path -- only through IWorldSimulation::onRandomTick, which the engine calls for
// the 8 picked cells per section per world tick (kDefaultRandomTicksPerSection).
#include "lithokraft/content/silica.h"

namespace lithokraft {
namespace {

// splitmix64: the same deterministic hash the biome pass uses (wafer_biomes.cpp).
uint64_t splitmix64(uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  uint64_t z = x;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

uint64_t posHash(int x, int y, int z) {
  const uint64_t px = uint64_t(uint32_t(x)), py = uint64_t(uint32_t(y)), pz = uint64_t(uint32_t(z));
  return splitmix64(px ^ (py * 0x9E3779B97F4A7C15ull) ^ (pz << 32));
}

// The 8 horizontal neighbours, in a fixed order.
constexpr int kNbr[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};

CellState grassState(int stage) { return withAttach(makeState(stage), 1); }  // mounted on the soil below

bool supported(const ChunkMap& w, const BlockRegistry& reg, int x, int y, int z) {
  return reg.has(w.get(x, y - 1, z), kSolid);
}

}  // namespace

bool silicaLightOk(const ChunkMap& w, int x, int y, int z, const SilicaLightQuery& light) {
  if (light) return light(x, y, z) >= kSilicaMinLight;
  // Open-sky fallback: the cell sits at or above the column's surface, so it sees
  // the sky. (The engine's ILighting is not plumbed into the sim path yet.)
  return w.surfaceY(x, z) <= y;
}

bool silicaOnRandomTick(ChunkMap& w, const BlockRegistry& reg, int x, int y, int z, uint32_t rnd,
                        const litholattice::IWorldSimulation::DirtyFn& dirty, const SilicaLightQuery& light, SilicaDropSink drop) {
  if (w.get(x, y, z) != kSilicaGrass) return false;  // defensive: only grass ticks here
  const int stage = silicaGrowthStage(w.getState(x, y, z));
  if (!supported(w, reg, x, y, z)) {
    // Uprooted: the soil below is gone. Pop to air and shed seeds (stage known here).
    w.set(x, y, z, kAir);
    dirty(x, y, z);
    if (drop) {
      const int n = silicaDropCount(uint32_t(posHash(x, y, z)), stage);
      if (n > 0) drop(kItemSilicaSeed, uint16_t(n), x, y, z);
    }
    return true;
  }
  if (!silicaLightOk(w, x, y, z, light)) return false;  // dark: dormant, like MC grass
  if (stage < kSilicaStageMature) {
    if ((rnd & 3u) != 0) return false;  // 1/4 of ticks: one growth step
    w.setState(x, y, z, grassState(stage + 1));
    dirty(x, y, z);
    return true;
  }
  // Mature: try to spread. 1/8 of ticks, one of the 8 neighbours by (rnd >> 6).
  if (((rnd >> 3) & 7u) != 0) return false;
  const int* d = kNbr[(rnd >> 6) & 7u];
  const int tx = x + d[0], tz = z + d[1];
  if (w.get(tx, y, tz) != kAir) return false;
  if (!silicaIsSpreadSoil(w.get(tx, y - 1, tz))) return false;  // dead silicon refuses
  if (!silicaLightOk(w, tx, y, tz, light)) return false;
  w.set(tx, y, tz, kSilicaGrass);
  w.setState(tx, y, tz, grassState(kSilicaStageSprout));
  dirty(tx, y, tz);
  return true;
}

bool silicaCanPlant(const ChunkMap& w, int x, int y, int z) {
  return w.get(x, y, z) == kAir && w.get(x, y - 1, z) == kSilicaTilledSoil;
}

void silicaDoPlant(ChunkMap& w, int x, int y, int z, const litholattice::IWorldSimulation::DirtyFn& dirty) {
  w.set(x, y, z, kSilicaGrass);
  w.setState(x, y, z, grassState(kSilicaStageSprout));
  dirty(x, y, z);
}

bool silicaTill(ChunkMap& w, int x, int y, int z, const litholattice::IWorldSimulation::DirtyFn& dirty) {
  if (!silicaIsTillableSoil(w.get(x, y, z))) return false;  // dead zones stay dead
  w.set(x, y, z, kSilicaTilledSoil);
  dirty(x, y, z);
  return true;
}

int silicaHarvest(ChunkMap& w, int x, int y, int z, uint32_t rnd, const litholattice::IWorldSimulation::DirtyFn& dirty) {
  if (w.get(x, y, z) != kSilicaGrass) return 0;
  if (silicaGrowthStage(w.getState(x, y, z)) < kSilicaStageMature) return 0;  // not ripe: nothing
  const int n = silicaDropCount(rnd, kSilicaStageMature);
  w.set(x, y, z, kAir);
  dirty(x, y, z);
  return n;
}

int silicaDropCount(uint32_t rnd, int stage) {
  if (stage >= kSilicaStageMature) return 2 + int(rnd & 1u);          // 2-3: the farmable loop
  if (stage == kSilicaStageMature - 1) return 1 + int(rnd & 1u);      // 1-2
  return ((rnd & 7u) == 0) ? 1 : 0;                                  // sprouts: usually nothing
}

int silicaBreakDrops(int x, int y, int z) {
  (void)x; (void)y; (void)z;
  return 1;  // pop policy: broken/popped grass always sheds exactly one seed
}

SilicaShade silicaShadeForWeights(const float* weights, size_t n) {
  // Per-biome sprite tints (ARGB). Capacitor Groves = JD's orange, Resonator
  // Spires = JD's purple; the others read as silicon vegetation on their ground.
  static constexpr uint32_t kTints[4] = {
      0xFF8FE3B0,  // TraceForest: pale silicon green
      0xFFD9D9E2,  // DicingPlains: warm silver
      0xFFE8A34C,  // CapacitorGroves: amber
      0xFFA87FE8,  // ResonatorSpires: violet
  };
  if (!weights || n == 0) return SilicaShade{};
  size_t best = 0;
  for (size_t i = 1; i < n; ++i)
    if (weights[i] > weights[best]) best = i;
  if (best >= 4) return SilicaShade{0xFF808080, 0.f};  // DeadZones (4) or unknown: barren
  return SilicaShade{kTints[best], weights[best]};
}

}  // namespace lithokraft
