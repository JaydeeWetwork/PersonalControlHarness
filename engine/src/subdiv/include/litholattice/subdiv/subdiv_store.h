#pragma once
// SubdivStore: chunk-scoped map from block-local coords to SparseOctree
// (workstream 1, 2026-10-11).
//
// WHAT: attaches micro-detail (one SparseOctree per 1m block) to the 16^3
//   blocks of a chunk. This is what the model pipeline stamps detail into
//   and what the mesher reads.
// WHY: blocks are addressed chunk-locally (x, y, z in 0..15); the store keeps
//   entries sorted by (z, y, x) so iteration and serialization are
//   deterministic.
// Serialized form (versioned byte stream):
//   magic "LTHS" (4 bytes), u16 version = 1 (LE), u32 entry count (LE),
//   then per entry: u8 x, u8 y, u8 z, u32 leaf count (LE),
//   then leaves as 5-byte records (level, x, y, z, material) in leaves()
//   order. deserialize() validates everything and leaves *this unchanged on
//   any malformation.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "litholattice/subdiv/octree.h"

namespace litholattice::subdiv {

class SubdivStore {
 public:
  SubdivStore() = default;

  /// Attach (or replace) the octree for block (x, y, z). Out-of-range coords
  /// are ignored; storing an empty octree removes the entry.
  void setBlockOctree(int x, int y, int z, SparseOctree octree);

  /// The octree for block (x, y, z), or nullptr when there is none or the
  /// coords are out of range.
  const SparseOctree* getBlockOctree(int x, int y, int z) const;

  /// Drop the entry for block (x, y, z). Returns false when there was no
  /// entry or the coords are out of range.
  bool removeBlockOctree(int x, int y, int z);

  /// Number of blocks carrying subdivision.
  size_t blockCount() const { return entries_.size(); }

  /// Approximate heap bytes used.
  size_t memoryBytes() const;

  /// Serialize to the versioned byte stream described above.
  std::vector<uint8_t> serialize() const;

  /// Parse a serialize() stream. Returns false (leaving *this unchanged) on
  /// any malformation: short reads, bad magic, unknown version, out-of-range
  /// coords/cells, or trailing garbage.
  bool deserialize(const uint8_t* data, size_t size);

 private:
  struct Entry {
    uint8_t x = 0, y = 0, z = 0;
    SparseOctree tree;
  };
  std::vector<Entry> entries_;  // sorted by (z, y, x)

  static bool validCoord(int v) { return v >= 0 && v < 16; }
  // Lower-bound index for (x, y, z) in the (z, y, x)-sorted entries_.
  size_t findIndex(int x, int y, int z) const;
};

}  // namespace litholattice::subdiv
