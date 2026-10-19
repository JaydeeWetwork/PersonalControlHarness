// Silica: the Wafer's grass equivalent (JD-locked 2026-10-11, workstream 5).
//
// "Silica grass" is tiny silicon fern/shrub ground cover (BlockId 53, Shape::Cross,
// kTransparent|kAttachable|kRandomTicks, not solid). It drops silica seeds (a non-block
// item), spreads via random tick onto silicon-surface blocks, and is farmable:
// till silicon soil -> plant seed -> random-tick growth -> harvest for more seeds.
//
// Id layout (see content/silica_registry.inc for the exact blocks.h / blocks.cpp
// snippets the coordinator appends once the ChipOR workstream's edits land):
//   53  kSilicaGrass      lithokraft:silica_grass  "Silica Grass"
//   54  kSilicaTilledSoil lithokraft:tilled_silicon "Tilled Silicon"
// The constants live here until that hookup; then they move to blocks.h and the
// matching lines below are deleted (the .inc file says exactly where).
//
// All functions below take the caller's BlockRegistry: the game passes
// blockRegistry(), the self-tests pass a test-local registry. Nothing here reads
// the global registry, so silica logic is testable before the hookup lands.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include "lithokraft/content/blocks.h"
#include "litholattice/sim/world_simulation.h"  // DirtyFn
#include "litholattice/world/block.h"
#include "litholattice/world/chunk_map.h"

namespace lithokraft {

// --- ids -----------------------------------------------------------------
// Block ids now live in blocks.h (kSilicaGrass = 53, kSilicaTilledSoil = 54);
// silica.h includes it via chunk_map.h -> block.h. (Moved by coordinator.)
// Non-block item ids live above the BlockId range so the two namespaces never collide
// (ItemStack::itemId is "BlockId for blocks; item ids for non-blocks").
constexpr uint32_t kFirstItemId = uint32_t(1) << 16;
constexpr uint32_t kItemSilicaSeed = kFirstItemId + 0;  ///< silica seed (drops from silica grass)

/// Growth stages, stored in the CellState value bits (stValue): 0 sprout .. 3 mature.
constexpr int kSilicaStageSprout = 0;
constexpr int kSilicaStageMature = 3;

/// Light gate (1-64 scale): silica is a sun-loving silicon fern; it only grows and
/// spreads in bright light. Above half brightness, well clear of the mob-spawn
/// threshold (16) so lit caves stay barren.
constexpr uint8_t kSilicaMinLight = 33;

/// Light query for the sim: 0-64 at (x, y, z). Empty = open-sky fallback
/// (surfaceY(x,z) <= y counts as lit): the engine's ILighting is not plumbed into
/// the sim path yet, so the game passes null today and wires ILighting::lightAt
/// here when it is.
using SilicaLightQuery = std::function<uint8_t(int x, int y, int z)>;
/// Seed-drop sink: itemId, count, position (the game turns these into item entities).
using SilicaDropSink = std::function<void(uint32_t itemId, uint16_t count, int x, int y, int z)>;

inline int silicaGrowthStage(CellState s) { return stValue(s); }

/// Soil silica spreads onto by random tick: the silicon-surface blocks, NOT
/// dead_silicon (dead zones stay dead) and NOT tilled soil (tilling is the
/// player's farming lane; wild spread stays wild).
inline bool silicaIsSpreadSoil(BlockId b) {
  return b == kWaferSilicon || b == kScribeSilicon || b == kDenseSilicon || b == kFleckedSilicon;
}
/// Soil the player can till into farmland: same set (dead zones refuse the hoe too).
inline bool silicaIsTillableSoil(BlockId b) { return silicaIsSpreadSoil(b); }
/// Any silica grass cell (single id today; a range check if variants ever land).
inline bool silicaIsGrass(BlockId b) { return b == kSilicaGrass; }

/// Bright enough to grow/spread at (x, y, z)?
bool silicaLightOk(const ChunkMap& w, int x, int y, int z, const SilicaLightQuery& light);

/// One random tick for the silica grass at (x, y, z): uproot check, growth, then
/// spread when mature. Deterministic in (world state, rnd); never runs on the
/// per-tick path, only via IWorldSimulation::onRandomTick (8 picks/section/tick).
/// Changed cells go to `dirty`; seed drops go to `drop`. Returns true on change.
bool silicaOnRandomTick(ChunkMap& w, const BlockRegistry& reg, int x, int y, int z, uint32_t rnd,
                        const litholattice::IWorldSimulation::DirtyFn& dirty, const SilicaLightQuery& light = {},
                        SilicaDropSink drop = nullptr);

// --- the farming loop -----------------------------------------------------
/// A seed may be planted at (x, y, z): the cell is air and the soil below is tilled.
bool silicaCanPlant(const ChunkMap& w, int x, int y, int z);
/// Plant: stage-0 grass attached to the tilled soil below. Precondition: silicaCanPlant.
void silicaDoPlant(ChunkMap& w, int x, int y, int z, const litholattice::IWorldSimulation::DirtyFn& dirty);
/// Till silicon soil into farmland. False when the block is not tillable (dead stays dead).
bool silicaTill(ChunkMap& w, int x, int y, int z, const litholattice::IWorldSimulation::DirtyFn& dirty);
/// Harvest mature grass: clears the cell, returns seeds granted (0 when immature).
int silicaHarvest(ChunkMap& w, int x, int y, int z, uint32_t rnd, const litholattice::IWorldSimulation::DirtyFn& dirty);
/// Seed yield for breaking grass at `stage` with per-pick randomness `rnd`.
/// Mature: 2-3 (the farmable loop: plant 1, reap 2-3); young: 0-1.
int silicaDropCount(uint32_t rnd, int stage);
/// Deterministic break drops from a position hash (used when the stage is unknown,
/// e.g. the support was broken out from under the grass).
int silicaBreakDrops(int x, int y, int z);

// --- per-biome tint / density ----------------------------------------------
// Wafer biome weight order (matches the "biomes" list in assets/worldgen/wafer.json):
// 0 TraceForest, 1 DicingPlains, 2 CapacitorGroves, 3 ResonatorSpires, 4 DeadZones.
struct SilicaShade {
  uint32_t tint = 0xFFB9C2CE;  ///< ARGB sprite tint for the dominant biome (untinted silver default)
  float density = 0.f;         ///< 0..1 scatter/spread density: dominant-biome weight, 0 in Dead Zones
};
/// Tint = the dominant biome's tint; density = the dominant biome's weight;
/// Dead Zones (index 4) and unknown indices yield density 0 (nothing grows there).
SilicaShade silicaShadeForWeights(const float* weights, size_t n);

}  // namespace lithokraft
