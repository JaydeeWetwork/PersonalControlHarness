// Chunk storage: 16x16 columns, world height 0..767 split into 48 sub-chunks of 16^3
// (all-air sub-chunks are null and skipped by worldgen, meshing and the sim).
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include "litholattice/world/block.h"
#include "litholattice/world/voxel_palette.h"
#include <optional>
#include <vector>

namespace litholattice {

constexpr int kChunkSize = 16;
constexpr int kWorldHeight = 768;
constexpr int kSubChunks = kWorldHeight / kChunkSize;  // 48
constexpr int kSubChunkVolume = kChunkSize * kChunkSize * kChunkSize;  // 4096


// Per-cell state: CellState (block.h); its meaning beyond the standard layout is the game's.
// ---- Sub-chunk: width-tagged paletted voxels (Infinite Registry, phase 1) ----
// Index layout y-major: idx = (y << 8) | (z << 4) | x  -> contiguous X runs for SIMD.
// Voxel storage is a VoxelPalette: 8/16/32/64-bit indices chosen from the palette
// size, palette maps index -> StableId (at 64-bit width the index IS the StableId).
// The engine's BlockId currency is translated at the ChunkMap boundary (get/set keep
// their BlockId signatures); direct sub-chunk readers resolve the palette once per
// sub-chunk. Full game-logic migration to StableIds is WS1 (future work).
struct SubChunk {
  VoxelPalette blocks{kSubChunkVolume};
  std::array<CellState, kSubChunkVolume> state{};  // see CellState helpers in block.h
  // PBR chunk materials (phase 2): per-voxel 8-bit index into the column's materialPalette
  // (below). A parallel byte array, NOT packed into the block word: BlockId is a dense 16-bit
  // palette index with no spare bits, so packing would invalidate every existing save and the
  // BLKS palette codec for zero benefit. 4 KiB per present sub-chunk; 0 is the palette's first
  // entry (the built-in default when the column palette is empty).
  std::array<uint8_t, kSubChunkVolume> materials{};
  uint16_t nonAir = 0;   // 0 => sub-chunk is freed / skipped entirely

  static constexpr int index(int x, int y, int z) { return (y << 8) | (z << 4) | x; }
  StableId at(int x, int y, int z) const { return blocks.at(size_t(index(x, y, z))); }
  void set(int x, int y, int z, StableId id) {
    const size_t i = size_t(index(x, y, z));
    const StableId c = blocks.at(i);
    nonAir += uint16_t((id != kAirStableId) - (c != kAirStableId));
    blocks.set(i, id);
    state[index(x, y, z)] = 0;
    // materials[] is left untouched: the material byte is the caller's to assign (a replaced
    // block keeps its predecessor's material until reassigned; fresh voxels read 0).
  }
  /// 8-bit material palette index of one voxel (indexes ChunkColumn::materialPalette).
  uint8_t materialAt(int x, int y, int z) const { return materials[index(x, y, z)]; }
  void setMaterial(int x, int y, int z, uint8_t m) { materials[index(x, y, z)] = m; }

  // Transient load scratch (Infinite Registry): the BLKS decoder parks save-time
  // BlockIds here; remapColumn() consumes them into `blocks` (the StableId paletted
  // storage) and clears it. Empty during normal operation.
  std::optional<std::vector<uint16_t>> loadScratch;
};

struct ChunkColumn {
  int cx = 0, cz = 0;
  // Column eviction bookkeeping (A2 / B-M1, 2026-10-13): `dirty` is set by
  // ChunkMap::set/setState on every mutation of a RESIDENT column, and
  // `generation` counts those mutations. A freshly generated or disk-loaded
  // column starts clean (dirty == false, generation == 0): it is exactly
  // reproducible (worldgen) or already on disk, so eviction may drop it
  // without saving. A dirty column must be saved before it is evicted
  // (world permanence: never drop unsaved state). Direct SubChunk writes
  // that bypass ChunkMap (the signal sim's cached-pointer fast paths) do
  // NOT set this flag -- eviction therefore ALSO treats any column holding
  // signal-sim-indexed cells as save-first (see Game::evictFarColumns).
  bool dirty = false;
  uint64_t generation = 0;
  void markDirty() {
    dirty = true;
    ++generation;
  }
  std::array<std::unique_ptr<SubChunk>, kSubChunks> subs;  // nullptr == all air
  // PBR chunk materials (phase 2): the per-column palette every SubChunk::materials byte
  // indexes into (phase 1's ChunkPalette, stored as plain data so this header stays free of
  // the render module). Byte i -> the global MaterialRegistry ID materialPalette[i].
  // materialPaletteSize == 0 (e.g. a column from an old save with no MATS payload) means every
  // byte maps to global ID 0, which initMaterialRegistry() guarantees is the built-in default.
  std::array<uint16_t, 256> materialPalette{};
  size_t materialPaletteSize = 0;  // valid entries in materialPalette (0..256)
};

struct ChunkKey {
  static uint64_t make(int cx, int cz) {
    return (uint64_t(uint32_t(cx)) << 32) | uint64_t(uint32_t(cz));
  }
};

// Packed world-cell (or sub-chunk) coordinate key for hash sets.
// RANGE (P3-2 B-L2): x and z carry 21 bits each with a +2^20 bias, y 10 bits.
// A coordinate is representable exactly iff x,z in [-2^20, 2^20-1] and
// y in [0, 1023]. OUTSIDE that range packPos ALIASES: two different cells
// map to the same key (e.g. x = 2^20 collides with x = -2^20), silently
// merging signal components, light sources and dirty marks for both cells.
// Every dimension's declared bounds must fit inside this range — the
// bounded dimensions do by an order of magnitude (largest: the Cosmic at
// +/-262144) — and the enforcement points are dimension/streamer setup
// (ChunkStreamer refuses out-of-range request centres, loudly, once) and
// packPosInRange() for new call sites. The VOID is the at-risk design:
// its locked "infinite" extent cannot be represented in a packPos key at
// all; before the Void is registered it must be given a finite bound
// inside this range (or a windowed/virtualised coordinate scheme) — see
// game dimensions.h.
constexpr int kPackPosCoordMin = -(1 << 20);        // -1048576
constexpr int kPackPosCoordMax = (1 << 20) - 1;     //  1048575
constexpr int kPackPosYMax = 1023;
inline bool packPosInRange(int x, int y, int z) {
  return x >= kPackPosCoordMin && x <= kPackPosCoordMax && z >= kPackPosCoordMin &&
         z <= kPackPosCoordMax && y >= 0 && y <= kPackPosYMax;
}
inline int64_t packPos(int x, int y, int z) {
  return (int64_t(x + (1 << 20)) << 31) | (int64_t(z + (1 << 20)) << 10) | int64_t(y);  // y: 10 bits
}
inline void unpackPos(int64_t k, int& x, int& y, int& z) {
  y = int(k & 1023); z = int((k >> 10) & ((1 << 21) - 1)) - (1 << 20); x = int(k >> 31) - (1 << 20);
}

}  // namespace litholattice
