// Self-tests for procedural rig generation (riggen.h).
// Wired into the engine `anim` ctest module via runRiggenSelfTest().

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "litholattice/anim/riggen.h"

namespace litholattice::anim {
namespace {

struct Checker {
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails;
  }
  int fails = 0;
};

// A chunky stick figure on a 32^3 grid: 2 legs, torso, head, 2 arms.
// Limbs are 3 voxels thick so the distance field has strict medial axes.
VoxelGrid stickFigure() {
  VoxelGrid g;
  g.size = 32;
  auto box = [&](int x0, int x1, int y0, int y1, int z0, int z1) {
    for (int z = z0; z <= z1; ++z)
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) g.cells.push_back({x, y, z});
  };
  box(12, 14, 2, 11, 15, 17);   // left leg
  box(17, 19, 2, 11, 15, 17);   // right leg
  box(12, 19, 12, 21, 15, 17);  // torso
  box(14, 17, 22, 25, 15, 17);  // head
  box(6, 11, 17, 19, 15, 17);   // left arm
  box(20, 25, 17, 19, 15, 17);  // right arm
  return g;
}

// A solid sphere (stress shape for the distance transform / Dijkstra).
VoxelGrid blobSphere() {
  VoxelGrid g;
  g.size = 32;
  for (int z = 0; z < 32; ++z)
    for (int y = 0; y < 32; ++y)
      for (int x = 0; x < 32; ++x) {
        const float dx = float(x) - 15.5f, dy = float(y) - 15.5f, dz = float(z) - 15.5f;
        if (dx * dx + dy * dy + dz * dz <= 14.f * 14.f) g.cells.push_back({x, y, z});
      }
  return g;
}

bool rigsEqual(const Rig& a, const Rig& b) {
  if (a.bones.size() != b.bones.size()) return false;
  for (size_t i = 0; i < a.bones.size(); ++i) {
    const Bone &x = a.bones[i], &y = b.bones[i];
    if (x.name != y.name || x.parent != y.parent) return false;
    if (x.head.x != y.head.x || x.head.y != y.head.y || x.head.z != y.head.z) return false;
    if (x.tail.x != y.tail.x || x.tail.y != y.tail.y || x.tail.z != y.tail.z) return false;
  }
  return true;
}

}  // namespace

int runRiggenSelfTest() {
  Checker check;

  // --- invalid input ---
  {
    VoxelGrid empty;
    empty.size = 32;
    check(!extractRig(empty).has_value(), "riggen: empty grid -> nullopt");
    VoxelGrid bad;
    bad.size = 32;
    bad.cells.push_back({99, 0, 0});
    check(!extractRig(bad).has_value(), "riggen: out-of-range cell -> nullopt");
  }

  // --- single voxel: degenerate but valid ---
  {
    VoxelGrid g;
    g.size = 8;
    g.cells.push_back({4, 4, 4});
    auto rig = extractRig(g);
    check(rig.has_value() && rig->valid() && rig->boneCount() == 1,
          "riggen: single voxel -> one-bone rig");
    computeSkinning(*rig, {{4.5f, 4.5f, 4.5f}});
    check(rig->influences.size() == 1 && rig->influences[0].size() == 1 &&
              rig->influences[0][0].weight == 1.f,
          "riggen: single voxel skins rigidly to its bone");
  }

  // --- stick figure: structure ---
  auto rig = extractRig(stickFigure());
  check(rig.has_value(), "riggen: stick figure extracts");
  if (rig) {
    check(rig->valid(), "riggen: stick-figure rig valid");
    check(rig->boneCount() <= size_t(kMaxBones), "riggen: bone count within cap");
    check(rig->boneCount() >= 7, "riggen: stick figure gets spine+head+limbs");
    check(rig->bones[0].name == "spine" && rig->bones[0].parent == -1,
          "riggen: bones[0] is the spine root");
    bool hasHead = false, hasLimb = false;
    for (const auto& b : rig->bones) {
      if (b.name == "head") hasHead = true;
      if (b.name.rfind("limb_", 0) == 0) hasLimb = true;
      check(b.parent < 0 || size_t(b.parent) < rig->boneCount(), "riggen: parent indices in range");
    }
    check(hasHead, "riggen: head bone detected");
    check(hasLimb, "riggen: limb bones detected");
    // parents form a valid tree (no cycles): walk to root terminates
    bool acyclic = true;
    for (size_t i = 0; i < rig->boneCount() && acyclic; ++i) {
      int p = rig->bones[i].parent, steps = 0;
      while (p >= 0 && steps <= int(rig->boneCount())) { p = rig->bones[size_t(p)].parent; ++steps; }
      if (steps > int(rig->boneCount())) acyclic = false;
    }
    check(acyclic, "riggen: bone hierarchy is a tree");
  }

  // --- determinism: same voxels -> bit-identical rig ---
  {
    auto a = extractRig(stickFigure());
    auto b = extractRig(stickFigure());
    check(a.has_value() && b.has_value() && rigsEqual(*a, *b),
          "riggen: extraction is deterministic");
  }

  // --- skinning: influence count, weight sums, flood-fill correctness ---
  if (rig) {
    // vertices: every 3rd voxel center + a few off-volume points
    std::vector<Vec3> verts;
    const VoxelGrid fig = stickFigure();
    for (size_t i = 0; i < fig.cells.size(); i += 3) {
      const auto& c = fig.cells[i];
      verts.push_back({float(c.x) + 0.5f, float(c.y) + 0.5f, float(c.z) + 0.5f});
    }
    verts.push_back({-5.f, -5.f, -5.f});  // outside: nearest-filled fallback
    // NOTE: extractRig must be (re-)run to arm the paired skinning state,
    // because the determinism check above re-extracted.
    rig = extractRig(fig);
    computeSkinning(*rig, verts);
    bool ok = rig->influences.size() == verts.size();
    float maxErr = 0.f;
    for (const auto& inf : rig->influences) {
      if (inf.size() > size_t(kMaxInfluences)) ok = false;
      float sum = 0.f;
      for (const auto& in : inf) {
        sum += in.weight;
        if (in.bone >= rig->boneCount()) ok = false;
      }
      maxErr = std::max(maxErr, std::abs(sum - 1.f));
    }
    check(ok, "riggen: <= 4 influences per vertex, bone indices in range");
    check(maxErr < 1e-4f, "riggen: weights sum to 1 per vertex");

    // Flood-fill check: a voxel at the tip of the left arm must be dominated
    // by a limb bone, never by a spine/head bone across empty space.
    // Left arm tip ≈ x=6.5, y=18.
    int armBone = -1;
    for (size_t i = 0; i < rig->boneCount(); ++i)
      if (rig->bones[i].name.rfind("limb_", 0) == 0) { armBone = int(i); break; }
    Rig probe = *rig;
    computeSkinning(probe, {{6.5f, 18.f, 16.f}});
    bool limbDominates = false;
    if (!probe.influences.empty() && !probe.influences[0].empty()) {
      const Influence* top = &probe.influences[0][0];
      for (const auto& in : probe.influences[0])
        if (in.weight > top->weight) top = &in;
      limbDominates = rig->bones[top->bone].name.rfind("limb_", 0) == 0;
    }
    check(armBone >= 0 && limbDominates,
          "riggen: arm-tip voxel follows a limb bone (flood-fill, not cross-space)");
  }

  // --- performance: 32^3 stress shape under 200 ms ---
  {
    const VoxelGrid blob = blobSphere();
    const auto t0 = std::chrono::steady_clock::now();
    auto r = extractRig(blob);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    char buf[128];
    std::snprintf(buf, sizeof buf, "riggen: 32^3 sphere extracts in %.1f ms (< 200 ms)", ms);
    check(r.has_value() && ms < 200.0, buf);
  }

  std::printf("riggen selftest: %d failure(s)\n", check.fails);
  return check.fails;
}

}  // namespace litholattice::anim
