// Immutable snapshot of a sub-chunk plus a 1-cell border (18^3 ids + state bytes), taken
// by the thread that owns the ChunkMap. Mesh workers read only this, never the live map.
#pragma once
#include <cstdint>
#include <memory>
#include "litholattice/world/chunk_map.h"

namespace litholattice {

constexpr int kMeshPad = kChunkSize + 2;  // 18
constexpr int kMeshPadVolume = kMeshPad * kMeshPad * kMeshPad;
inline int meshPadIndex(int x, int y, int z) { return (y * kMeshPad + z) * kMeshPad + x; }

struct MeshInput {
  int cx = 0, sy = 0, cz = 0;
  uint32_t epoch = 0;   // dimension load generation (stale results are dropped)
  uint64_t seq = 0;     // newer snapshot of the same sub-chunk wins
  BlockId id[kMeshPadVolume];
  CellState st[kMeshPadVolume];
  uint8_t mat[kMeshPadVolume];  // per-voxel 8-bit material palette index (PBR chunk materials);
                               // 0 when the source cell has none
  // Column material palette for the BinaryQuad path (PBR phase 2, Worker 3): matPalette[i]
  // maps a mat[] byte to a compact MaterialRegistry ID. Copied at snapshot time so mesh
  // workers never touch the live ChunkMap (same immutability contract as id[]/st[]/mat[]).
  // matPaletteSize == 0 (old save, no MATS payload) means every byte resolves to ID 0.
  uint16_t matPalette[256]{};
  size_t matPaletteSize = 0;
};

/// Fill `out` with the snapshot (out-param form, for pooled callers that recycle the
/// ~24 KB MeshInput instead of make_shared-ing one per snapshot).
/// `reg` translates the StableId voxel storage to the BlockId currency of MeshInput
/// (resolved once per sub-chunk palette, then O(1) per voxel).
void snapshotForMeshingInto(const ChunkMap& w, const BlockRegistry& reg, int cx, int sy, int cz,
                            uint32_t epoch, uint64_t seq, BlockId belowWorld, MeshInput& out);

/// Snapshot of sub-chunk (cx, sy, cz) with its border. Cells below y = 0 read as `belowWorld`
/// (the dimension's fill block; a solid one keeps the world's bottom faces from being meshed).
std::shared_ptr<MeshInput> snapshotForMeshing(const ChunkMap& w, const BlockRegistry& reg, int cx, int sy,
                                              int cz, uint32_t epoch, uint64_t seq,
                                              BlockId belowWorld = kAir);

}  // namespace litholattice
