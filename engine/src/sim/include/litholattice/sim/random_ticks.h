// Random ticks: every world tick, `perSection` cells of each non-empty 16^3 section around the
// player are picked at random; cells whose block opted in (kRandomTicks) are handed to the
// game (IWorldSimulation::onRandomTick). Runs at the world TPS only (never in the faster clock
// domains, so beacons / chip domains do not scale it). Deterministic per (tick, section).
#pragma once
#include <cstdint>
#include <functional>
#include <vector>
#include "litholattice/world/block.h"
#include "litholattice/world/chunk_map.h"

namespace litholattice {

constexpr int kDefaultRandomTicksPerSection = 8;

/// tickable[id] != 0 for blocks with kRandomTicks.
std::vector<uint8_t> randomTickableBlocks(const BlockRegistry& blocks);

/// Called for each picked opted-in cell: world position, its block and a per-pick random value.
using RandomTickFn = std::function<void(int x, int y, int z, BlockId block, uint32_t rnd)>;

/// Cache of ticking column pointers, rebuilt when the tick area changes. The column
/// set is fixed while the sim thread runs (dimension load inserts everything up
/// front), so raw pointers are stable across ticks; the column-count check guards
/// against a streaming future. Iterates in the same cz/cx order as the uncached
/// path, so results are bit-identical.
struct RandomTickCache {
  int pcx = 0, pcz = 0, radius = -1;
  size_t columnCount = 0;
  struct Col {
    int cx = 0, cz = 0;
    const ChunkColumn* col = nullptr;
  };
  std::vector<Col> cols;
};

/// One world tick's random ticks in the square of `radius` chunks around (pcx, pcz). Returns the
/// number of cells sampled (opted in or not). Pass a cache to skip the per-column
/// hash lookups on ticks where the area didn't change (may be nullptr).
/// `reg` translates the StableId voxel storage to the BlockId currency of `tickable`
/// and `fn` (palette resolved once per sub-chunk, then O(1) per pick).
uint64_t runRandomTicks(const ChunkMap& world, const BlockRegistry& reg, const std::vector<uint8_t>& tickable,
                        int pcx, int pcz, int radius, uint64_t tickIndex, int perSection, const RandomTickFn& fn,
                        RandomTickCache* cache = nullptr);

}  // namespace litholattice
