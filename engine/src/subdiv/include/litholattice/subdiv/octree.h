// Sparse octree subdivision for LithoLattice (JD-locked 2026-10-11).
//
// WHAT: a 1m block subdivides into micro-cells down to 64^3 (6 levels, powers
//   of two), ONLY where detail exists. Empty regions cost nothing.
// WHY: a modeling system — subdivide -> add detail -> save -> place. The
//   armory, silicon trees, mobs, and the ChipOR viewport all build on this.
//   Render/detail-level ONLY: the 512 TPS sim ticks whole blocks; micro-cells
//   are inert geometry.
// HOW: classic sparse octree. Level 0 = the whole block (1 cell). Each level
//   splits every axis in two: level L has (2^L)^3 addressable cells, level 6
//   reaching 64^3. Interior nodes exist only if they have non-empty children.
//
// CONTRACT (all subdivision workstreams code against this header; do not
// change the public API without coordinator approval):
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace litholattice::subdiv {

/// Max subdivision depth: level 6 => 64 micro-cells per axis.
inline constexpr int kMaxLevel = 6;
/// Cells per axis at max depth.
inline constexpr int kMaxCells = 64;

/// One addressed micro-cell. (level, x, y, z): level in [0, kMaxLevel],
/// x/y/z in [0, 2^level). material 0 = empty/clear, nonzero = filled.
struct MicroCell {
  uint8_t level = 0;
  uint8_t x = 0, y = 0, z = 0;
  uint8_t material = 0;  ///< palette/material index (mesher passthrough)
};

/// Sparse octree over one block's volume. Deterministic: iteration order is
/// fixed (level, then z, y, x), no hashing, no thread-locals.
class SparseOctree {
 public:
  SparseOctree() = default;
  // NOTE (workstream 1, 2026-10-11): the contract draft defaulted the three
  // members below. Defaulting them over the owning `root_` pointer would
  // shallow-copy the node graph (copies aliasing each other) and never free
  // nodes. They are declared here and defined in octree.cpp as a deep copy
  // and a freeing destroy instead. The callable public API is unchanged;
  // coordinator: reinstate `= default` only alongside a non-owning Node.
  ~SparseOctree();
  SparseOctree(const SparseOctree&);
  SparseOctree& operator=(const SparseOctree&);

  /// Set the cell at (level,x,y,z) to material (0 clears). Setting a cell
  /// clears any finer cells beneath it; clearing collapses empty parents.
  void set(int level, int x, int y, int z, uint8_t material);

  /// Material at the addressed cell, resolving through coarser ancestors:
  /// returns the finest explicitly-set material covering the address, or 0.
  uint8_t get(int level, int x, int y, int z) const;

  /// All non-empty leaf cells (finest explicitly-set, non-overlapping),
  /// in deterministic order. This is the serialization primitive.
  std::vector<MicroCell> leaves() const;

  /// Number of non-empty leaf cells.
  size_t leafCount() const;
  /// Approximate heap bytes used (for sparse-memory-bound tests).
  size_t memoryBytes() const;
  /// True when no cell is set.
  bool empty() const;

  /// Fill a dense level-L grid (row-major (z*S+y)*S+x, S=2^L) with materials,
  /// resolving through the tree. Out-of-range level => no-op.
  void fillDense(int level, uint8_t* out) const;

 private:
  struct Node;
  Node* root_ = nullptr;
};

/// Deterministic FNV-1a over the leaf stream (for determinism self-tests).
uint64_t hashLeaves(const SparseOctree& tree);

}  // namespace litholattice::subdiv
