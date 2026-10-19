// subdiv_selftest.cpp: headless checks for the subdiv module (workstream 1).
//
// Covers the contract: set/get/collapse semantics, sparse memory bounds,
// determinism (insertion-order-independent hashLeaves), leaves() ordering,
// fillDense, deep-copy value semantics, and SubdivStore round-trips.

#include "litholattice/subdiv/subdiv_selftest.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "litholattice/subdiv/octree.h"
#include "litholattice/subdiv/subdiv_store.h"

namespace litholattice::subdiv {
namespace {

class Checker {
 public:
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] subdiv: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }

 private:
  int fails_ = 0;
};

bool leavesSorted(const std::vector<MicroCell>& ls) {
  for (size_t i = 1; i < ls.size(); ++i) {
    const MicroCell& a = ls[i - 1];
    const MicroCell& b = ls[i];
    if (a.level != b.level) {
      if (!(a.level < b.level)) return false;
    } else if (a.z != b.z) {
      if (!(a.z < b.z)) return false;
    } else if (a.y != b.y) {
      if (!(a.y < b.y)) return false;
    } else if (!(a.x < b.x)) {
      return false;
    }
  }
  return true;
}

void testEmptyAndBounds(Checker& c) {
  SparseOctree t;
  c(t.empty(), "fresh tree is empty");
  c(t.leafCount() == 0, "fresh tree has no leaves");
  c(t.leaves().empty(), "fresh tree leaf stream is empty");
  c(t.memoryBytes() < 256, "empty tree costs tens of bytes");
  c(t.get(6, 0, 0, 0) == 0, "get on an empty tree is 0");
  // Clearing an empty tree is a no-op and stays empty.
  t.set(3, 1, 2, 3, 0);
  c(t.empty(), "clearing an empty tree is a no-op");

  SparseOctree one;
  one.set(6, 10, 20, 30, 7);
  c(!one.empty(), "tree with one cell is not empty");
  c(one.leafCount() == 1, "single level-6 cell => one leaf");
  c(one.get(6, 10, 20, 30) == 7, "single cell reads back its material");
  c(one.get(6, 11, 20, 30) == 0, "neighbouring cell is 0");
  c(one.get(0, 0, 0, 0) == 0,
    "coarse get over a refined region is 0 (never explicitly set)");
  c(one.memoryBytes() < 4096,
    "single level-6 cell costs << 64^3 bytes (sparse bound)");
  // Out-of-range sets are no-ops.
  one.set(7, 0, 0, 0, 1);
  one.set(6, 64, 0, 0, 1);
  one.set(6, 0, 0, -1, 1);
  one.set(-1, 0, 0, 0, 1);
  c(one.leafCount() == 1, "out-of-range set is a no-op");
  c(one.get(7, 0, 0, 0) == 0 && one.get(-1, 0, 0, 0) == 0,
    "out-of-range get is 0");
  // Clearing the only cell collapses back to empty.
  one.set(6, 10, 20, 30, 0);
  c(one.empty() && one.leafCount() == 0,
    "clearing the only cell empties the tree");
  c(one.memoryBytes() < 256, "emptied tree shrinks back to tens of bytes");
}

void testAncestorResolution(Checker& c) {
  SparseOctree t;
  t.set(2, 1, 1, 1, 3);
  c(t.get(2, 1, 1, 1) == 3, "coarse cell reads back");
  // Level-2 cell (1,1,1) covers level-6 cells [16,32) on each axis.
  c(t.get(6, 16, 16, 16) == 3 && t.get(6, 31, 31, 31) == 3 &&
        t.get(6, 16, 31, 24) == 3,
    "fine get resolves through the coarsest covering ancestor");
  c(t.get(6, 32, 16, 16) == 0 && t.get(6, 15, 16, 16) == 0 &&
        t.get(6, 16, 16, 15) == 0,
    "addresses outside the coarse cell are 0");
  c(t.leafCount() == 1, "one coarse set => one leaf");
}

void testSetClearsFiner(Checker& c) {
  SparseOctree t;
  t.set(6, 16, 16, 16, 5);
  t.set(6, 17, 16, 16, 6);
  t.set(5, 20, 20, 20, 7);  // level-5 cell 20 covers level-6 x in [40,42)
  c(t.leafCount() == 3, "three disjoint sets => three leaves");
  // Level-2 cell (1,1,1) covers level-6 [16,32)^3: swallows the first two.
  t.set(2, 1, 1, 1, 9);
  c(t.get(6, 16, 16, 16) == 9 && t.get(6, 17, 16, 16) == 9 &&
        t.get(6, 31, 31, 31) == 9,
    "coarse set overwrites the region");
  c(t.get(6, 40, 40, 40) == 7 && t.get(6, 41, 40, 40) == 7,
    "detail outside the coarse cell survives");
  c(t.leafCount() == 2,
    "setting a cell clears the finer cells beneath it");
}

void testRefinePushDown(Checker& c) {
  SparseOctree t;
  t.set(1, 0, 0, 0, 5);
  t.set(3, 1, 1, 1, 9);
  // Level-1 cell (0,0,0) covers level-6 [0,32)^3;
  // level-3 cell (1,1,1) covers [8,16)^3.
  std::vector<uint8_t> dense(64 * 64 * 64, 0);
  t.fillDense(6, dense.data());
  size_t n5 = 0, n9 = 0, n0 = 0;
  for (uint8_t v : dense) {
    if (v == 5)
      ++n5;
    else if (v == 9)
      ++n9;
    else if (v == 0)
      ++n0;
    else
      n0 = (size_t)-1;  // poison: unexpected material
  }
  c(n9 == 512, "refined 8^3 block reads back its material");
  c(n5 == 32768 - 512, "push-down keeps the coarse material around it");
  c(n0 == 262144 - 32768, "untouched volume stays 0");
  c(t.leafCount() == 15, "push-down leaf budget is exact (7+7+1)");
  // Out-of-range fillDense levels are no-ops.
  std::vector<uint8_t> sentinel(4, 0xAA);
  t.fillDense(7, sentinel.data());
  t.fillDense(-1, sentinel.data());
  c(sentinel[0] == 0xAA && sentinel[3] == 0xAA,
    "fillDense with out-of-range level is a no-op");
}

void testLeavesOrder(Checker& c) {
  SparseOctree t;
  t.set(6, 5, 4, 3, 1);
  t.set(2, 3, 3, 3, 2);
  t.set(4, 0, 0, 1, 3);
  t.set(6, 60, 1, 2, 4);
  const std::vector<MicroCell> ls = t.leaves();
  c(ls.size() == 4, "four disjoint sets => four leaves");
  c(leavesSorted(ls), "leaves() is sorted (level, z, y, x)");
  // Non-overlapping: no leaf contains another.
  bool overlap = false;
  for (size_t i = 0; i < ls.size() && !overlap; ++i)
    for (size_t j = 0; j < ls.size() && !overlap; ++j) {
      if (i == j) continue;
      const MicroCell& a = ls[i];
      const MicroCell& b = ls[j];
      if (b.level <= a.level) continue;
      const int shift = b.level - a.level;
      if ((b.x >> shift) == a.x && (b.y >> shift) == a.y &&
          (b.z >> shift) == a.z)
        overlap = true;
    }
  c(!overlap, "leaves() are non-overlapping");
}

void testDeterminism(Checker& c) {
  auto build = [](bool reverse) {
    SparseOctree t;
    struct Op {
      int l, x, y, z;
      uint8_t m;
    };
    const Op ops[] = {{6, 1, 2, 3, 9}, {2, 0, 0, 1, 4}, {4, 9, 8, 7, 2},
                      {6, 63, 63, 63, 5}, {1, 1, 0, 1, 6}, {5, 3, 3, 3, 1}};
    if (!reverse) {
      for (const Op& o : ops) t.set(o.l, o.x, o.y, o.z, o.m);
    } else {
      for (int i = 5; i >= 0; --i) t.set(ops[i].l, ops[i].x, ops[i].y, ops[i].z,
                                        ops[i].m);
    }
    return t;
  };
  const SparseOctree fwd = build(false);
  const SparseOctree rev = build(true);
  c(hashLeaves(fwd) == hashLeaves(rev),
    "insertion order does not change hashLeaves");
  c(fwd.leaves().size() == rev.leaves().size() &&
        hashLeaves(fwd) == hashLeaves(build(false)),
    "same op sequence => identical leaf stream");
  // hashLeaves is stable across identical rebuilds (structural determinism).
  SparseOctree a = build(false), b = build(false);
  c(hashLeaves(a) == hashLeaves(b), "repeated builds hash identically");
}

void testCopySemantics(Checker& c) {
  SparseOctree a;
  a.set(6, 1, 2, 3, 9);
  a.set(2, 0, 0, 1, 4);
  SparseOctree b = a;  // copy-construct
  a.set(6, 1, 2, 3, 1);
  c(b.get(6, 1, 2, 3) == 9 && b.get(2, 0, 0, 1) == 4 && b.leafCount() == 2,
    "copy is deep: mutating the original leaves the copy alone");
  SparseOctree d;
  d = a;  // copy-assign
  c(d.get(6, 1, 2, 3) == 1 && d.get(2, 0, 0, 1) == 4,
    "copy-assign deep-copies");
  d.set(6, 1, 2, 3, 2);
  c(a.get(6, 1, 2, 3) == 1, "copy-assign target is independent afterwards");
  SparseOctree e;
  e.set(0, 0, 0, 0, 8);
  e = e;  // self-assign must not corrupt
  c(e.get(0, 0, 0, 0) == 8 && e.get(6, 1, 2, 3) == 8, "self-assign is safe");
}

// Differential test: the tree must agree with a naive dense 64^3 reference
// model across a long deterministic pseudo-random op sequence, both for
// point queries and for the full leaf rasterization.
void testRandomizedModel(Checker& c) {
  constexpr int S = 64;
  std::vector<uint8_t> ref((size_t)S * S * S, 0);
  SparseOctree t;
  uint64_t rng = 0x243F6A8885A308D3ULL;  // fixed seed: deterministic
  auto next = [&]() {
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return rng >> 11;
  };
  for (int i = 0; i < 3000; ++i) {
    const int level = (int)(next() % 7);
    const int e = 1 << level;
    const int x = (int)(next() % (uint64_t)e);
    const int y = (int)(next() % (uint64_t)e);
    const int z = (int)(next() % (uint64_t)e);
    const uint8_t m = (uint8_t)(next() % 8);  // 0 clears, 1..7 fill
    t.set(level, x, y, z, m);
    const int w = S >> level;
    for (int dz = 0; dz < w; ++dz)
      for (int dy = 0; dy < w; ++dy)
        for (int dx = 0; dx < w; ++dx)
          ref[((size_t)((z * w + dz) * S + (y * w + dy)) * S) + (x * w + dx)] = m;
    // Spot-check point queries against the reference after every op.
    // (Queried at max level, where get() provably equals the dense cell:
    // coarser queries legitimately return 0 over partially-refined regions.)
    for (int k = 0; k < 4; ++k) {
      const int qx = (int)(next() % 64);
      const int qy = (int)(next() % 64);
      const int qz = (int)(next() % 64);
      const uint8_t want = ref[((size_t)((qz * S + qy)) * S) + qx];
      if (t.get(6, qx, qy, qz) != want) {
        c(false, "randomized model: get() matches the dense reference");
        return;
      }
    }
  }
  // Rasterize the leaf stream and compare against the reference volume.
  // (Leaves cover exactly the painted regions; the rest is 0 in both.)
  std::vector<uint8_t> dense((size_t)S * S * S, 0);
  for (const MicroCell& lc : t.leaves()) {
    const int w = S >> lc.level;
    for (int dz = 0; dz < w; ++dz)
      for (int dy = 0; dy < w; ++dy)
        for (int dx = 0; dx < w; ++dx)
          dense[((size_t)(((lc.z * w + dz)) * S + (lc.y * w + dy)) * S) +
                (lc.x * w + dx)] = lc.material;
  }
  c(dense == ref, "randomized model: leaf rasterization matches the reference");
  c(t.leafCount() == t.leaves().size(), "leafCount agrees with leaves()");
}

void testStress(Checker& c) {
  SparseOctree t;
  for (int z = 0; z < 32; ++z)
    for (int y = 0; y < 32; ++y)
      for (int x = 0; x < 32; ++x)
        t.set(5, x, y, z, (uint8_t)(1 + (x + y + z) % 7));
  c(t.leafCount() == 32768, "32^3 level-5 cells stored");
  c(t.memoryBytes() < 8 * 1024 * 1024,
    "full-detail stress stays sane (<8MB for 32^3 cells)");
  c(t.get(5, 0, 0, 0) == 1 && t.get(5, 31, 31, 31) == (uint8_t)(1 + 93 % 7),
    "stress volume spot-checks read back");
  // Clearing a carved-out octant collapses it.
  for (int z = 0; z < 16; ++z)
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 16; ++x) t.set(5, x, y, z, 0);
  c(t.leafCount() == 32768 - 4096, "clearing an octant drops its leaves");
  c(t.get(5, 0, 0, 0) == 0 && t.get(5, 16, 0, 0) == (uint8_t)(1 + 16 % 7),
    "cleared region reads 0, neighbours intact");
}

void testStoreBasics(Checker& c) {
  SubdivStore s;
  c(s.blockCount() == 0, "fresh store has no blocks");
  c(s.getBlockOctree(0, 0, 0) == nullptr, "missing block => nullptr");
  c(s.getBlockOctree(16, 0, 0) == nullptr, "out-of-range get => nullptr");

  SparseOctree o;
  o.set(6, 1, 1, 1, 5);
  s.setBlockOctree(3, 7, 15, o);
  c(s.blockCount() == 1, "one stored block");
  const SparseOctree* g = s.getBlockOctree(3, 7, 15);
  c(g && g->get(6, 1, 1, 1) == 5, "stored octree reads back");
  // The store keeps its own copy.
  o.set(6, 1, 1, 1, 6);
  c(s.getBlockOctree(3, 7, 15)->get(6, 1, 1, 1) == 5,
    "store is unaffected by later mutation of the source tree");
  // Out-of-range coords are ignored.
  s.setBlockOctree(16, 0, 0, o);
  s.setBlockOctree(-1, 0, 0, o);
  s.setBlockOctree(0, 16, 0, o);
  c(s.blockCount() == 1, "out-of-range setBlockOctree is ignored");
  // Storing an empty octree removes the entry.
  s.setBlockOctree(3, 7, 15, SparseOctree{});
  c(s.blockCount() == 0 && s.getBlockOctree(3, 7, 15) == nullptr,
    "storing an empty octree removes the entry");
  // Replacement and removal.
  s.setBlockOctree(1, 2, 3, o);
  s.setBlockOctree(1, 2, 3, o);
  c(s.blockCount() == 1, "re-set replaces instead of duplicating");
  c(s.removeBlockOctree(1, 2, 3), "remove reports true");
  c(!s.removeBlockOctree(1, 2, 3), "double remove reports false");
  c(!s.removeBlockOctree(16, 0, 0), "out-of-range remove reports false");
  c(s.memoryBytes() >= sizeof(SubdivStore), "store memoryBytes is sane");
}

void testStoreSerialize(Checker& c) {
  SubdivStore s;
  SparseOctree a;
  a.set(6, 1, 2, 3, 9);
  a.set(2, 0, 0, 1, 4);
  SparseOctree b;
  b.set(0, 0, 0, 0, 2);
  s.setBlockOctree(0, 0, 0, a);
  s.setBlockOctree(15, 15, 15, b);
  s.setBlockOctree(7, 8, 9, a);
  const std::vector<uint8_t> bytes = s.serialize();
  c(bytes.size() >= 10 && bytes[0] == 'L' && bytes[1] == 'T' &&
        bytes[2] == 'H' && bytes[3] == 'S',
    "stream starts with magic LTHS");
  c(bytes[4] == 1 && bytes[5] == 0, "stream carries version 1 (LE)");

  SubdivStore s2;
  c(s2.deserialize(bytes.data(), bytes.size()), "well-formed stream parses");
  c(s2.blockCount() == 3, "round-trip preserves block count");
  c(s2.serialize() == bytes, "serialize round-trips byte-identical");
  const SparseOctree* g = s2.getBlockOctree(15, 15, 15);
  c(g && g->get(0, 0, 0, 0) == 2, "round-trip preserves a level-0 cell");
  c(s2.getBlockOctree(0, 0, 0)->get(6, 1, 2, 3) == 9,
    "round-trip preserves fine detail");

  // Malformed streams are rejected and leave the store unchanged.
  SubdivStore s3;
  s3.setBlockOctree(1, 1, 1, a);
  const std::vector<uint8_t> before = s3.serialize();
  c(!s3.deserialize(bytes.data(), 5), "truncated stream rejected");
  c(!s3.deserialize(nullptr, 0), "null/empty stream rejected");
  std::vector<uint8_t> bad = bytes;
  bad[0] = 'X';
  c(!s3.deserialize(bad.data(), bad.size()), "bad magic rejected");
  bad = bytes;
  bad[4] = 2;
  c(!s3.deserialize(bad.data(), bad.size()), "unknown version rejected");
  bad = bytes;
  bad.push_back(0xFF);
  c(!s3.deserialize(bad.data(), bad.size()), "trailing garbage rejected");
  bad = bytes;
  bad[10] = 16;  // first entry x out of range
  c(!s3.deserialize(bad.data(), bad.size()), "out-of-range block rejected");
  c(s3.serialize() == before, "failed deserialize leaves the store unchanged");

  // Empty store round-trips.
  SubdivStore e, e2;
  const std::vector<uint8_t> eb = e.serialize();
  c(e2.deserialize(eb.data(), eb.size()) && e2.blockCount() == 0,
    "empty store round-trips");
}

}  // namespace

int runSubdivSelfTest() {
  Checker c;
  testEmptyAndBounds(c);
  testAncestorResolution(c);
  testSetClearsFiner(c);
  testRefinePushDown(c);
  testLeavesOrder(c);
  testRandomizedModel(c);
  testDeterminism(c);
  testCopySemantics(c);
  testStress(c);
  testStoreBasics(c);
  testStoreSerialize(c);
  return c.fails();
}

}  // namespace litholattice::subdiv
