// IK solver self-tests.
#include "litholattice/anim/anim_selftest.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include "iksolve.h"
#include "litholattice/anim/riggen.h"  // Worker 1: runRiggenSelfTest
#include "litholattice/anim/rig.h"
#include "litholattice/core/math.h"

namespace litholattice::anim {
namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fails;
}
bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }
float vdist(Vec3 a, Vec3 b) {
  const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}
Vec3 xfp(const Mat4& m, Vec3 v) {
  return {m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12],
          m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13],
          m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14]};
}
bool finiteMat(const Mat4& m) {
  for (float f : m.m)
    if (!std::isfinite(f)) return false;
  return true;
}

// Two-bone arm: shoulder at origin, upper arm 10 down -Y, forearm 8 down -Y.
Rig makeArm() {
  Rig r;
  r.bones.push_back({"upper", -1, {0, 0, 0}, {0, -10, 0}});
  r.bones.push_back({"fore", 0, {0, -10, 0}, {0, -18, 0}});
  return r;
}
// Three-bone chain, 5 units per bone, hanging down -Y.
Rig makeChain3() {
  Rig r;
  r.bones.push_back({"b0", -1, {0, 0, 0}, {0, -5, 0}});
  r.bones.push_back({"b1", 0, {0, -5, 0}, {0, -10, 0}});
  r.bones.push_back({"b2", 1, {0, -10, 0}, {0, -15, 0}});
  return r;
}

// Apply an IkSolution over the bind pose and return model-space matrices.
std::vector<Mat4> applySolution(const Rig& rig, const IkSolution& sol) {
  auto locals = bindLocalPoses(rig);
  for (size_t i = 0; i < sol.bones.size() && i < sol.local.size(); ++i) {
    const int b = sol.bones[i];
    if (b >= 0 && size_t(b) < locals.size()) locals[size_t(b)] = sol.local[i];
  }
  return computeSkinningMatrices(rig, locals);
}
// Tip position: the chain tip's tail in model space. In a bone's local frame
// +Y runs head->tail, so the tail is at (0, len, 0).
Vec3 tipPos(const std::vector<Mat4>& model, int tipBone, float len) {
  return xfp(model[size_t(tipBone)], {0, len, 0});
}

}  // namespace

int runAnimSelfTest() {
  g_fails = 0;
  std::printf("LithoLattice anim self-test\n");

  g_fails += runRiggenSelfTest();  // Worker 1: skeleton extraction + skinning

  // 1. Two-bone reaches a reachable target exactly.
  {
    const Rig rig = makeArm();
    IkChain chain;
    chain.bones = {0, 1};
    const Vec3 target{6, -12, 0};
    const IkSolution sol = solveTwoBone(rig, chain, {0, 0, 0}, target, {0, 0, 1});
    const auto model = applySolution(rig, sol);
    const Vec3 tip = tipPos(model, 1, 8.f);
    check(vdist(tip, target) < 1e-3f, "ik: two-bone analytic reaches the target");
    // Elbow bent toward the pole (+Z): elbow z must be positive.
    const Vec3 elbow = xfp(model[0], {0, 10.f, 0});
    check(elbow.z > 0.5f, "ik: two-bone elbow follows the pole hint");
  }

  // 2. Unreachable target clamps to max extension.
  {
    const Rig rig = makeArm();
    IkChain chain;
    chain.bones = {0, 1};
    const IkSolution sol = solveTwoBone(rig, chain, {0, 0, 0}, {0, -30, 0}, {0, 0, 1});
    const auto model = applySolution(rig, sol);
    const Vec3 tip = tipPos(model, 1, 8.f);
    check(near(tip.x, 0.f) && near(tip.z, 0.f) && near(tip.y, -18.f, 1e-2f),
          "ik: two-bone clamps an unreachable target to full extension");
    bool ok = true;
    for (const Mat4& m : model) ok = ok && finiteMat(m);
    check(ok, "ik: two-bone clamp produces finite matrices");
  }

  // 3. Straight-line degenerate (target exactly at max reach, pole parallel) is NaN-free.
  {
    const Rig rig = makeArm();
    IkChain chain;
    chain.bones = {0, 1};
    // Pole parallel to the target direction: the bend plane is degenerate.
    const IkSolution sol = solveTwoBone(rig, chain, {0, 0, 0}, {0, -18, 0}, {0, -1, 0});
    const auto model = applySolution(rig, sol);
    bool ok = true;
    for (const Mat4& m : model) ok = ok && finiteMat(m);
    const Vec3 tip = tipPos(model, 1, 8.f);
    ok = ok && vdist(tip, Vec3{0, -18, 0}) < 1e-2f;
    check(ok, "ik: two-bone straight-line degenerate stays finite and on target");
  }

  // 3b. H-ANIM-1 residual: a NaN RIG bone (finite inputs) must yield no
  // solution rather than NaN matrices — CCD/FABRIK already guarded this via
  // chainJoints; two-bone now validates its two bones' head/tail directly.
  {
    Rig rig = makeArm();
    rig.bones[1].tail.x = std::numeric_limits<float>::quiet_NaN();
    IkChain chain;
    chain.bones = {0, 1};
    const IkSolution sol = solveTwoBone(rig, chain, {0, 0, 0}, {6, -12, 0}, {0, 0, 1});
    check(sol.bones.empty() && sol.local.empty(),
          "ik: two-bone refuses a rig with a non-finite bone (no NaN solution)");
  }

  // 4. CCD converges on a 3-joint chain.
  {
    const Rig rig = makeChain3();
    IkChain chain;
    chain.bones = {0, 1, 2};
    const Vec3 target{6, -8, 3};
    const IkSolution sol = solveCcd(rig, chain, target);
    const auto model = applySolution(rig, sol);
    const Vec3 tip = tipPos(model, 2, 5.f);
    check(vdist(tip, target) < 0.5f, "ik: CCD converges on a 3-joint chain");
  }

  // 5. FABRIK converges on a 3-joint chain.
  {
    const Rig rig = makeChain3();
    IkChain chain;
    chain.bones = {0, 1, 2};
    const Vec3 target{6, -8, 3};
    const IkSolution sol = solveFabrik(rig, chain, target);
    const auto model = applySolution(rig, sol);
    const Vec3 tip = tipPos(model, 2, 5.f);
    check(vdist(tip, target) < 0.5f, "ik: FABRIK converges on a 3-joint chain");
  }

  // 6. Iterative solvers handle unreachable targets without NaN.
  {
    const Rig rig = makeChain3();
    IkChain chain;
    chain.bones = {0, 1, 2};
    bool ok = true;
    for (const Vec3 target : {Vec3{0, -40, 0}, Vec3{30, 30, 30}}) {
      const IkSolution c = solveCcd(rig, chain, target);
      const IkSolution f = solveFabrik(rig, chain, target);
      for (const auto& sol : {c, f})
        for (const Mat4& m : applySolution(rig, sol)) ok = ok && finiteMat(m);
      // Stretched toward the target: tip at max reach (15) along the target dir.
      const Vec3 dir{target.x, target.y, target.z};
      const float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
      const Vec3 expect{dir.x / dl * 15.f, dir.y / dl * 15.f, dir.z / dl * 15.f};
      const Vec3 tipC = tipPos(applySolution(rig, c), 2, 5.f);
      const Vec3 tipF = tipPos(applySolution(rig, f), 2, 5.f);
      ok = ok && vdist(tipC, expect) < 0.6f && vdist(tipF, expect) < 0.6f;
    }
    check(ok, "ik: CCD/FABRIK stretch to max reach on unreachable targets, finite");
  }

  // 7. Skinning matrix count matches bone count; bind pose is identity-ish.
  {
    const Rig rig = makeChain3();
    const auto bind = bindLocalPoses(rig);
    check(bind.size() == rig.boneCount(), "ik: bindLocalPoses returns one matrix per bone");
    const auto model = computeSkinningMatrices(rig, bind);
    check(model.size() == rig.boneCount(), "ik: computeSkinningMatrices returns one matrix per bone");
    // Bind pose: bone tips land on the bind tails.
    bool ok = true;
    const float lens[3] = {5.f, 5.f, 5.f};
    const Vec3 tails[3] = {{0, -5, 0}, {0, -10, 0}, {0, -15, 0}};
    for (int i = 0; i < 3; ++i) {
      const Vec3 tip = tipPos(model, i, lens[i]);
      if (vdist(tip, tails[i]) > 1e-3f) ok = false;
    }
    check(ok, "ik: bind-pose skinning matrices reproduce the bind tails");
  }

  // 8. Caps respected.
  {
    Rig big;
    for (int i = 0; i < 40; ++i)
      big.bones.push_back({"b" + std::to_string(i), i - 1, {0, float(-i), 0}, {0, float(-i - 1), 0}});
    check(!big.valid(), "ik: rig over the 32-bone cap is invalid");
    const Rig rig = makeChain3();
    check(rig.valid(), "ik: rig within caps is valid");
  }

  std::printf(g_fails ? "ANIM SELFTEST FAILED (%d)\n" : "ANIM SELFTEST OK\n", g_fails);
  return g_fails;
}

}  // namespace litholattice::anim
