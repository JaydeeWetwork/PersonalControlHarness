// Headless checks for the procedural tree generator (--selftest via ctest engine_trees).
#include "litholattice/procgen/trees.h"

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <limits>
#include <string>
#include <vector>
#include "litholattice/subdiv/octree.h"
#include "litholattice/subdiv/subdiv_store.h"

namespace litholattice::procgen {
namespace {

class Checker {
 public:
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }

 private:
  int fails_ = 0;
};

using subdiv::MicroCell;
using subdiv::SparseOctree;

/// modelEquals-style leaf-stream equality: identical leaves in identical order.
bool leafStreamsEqual(const SparseOctree& a, const SparseOctree& b) {
  const std::vector<MicroCell> la = a.leaves(), lb = b.leaves();
  if (la.size() != lb.size()) return false;
  for (size_t i = 0; i < la.size(); ++i)
    if (la[i].level != lb[i].level || la[i].x != lb[i].x || la[i].y != lb[i].y ||
        la[i].z != lb[i].z || la[i].material != lb[i].material)
      return false;
  return true;
}

/// Max cell-top y over all leaves (tree height in micro-cells); 0 when empty.
int treeHeightCells(const SparseOctree& t) {
  int top = 0;
  for (const MicroCell& c : t.leaves()) top = std::max(top, int(c.y) + 1);
  return top;
}

/// Every leaf inside the block volume and at level 6, using only expected materials.
bool leavesValid(const SparseOctree& t, std::initializer_list<uint8_t> mats) {
  for (const MicroCell& c : t.leaves()) {
    if (c.level != 6 || c.x >= 64 || c.y >= 64 || c.z >= 64) return false;
    bool ok = false;
    for (uint8_t m : mats)
      if (m != 0 && c.material == m) ok = true;
    if (!ok) return false;
  }
  return true;
}

void determinismChecks(Checker& check) {
  TreeParams p;
  p.seed = 123456789ull;
  const SparseOctree a = generateTree(p), b = generateTree(p);
  check(subdiv::hashLeaves(a) == subdiv::hashLeaves(b),
        "trees: same (params, seed) twice -> identical hashLeaves");
  check(leafStreamsEqual(a, b), "trees: same (params, seed) twice -> identical leaf stream");

  TreeParams c = p;
  c.crystalline = true;
  check(leafStreamsEqual(generateTree(c), generateTree(c)),
        "trees: crystalline mode is deterministic too");

  // Seed sensitivity across a few seeds, both modes.
  bool allDiffer = true;
  const uint64_t seeds[] = {1, 2, 42, 0xDEADBEEFull, 0xFFFFFFFFFFFFFFFFull};
  for (size_t i = 0; i < 5; ++i)
    for (size_t j = i + 1; j < 5; ++j) {
      TreeParams pi = p, pj = p;
      pi.seed = seeds[i];
      pj.seed = seeds[j];
      if (subdiv::hashLeaves(generateTree(pi)) == subdiv::hashLeaves(generateTree(pj)))
        allDiffer = false;
    }
  check(allDiffer, "trees: different seeds -> different trees (5 seeds, pairwise)");
}

void degenerateChecks(Checker& check) {
  // Fail closed: no crash, valid bounded output.
  const float nan = std::numeric_limits<float>::quiet_NaN();
  std::vector<TreeParams> bad;
  TreeParams z;  // all defaults, then break each field
  z.heightCells = 0;
  z.trunkRadiusCells = 0;
  z.branchLevels = -5;
  z.branchesPerNode = 0;
  z.branchAngleRad = nan;
  z.lengthFalloff = 0.0f;
  z.noiseAmount = nan;
  bad.push_back(z);
  TreeParams z2 = z;
  z2.lengthFalloff = 1.5f;
  z2.branchAngleRad = -3.0f;
  bad.push_back(z2);
  TreeParams z3 = z;
  z3.crystalline = true;
  bad.push_back(z3);
  TreeParams z4;  // max everything: must terminate (segment budget) and stay bounded
  z4.seed = 7;
  z4.heightCells = 64;
  z4.trunkRadiusCells = 100;
  z4.branchLevels = 6;
  z4.branchesPerNode = 8;
  z4.lengthFalloff = 0.99f;
  z4.noiseAmount = 1.0f;
  bad.push_back(z4);
  TreeParams z5 = z4;
  z5.crystalline = true;
  bad.push_back(z5);

  bool ok = true;
  for (const TreeParams& bp : bad) {
    const SparseOctree t = generateTree(bp);  // must not crash or hang
    const int budget = std::clamp(bp.heightCells, 1, 64);
    if (treeHeightCells(t) > budget) ok = false;  // hard ceiling: never over budget
    if (!leavesValid(t, {bp.trunkMaterial, bp.leafMaterial, bp.tipMaterial}) && !t.empty())
      ok = false;
  }
  check(ok, "trees: zero/negative/NaN/extreme params -> no crash, bounded valid output");
  check(!generateTree(z).empty(), "trees: all-zero-ish params still yield a minimal tree");
}

void presetChecks(Checker& check, const std::string& name, TreeParams (*preset)(uint64_t),
                  std::initializer_list<uint8_t> mats) {
  const TreeParams p = preset(20261011ull);
  const SparseOctree t = generateTree(p);
  check(!t.empty(), "trees: " + name + " preset generates a non-empty tree");
  check(leavesValid(t, mats), "trees: " + name + " uses only its palette slots, level 6, in-volume");
  const int h = treeHeightCells(t);
  check(h <= 64 && h <= p.heightCells,
        "trees: " + name + " bounded (height " + std::to_string(h) + " <= " +
            std::to_string(p.heightCells) + " cells)");
  // Deterministic per seed, distinct per seed.
  const SparseOctree t2 = generateTree(preset(20261011ull));
  const SparseOctree t3 = generateTree(preset(999ull));
  check(leafStreamsEqual(t, t2), "trees: " + name + " deterministic for its seed");
  check(subdiv::hashLeaves(t) != subdiv::hashLeaves(t3),
        "trees: " + name + " varies with seed");
}

void familyChecks(Checker& check) {
  presetChecks(check, "clockwood wirewood", clockwoodWirewood, {3});
  presetChecks(check, "dopant crystal-tree", dopantCrystalTree, {4, 5, 6});
  presetChecks(check, "trace-forest circuit-tree", traceForestTree, {7, 8, 9});

  // Documented preset values.
  const TreeParams cw = clockwoodWirewood(1);
  check(cw.heightCells >= 56 && cw.heightCells <= 64 && cw.trunkRadiusCells == 2 &&
            cw.branchesPerNode == 2 && cw.leafMaterial == 0 && !cw.crystalline,
        "trees: clockwood preset values (56-64 tall, radius 2, sparse, leafless, organic)");
  const TreeParams dc = dopantCrystalTree(1);
  check(dc.crystalline && dc.trunkMaterial == 4 && dc.leafMaterial == 5 && dc.tipMaterial == 6,
        "trees: dopant preset values (crystalline, blue/amber/white-hot slots 4/5/6)");
  const TreeParams tf = traceForestTree(1);
  check(!tf.crystalline && tf.trunkMaterial == 7 && tf.tipMaterial == 8 && tf.leafMaterial == 9,
        "trees: trace-forest preset values (copper trunk, red/blue tips via slots 7/8/9)");

  // Crystalline vs organic are structurally different for the same seed.
  TreeParams org, cry;
  org.seed = cry.seed = 424242ull;
  cry.crystalline = true;
  const SparseOctree to = generateTree(org), tc = generateTree(cry);
  check(to.leafCount() != tc.leafCount(),
        "trees: crystalline vs organic differ structurally (leaf counts " +
            std::to_string(to.leafCount()) + " vs " + std::to_string(tc.leafCount()) + ")");
  check(subdiv::hashLeaves(to) != subdiv::hashLeaves(tc),
        "trees: crystalline vs organic differ (hashLeaves)");

  // Directly placeable into a SubdivStore (the workstream-3 seam).
  subdiv::SubdivStore store;
  store.setBlockOctree(3, 9, 5, generateTree(traceForestTree(11ull)));
  const SparseOctree* got = store.getBlockOctree(3, 9, 5);
  check(got != nullptr && !got->empty() &&
            leafStreamsEqual(*got, generateTree(traceForestTree(11ull))),
        "trees: generateTree output round-trips through SubdivStore::setBlockOctree");
}

}  // namespace

int runTreeGenSelfTest() {
  std::printf("TreeGen self-test (procedural vegetation)\n");
  Checker check;
  determinismChecks(check);
  degenerateChecks(check);
  familyChecks(check);
  std::printf(check.fails() ? "TREEGEN SELFTEST FAILED (%d)\n" : "TREEGEN SELFTEST OK\n",
              check.fails());
  return check.fails();
}

}  // namespace litholattice::procgen
