// agent_models_selftest: the Elfie spawn model — shape/palette sanity,
// determinism against the design prototype (cell count + FNV-1a over the
// canonical sorted cell list), and the rig pipeline (extractRig +
// computeSkinning + validateRig) on the real figure.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "agent_models.h"
#include "litholattice/anim/riggen.h"

namespace {
int g_fails = 0;
void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++g_fails;
}
}  // namespace

int main() {
  using namespace lithokraft::model::agents;
  namespace pal = lithokraft::model::agents::palette;

  const AgentModelDef* def = findAgentModel(kDefaultAgentModel);
  check(def != nullptr, "default agent model is registered (elfie)");
  if (!def) return 1;
  const auto m = def->author();

  check(m.size == 32, "grid is 32^3");
  check(m.cells.size() == 1344, "cell count matches the design prototype (1344)");

  // FNV-1a 64 over the canonical (x,y,z)-sorted cells, bytes x,y,z,palette —
  // the same hash the design prototype prints. Drift between design and code
  // fails here, loudly.
  uint64_t h = 0xcbf29ce484222325ull;
  auto mix = [&h](uint8_t b) {
    h ^= b;
    h *= 0x100000001b3ull;
  };
  int minY = 99, maxY = -1;
  int glowPurple = 0, glowOrange = 0;
  bool inBounds = true;
  for (const auto& c : m.cells) {
    mix(uint8_t(c.x));
    mix(uint8_t(c.y));
    mix(uint8_t(c.z));
    mix(c.palette);
    if (c.x < 0 || c.x >= 32 || c.y < 0 || c.y >= 32 || c.z < 0 || c.z >= 32) inBounds = false;
    minY = std::min(minY, c.y);
    maxY = std::max(maxY, c.y);
    if (c.palette == pal::kGlowPurple) ++glowPurple;
    if (c.palette == pal::kGlowOrange) ++glowOrange;
  }
  check(inBounds, "all cells inside the grid");
  check(minY == 0 && maxY == 29, "figure stands on y=0 and is 30 voxels tall");
  check(h == 0x49fb03e56d1942a6ull, "cell hash matches the design prototype");
  check(glowPurple >= 20 && glowOrange >= 20, "Tron glow traces present (purple + orange)");

  // Identity disc core sits at (16,19,12), over the hair.
  bool disc = false;
  for (const auto& c : m.cells)
    if (c.x == 16 && c.y == 19 && c.z == 12 && c.palette == pal::kGlowCore) disc = true;
  check(disc, "identity disc core at (16,19,12)");

  // Determinism: re-authoring is byte-identical.
  const auto m2 = def->author();
  check(m2.cells.size() == m.cells.size(), "re-author: same cell count");
  bool identical = m2.cells.size() == m.cells.size();
  if (identical)
    for (size_t i = 0; i < m.cells.size(); ++i) {
      const auto& a = m.cells[i];
      const auto& b = m2.cells[i];
      if (a.x != b.x || a.y != b.y || a.z != b.z || a.palette != b.palette) identical = false;
    }
  check(identical, "re-author: identical cells (deterministic)");

  // Rig pipeline on the real figure.
  auto grid = toVoxelGrid(m);
  auto rig = litholattice::anim::extractRig(grid);
  check(rig.has_value(), "extractRig succeeds on the figure");
  if (rig) {
    check(rig->valid(), "rig is valid (bone cap respected)");
    // Bone layout on this figure (verified against riggen): a 4-segment
    // spine whose top segment reaches into the head (riggen folds the head
    // into the top spine bone when the neck run is short — no separate
    // "head" bone is named), plus 4 limbs x 3 segments (two arms, two legs).
    int limbs = 0;
    float spineTop = 0.f;
    for (const auto& bone : rig->bones) {
      if (bone.name.rfind("limb_", 0) == 0) ++limbs;
      spineTop = std::max(spineTop, std::max(bone.head.y, bone.tail.y));
    }
    check(limbs == 12, "rig extracts 4 limbs x 3 segments (arms + legs)");
    check(spineTop >= 23.f, "spine reaches the head (top segment covers y>=23)");
    // Structural sanity: root has no parent, every other parent is in range
    // and precedes the child (the engine's validateRig covers the rest).
    bool parentsOk = !rig->bones.empty() && rig->bones[0].parent == -1;
    for (size_t i = 1; i < rig->bones.size(); ++i) {
      const int p = rig->bones[i].parent;
      if (p < 0 || size_t(p) >= i) parentsOk = false;
    }
    check(parentsOk, "rig parent chain is well-formed");

    // Skinning over the voxel centers: every vertex influenced, weights sum to 1.
    std::vector<litholattice::Vec3> verts;
    verts.reserve(m.cells.size());
    for (const auto& c : m.cells) verts.push_back({c.x + 0.5f, c.y + 0.5f, c.z + 0.5f});
    litholattice::anim::computeSkinning(*rig, verts);
    bool skinOk = rig->influences.size() == verts.size();
    if (skinOk)
      for (const auto& inf : rig->influences) {
        if (inf.empty() || inf.size() > 4) skinOk = false;
        float sum = 0.f;
        for (const auto& i : inf) sum += i.weight;
        if (std::fabs(sum - 1.f) > 1e-3f) skinOk = false;
      }
    check(skinOk, "skinning: every voxel influenced, weights normalized");
  }

  std::printf(g_fails ? "AGENT MODELS SELFTEST: FAILED (%d)\n" : "AGENT MODELS SELFTEST: OK\n", g_fails);
  return g_fails;
}
