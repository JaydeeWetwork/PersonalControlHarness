#include "litholattice/sim/random_ticks.h"

namespace litholattice {
namespace {
uint64_t splitmix(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}
}  // namespace

std::vector<uint8_t> randomTickableBlocks(const BlockRegistry& blocks) {
  std::vector<uint8_t> t(blocks.size(), 0);
  for (size_t i = 0; i < t.size(); ++i) t[i] = blocks.has(BlockId(i), kRandomTicks) ? 1 : 0;
  return t;
}

uint64_t runRandomTicks(const ChunkMap& world, const BlockRegistry& reg, const std::vector<uint8_t>& tickable,
                        int pcx, int pcz, int radius, uint64_t tickIndex, int perSection, const RandomTickFn& fn,
                        RandomTickCache* cache) {
  if (perSection <= 0) return 0;
  // Column-pointer cache: one hash lookup per column per area change instead of
  // per column per tick. Rebuilt when the area moves or the column set changes.
  const RandomTickCache::Col* cols = nullptr;
  size_t ncols = 0;
  if (cache && cache->pcx == pcx && cache->pcz == pcz && cache->radius == radius &&
      cache->columnCount == world.columns().size()) {
    cols = cache->cols.data();
    ncols = cache->cols.size();
  } else {
    static thread_local std::vector<RandomTickCache::Col> scratch;
    scratch.clear();
    for (int cz = pcz - radius; cz <= pcz + radius; ++cz)
      for (int cx = pcx - radius; cx <= pcx + radius; ++cx)
        if (const ChunkColumn* col = world.column(cx, cz)) scratch.push_back({cx, cz, col});
    if (cache) {
      cache->pcx = pcx; cache->pcz = pcz; cache->radius = radius;
      cache->columnCount = world.columns().size();
      cache->cols = scratch;
    }
    cols = scratch.data();
    ncols = scratch.size();
  }
  uint64_t sampled = 0;
  std::vector<BlockId> lut;  // reused across sub-chunks; capacity retained
  for (size_t ci = 0; ci < ncols; ++ci) {
    const int cx = cols[ci].cx, cz = cols[ci].cz;
    const ChunkColumn* col = cols[ci].col;
    for (int sy = 0; sy < kSubChunks; ++sy) {
      const SubChunk* sub = col->subs[size_t(sy)].get();
      if (!sub || !sub->nonAir) continue;  // all-air sections cost nothing
      // Resolve the StableId palette to BlockIds once; picks below are O(1).
      const bool w64 = sub->blocks.width() == VoxelPalette::Width::W64;
      if (!w64) sub->blocks.resolveBlockIds(reg, lut);
      uint64_t s = splitmix(tickIndex * 0x100000001b3ULL ^ uint64_t(packPos(cx, sy, cz)));
      for (int i = 0; i < perSection; ++i) {
        s = splitmix(s);
        const int idx = int(s & 4095);
        ++sampled;
        BlockId b;
        if (w64) {  // degenerate width: index IS the StableId (unreachable in practice)
          const auto id = reg.findByStableId(sub->blocks.at(size_t(idx)));
          b = id ? *id : kAir;
        } else {
          b = lut[sub->blocks.rawIndex(size_t(idx))];
        }
        if (b >= tickable.size() || !tickable[b]) continue;
        fn(cx * 16 + (idx & 15), sy * 16 + (idx >> 8), cz * 16 + ((idx >> 4) & 15), b, uint32_t(s >> 32));
      }
    }
  }
  return sampled;
}

}  // namespace litholattice
