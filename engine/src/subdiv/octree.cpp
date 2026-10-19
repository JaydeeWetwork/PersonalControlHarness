// Sparse octree subdivision for LithoLattice (workstream 1, JD-locked 2026-10-11).
//
// Replaces workstream 2's stopgap (2026-10-11): the real implementation.
// Public contract in octree.h — unchanged callable API.
//
// Implementation notes:
// - Nodes live in one per-tree pool (vector<Cell>); children are cell indices.
//   Copying a tree deep-copies the pool, so trees are true values: the store
//   can hold one copy while the caller keeps mutating its own. (This is why
//   the header declares — rather than defaults — the destructor and copies;
//   see the header note.)
// - Interior nodes always carry material 0; a nonzero material lives only on
//   leaves. Refining a nonzero leaf pushes its material down into all eight
//   children, so the surrounding volume keeps its value.
// - Clearing a cell collapses ancestors whose children are all empty leaves,
//   so an emptied tree shrinks back to ~tens of bytes. Freed cells go on a
//   free-list and are reused by later sets.
// - Determinism is structural: no hashing, no thread-locals, no RNG. leaves()
//   is sorted (level, z, y, x); every other observable follows from it.

#include "litholattice/subdiv/octree.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

namespace litholattice::subdiv {

struct SparseOctree::Node {
  struct Cell {
    uint8_t material = 0;
    int32_t child[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  };
  std::vector<Cell> cells;        // cells[0] is the level-0 root cell
  std::vector<int32_t> freeList;  // recycled cell indices

  int32_t alloc() {
    if (!freeList.empty()) {
      const int32_t i = freeList.back();
      freeList.pop_back();
      cells[(size_t)i] = Cell{};
      return i;
    }
    const int32_t i = (int32_t)cells.size();
    cells.emplace_back();
    return i;
  }

  void release(int32_t idx) {
    // Iterative: reset the whole subtree and recycle its indices.
    // (Touches cells/freeList only; never reallocates cells.)
    std::vector<int32_t> stack;
    stack.push_back(idx);
    while (!stack.empty()) {
      const int32_t c = stack.back();
      stack.pop_back();
      Cell& cell = cells[(size_t)c];
      for (int32_t& ch : cell.child) {
        if (ch >= 0) {
          stack.push_back(ch);
          ch = -1;
        }
      }
      cell.material = 0;
      freeList.push_back(c);
    }
  }

  static bool isLeaf(const Cell& cell) {
    for (int32_t ch : cell.child)
      if (ch >= 0) return false;
    return true;
  }
};

SparseOctree::~SparseOctree() { delete root_; }

SparseOctree::SparseOctree(const SparseOctree& other)
    : root_(other.root_ ? new Node(*other.root_) : nullptr) {}

SparseOctree& SparseOctree::operator=(const SparseOctree& other) {
  if (this != &other) {
    delete root_;
    root_ = other.root_ ? new Node(*other.root_) : nullptr;
  }
  return *this;
}

namespace {

// Octant child index when descending from `parentLevel` toward `level`:
// bit = (zb << 2) | (yb << 1) | xb, matching children[] layout.
inline int octantBit(int level, int parentLevel, int x, int y, int z) {
  const int shift = level - parentLevel - 1;
  return (((z >> shift) & 1) << 2) | (((y >> shift) & 1) << 1) | ((x >> shift) & 1);
}

inline bool inRange(int level, int x, int y, int z) {
  if (level < 0 || level > kMaxLevel) return false;
  const int extent = 1 << level;
  return x >= 0 && x < extent && y >= 0 && y < extent && z >= 0 && z < extent;
}

}  // namespace

void SparseOctree::set(int level, int x, int y, int z, uint8_t material) {
  if (!inRange(level, x, y, z)) return;
  if (!root_) root_ = new Node();
  if (root_->cells.empty()) {
    if (material == 0) return;  // clearing an empty tree is a no-op
    root_->cells.emplace_back();  // level-0 root cell
  }
  Node* n = root_;
  int32_t path[kMaxLevel + 1];
  int32_t idx = 0;
  path[0] = 0;
  for (int p = 0; p < level; ++p) {
    const int bit = octantBit(level, p, x, y, z);
    int32_t c = n->cells[(size_t)idx].child[bit];
    if (c < 0) {
      const uint8_t m = n->cells[(size_t)idx].material;
      if (m != 0) {
        // Refining a filled coarse cell: push its material down so the
        // surrounding volume keeps its value.
        n->cells[(size_t)idx].material = 0;
        for (int i = 0; i < 8; ++i) {
          const int32_t nc = n->alloc();  // cells re-fetched after every alloc
          n->cells[(size_t)nc].material = m;
          n->cells[(size_t)idx].child[i] = nc;
        }
        c = n->cells[(size_t)idx].child[bit];
      } else {
        c = n->alloc();
        n->cells[(size_t)idx].child[bit] = c;
      }
    }
    idx = c;
    path[p + 1] = idx;
  }
  // The addressed cell takes the material; anything finer beneath is gone.
  n->cells[(size_t)idx].material = material;
  for (int i = 0; i < 8; ++i) {
    const int32_t ch = n->cells[(size_t)idx].child[i];
    if (ch >= 0) {
      n->release(ch);
      n->cells[(size_t)idx].child[i] = -1;
    }
  }
  // Collapse ancestors whose children are all empty leaves.
  bool collapsedToRoot = true;
  for (int p = level - 1; p >= 0; --p) {
    const int32_t a = path[p];
    bool allEmpty = true;
    for (int i = 0; i < 8 && allEmpty; ++i) {
      const int32_t ch = n->cells[(size_t)a].child[i];
      if (ch < 0) continue;
      const Node::Cell& cc = n->cells[(size_t)ch];
      if (cc.material != 0 || !Node::isLeaf(cc)) allEmpty = false;
    }
    if (!allEmpty) {
      collapsedToRoot = false;
      break;
    }
    for (int i = 0; i < 8; ++i) {
      const int32_t ch = n->cells[(size_t)a].child[i];
      if (ch >= 0) {
        n->release(ch);
        n->cells[(size_t)a].child[i] = -1;
      }
    }
    n->cells[(size_t)a].material = 0;
  }
  if (level == 0) collapsedToRoot = (material == 0);
  if (collapsedToRoot) {
    // Fully empty: release the pool so an emptied tree is back to tens of
    // bytes instead of retaining peak capacity.
    n->cells.clear();
    n->cells.shrink_to_fit();
    n->freeList.clear();
    n->freeList.shrink_to_fit();
  }
}

uint8_t SparseOctree::get(int level, int x, int y, int z) const {
  if (!inRange(level, x, y, z)) return 0;
  if (!root_ || root_->cells.empty()) return 0;
  const Node* n = root_;
  int32_t idx = 0;
  for (int p = 0; p < level; ++p) {
    const Node::Cell& cell = n->cells[(size_t)idx];
    if (Node::isLeaf(cell)) return cell.material;  // coarsest covering ancestor
    const int32_t c = cell.child[octantBit(level, p, x, y, z)];
    if (c < 0) return 0;  // never refined here: nothing set beneath
    idx = c;
  }
  return n->cells[(size_t)idx].material;
}

std::vector<MicroCell> SparseOctree::leaves() const {
  std::vector<MicroCell> out;
  if (!root_ || root_->cells.empty()) return out;
  const Node* n = root_;
  std::function<void(int32_t, int, uint8_t, uint8_t, uint8_t)> visit =
      [&](int32_t idx, int level, uint8_t x, uint8_t y, uint8_t z) {
        const Node::Cell& cell = n->cells[(size_t)idx];
        if (Node::isLeaf(cell)) {
          if (cell.material != 0)
            out.push_back(MicroCell{(uint8_t)level, x, y, z, cell.material});
          return;
        }
        for (int i = 0; i < 8; ++i) {
          const int32_t ch = cell.child[i];
          if (ch < 0) continue;
          visit(ch, level + 1, (uint8_t)(x * 2 + (i & 1)),
                (uint8_t)(y * 2 + ((i >> 1) & 1)), (uint8_t)(z * 2 + ((i >> 2) & 1)));
        }
      };
  visit(0, 0, 0, 0, 0);
  std::sort(out.begin(), out.end(), [](const MicroCell& a, const MicroCell& b) {
    if (a.level != b.level) return a.level < b.level;
    if (a.z != b.z) return a.z < b.z;
    if (a.y != b.y) return a.y < b.y;
    return a.x < b.x;
  });
  return out;
}

size_t SparseOctree::leafCount() const { return leaves().size(); }

size_t SparseOctree::memoryBytes() const {
  size_t bytes = sizeof(SparseOctree);
  if (root_) {
    bytes += sizeof(Node);
    bytes += root_->cells.capacity() * sizeof(Node::Cell);
    bytes += root_->freeList.capacity() * sizeof(int32_t);
  }
  return bytes;
}

bool SparseOctree::empty() const { return leafCount() == 0; }

void SparseOctree::fillDense(int level, uint8_t* out) const {
  if (!out) return;
  if (level < 0 || level > kMaxLevel) return;  // out-of-range level: no-op
  const int s = 1 << level;
  for (int z = 0; z < s; ++z)
    for (int y = 0; y < s; ++y)
      for (int x = 0; x < s; ++x)
        out[(size_t)((z * s + y) * s + x)] = get(level, x, y, z);
}

uint64_t hashLeaves(const SparseOctree& tree) {
  // FNV-1a over the deterministic leaf stream. The offset basis matches the
  // house constant used across the engine (det_sched.h, image.cpp, ...).
  uint64_t h = 1469598103934665603ULL;
  for (const MicroCell& c : tree.leaves()) {
    const uint8_t bytes[5] = {c.level, c.x, c.y, c.z, c.material};
    for (uint8_t b : bytes) {
      h ^= b;
      h *= 1099511628211ULL;
    }
  }
  return h;
}

}  // namespace litholattice::subdiv
