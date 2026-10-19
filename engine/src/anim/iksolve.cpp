// IK solvers: analytic two-bone, CCD, FABRIK. See iksolve.h for the contract.
#include "iksolve.h"

#include <cmath>
#include <functional>

#include "litholattice/core/math.h"

namespace litholattice::anim {
namespace {

// ---- local math helpers (core math.h has no Vec3 subtraction or quaternions) ----

inline Vec3 vsub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float vlen(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 snorm(Vec3 a) {  // normalize with zero guard (core normalize() has none)
  const float l = vlen(a);
  return l < 1e-9f ? Vec3{0, 1, 0} : a * (1.f / l);
}

struct Quat {
  float w = 1, x = 0, y = 0, z = 0;
};
inline Quat qmul(Quat a, Quat b) {
  return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
          a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
inline Quat qconj(Quat q) { return {q.w, -q.x, -q.y, -q.z}; }
inline Quat qnorm(Quat q) {
  const float l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  if (l < 1e-12f) return {1, 0, 0, 0};
  return {q.w / l, q.x / l, q.y / l, q.z / l};
}
inline Vec3 qrot(Quat q, Vec3 v) {  // rotate v by unit quaternion q
  const Quat p{0, v.x, v.y, v.z};
  const Quat r = qmul(qmul(q, p), qconj(q));
  return {r.x, r.y, r.z};
}
/// Shortest-arc rotation taking unit vector a to unit vector b. NaN-free:
/// identity for parallel, arbitrary perpendicular axis for antiparallel.
inline Quat fromTwoVectors(Vec3 a, Vec3 b) {
  const float d = dot(a, b);
  if (d >= 1.f - 1e-6f) return {1, 0, 0, 0};
  if (d <= -1.f + 1e-6f) {
    const Vec3 ref = std::fabs(a.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const Vec3 p = snorm(cross(a, ref));
    return {0, p.x, p.y, p.z};  // 180 degrees about p
  }
  const Vec3 c = cross(a, b);
  return qnorm({1.f + d, c.x, c.y, c.z});
}
inline Mat4 quatToMat4(Quat q) {  // column-major rotation
  const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
  const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  Mat4 r = Mat4::identity();
  r.m[0] = 1 - 2 * (yy + zz); r.m[1] = 2 * (xy + wz);     r.m[2] = 2 * (xz - wy);
  r.m[4] = 2 * (xy - wz);     r.m[5] = 1 - 2 * (xx + zz); r.m[6] = 2 * (yz + wx);
  r.m[8] = 2 * (xz + wy);     r.m[9] = 2 * (yz - wx);     r.m[10] = 1 - 2 * (xx + yy);
  return r;
}
inline Mat4 translation(Vec3 t) {
  Mat4 r = Mat4::identity();
  r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
  return r;
}
inline Vec3 xformPoint(const Mat4& m, Vec3 v) {
  return {m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12],
          m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13],
          m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14]};
}
/// Inverse of a rigid transform (rotation + translation only).
inline Mat4 rigidInverse(const Mat4& m) {
  Mat4 r;
  // transpose the 3x3 rotation
  r.m[0] = m.m[0]; r.m[1] = m.m[4]; r.m[2] = m.m[8];   r.m[3] = 0;
  r.m[4] = m.m[1]; r.m[5] = m.m[5]; r.m[6] = m.m[9];   r.m[7] = 0;
  r.m[8] = m.m[2]; r.m[9] = m.m[6]; r.m[10] = m.m[10]; r.m[11] = 0;
  // -R^T * t
  r.m[12] = -(r.m[0] * m.m[12] + r.m[4] * m.m[13] + r.m[8] * m.m[14]);
  r.m[13] = -(r.m[1] * m.m[12] + r.m[5] * m.m[13] + r.m[9] * m.m[14]);
  r.m[14] = -(r.m[2] * m.m[12] + r.m[6] * m.m[13] + r.m[10] * m.m[14]);
  r.m[15] = 1;
  return r;
}

// ---- bind-pose helpers ----

inline float boneLen(const Rig& rig, int i) {
  return vlen(vsub(rig.bones[size_t(i)].tail, rig.bones[size_t(i)].head));
}
inline Vec3 bindDir(const Rig& rig, int i) {  // unit, model space
  return snorm(vsub(rig.bones[size_t(i)].tail, rig.bones[size_t(i)].head));
}
inline Quat bindModelQuat(const Rig& rig, int i) {  // +Y -> bindDir
  return fromTwoVectors(Vec3{0, 1, 0}, bindDir(rig, i));
}
inline Mat4 bindModelMat(const Rig& rig, int i) {
  return mul(translation(rig.bones[size_t(i)].head), quatToMat4(bindModelQuat(rig, i)));
}
/// Local transform of bone i in its parent's bind model space.
inline Mat4 bindLocalMat(const Rig& rig, int i) {
  const int p = rig.bones[size_t(i)].parent;
  if (p < 0) return bindModelMat(rig, i);
  return mul(rigidInverse(bindModelMat(rig, p)), bindModelMat(rig, i));
}
/// Translation part of the bind local matrix (head in parent space).
inline Vec3 bindLocalPos(const Rig& rig, int i) {
  const Mat4 l = bindLocalMat(rig, i);
  return {l.m[12], l.m[13], l.m[14]};
}

}  // namespace

std::vector<Mat4> bindLocalPoses(const Rig& rig) {
  std::vector<Mat4> out;
  out.reserve(rig.bones.size());
  for (size_t i = 0; i < rig.bones.size(); ++i) out.push_back(bindLocalMat(rig, int(i)));
  return out;
}

std::vector<Mat4> computeSkinningMatrices(const Rig& rig, const std::vector<Mat4>& localPoses) {
  const size_t n = rig.bones.size();
  std::vector<Mat4> out(n, Mat4::identity());
  if (localPoses.size() != n || n == 0) return out;
  // Parent-first order, iterative with explicit cycle breaking: state 0 =
  // unvisited, 1 = in-progress, 2 = done. Revisiting an in-progress bone means
  // a parent cycle (possible in unvalidated authored templates) — break it by
  // treating the bone as a root rather than recursing forever.
  std::vector<char> state(n, 0);
  std::vector<size_t> stack;
  for (size_t root = 0; root < n; ++root) {
    if (state[root] != 0) continue;
    size_t i = root;
    for (;;) {
      if (state[i] == 2) break;
      if (state[i] == 1) {  // cycle: break by treating i as a root
        out[i] = localPoses[i];
        state[i] = 2;
        break;
      }
      state[i] = 1;
      stack.push_back(i);
      const int p = rig.bones[i].parent;
      if (p < 0 || size_t(p) >= n) break;  // root or dangling: stop climbing
      i = size_t(p);
    }
    // Unwind: parents before children (stack holds the chain root-last).
    while (!stack.empty()) {
      const size_t j = stack.back();
      stack.pop_back();
      if (state[j] == 2) continue;
      const int p = rig.bones[j].parent;
      out[j] = (p < 0 || size_t(p) >= n || state[size_t(p)] != 2)
                   ? localPoses[j]
                   : mul(out[size_t(p)], localPoses[j]);
      state[j] = 2;
    }
  }
  return out;
}

bool validateRig(const Rig& rig, std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    return false;
  };
  const size_t n = rig.bones.size();
  if (n == 0) return fail("rig has no bones");
  if (n > size_t(kMaxBones)) return fail("rig exceeds kMaxBones");
  if (rig.bones[0].parent != -1) return fail("bones[0] must be the root (parent -1)");
  for (size_t i = 0; i < n; ++i) {
    const int p = rig.bones[i].parent;
    if (p < -1 || size_t(p) >= n) return fail("bone " + std::to_string(i) + " has out-of-range parent");
    if (p == int(i)) return fail("bone " + std::to_string(i) + " is its own parent");
  }
  // Acyclic parents: walk each chain with a step cap.
  for (size_t i = 0; i < n; ++i) {
    size_t cur = i;
    for (size_t hop = 0; hop <= n; ++hop) {
      const int p = rig.bones[cur].parent;
      if (p < 0) break;
      cur = size_t(p);
      if (hop == n) return fail("parent cycle detected at bone " + std::to_string(i));
    }
  }
  // Influence bounds (per-vertex; count is unrelated to bone count).
  for (size_t v = 0; v < rig.influences.size(); ++v) {
    const auto& inf = rig.influences[v];
    if (inf.size() > size_t(kMaxInfluences))
      return fail("vertex " + std::to_string(v) + " exceeds kMaxInfluences");
    float sum = 0.f;
    for (const auto& in : inf) {
      if (size_t(in.bone) >= n) return fail("vertex influence references out-of-range bone");
      if (!std::isfinite(in.weight) || in.weight < 0.f) return fail("vertex influence has bad weight");
      sum += in.weight;
    }
    if (!inf.empty() && std::fabs(sum - 1.f) > 1e-3f)
      return fail("vertex influence weights must sum to 1");
  }
  return true;
}

// NaN guard: IK inputs come from render-rate entity positions; a single degenerate
// entity must not NaN the whole skeleton. Any non-finite input -> bind-pose fallback.
inline bool finiteVec3(Vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// ---- two-bone analytic ----

IkSolution solveTwoBone(const Rig& rig, const IkChain& chain, Vec3 rootPos, Vec3 target,
                        Vec3 pole) {
  IkSolution sol;
  if (chain.bones.size() != 2) return sol;
  const int b0 = chain.bones[0], b1 = chain.bones[1];
  if (b0 < 0 || b1 < 0 || size_t(b0) >= rig.bones.size() || size_t(b1) >= rig.bones.size())
    return sol;
  if (!finiteVec3(rootPos) || !finiteVec3(target) || !finiteVec3(pole)) {
    sol.bones = {b0, b1};  // non-finite input: bind pose, never NaN matrices
    sol.local = {bindLocalMat(rig, b0), bindLocalMat(rig, b1)};
    return sol;
  }
  // H-ANIM-1 residual: validate the RIG's own bone head/tail finiteness, like
  // solveCcd/solveFabrik do via chainJoints. A NaN rig (corrupt asset data,
  // not NaN input) otherwise poisoned the solve: boneLen/bindDir of a NaN
  // bone produce NaN quaternions and matrices. No solution on a corrupt rig.
  if (!finiteVec3(rig.bones[size_t(b0)].head) || !finiteVec3(rig.bones[size_t(b0)].tail) ||
      !finiteVec3(rig.bones[size_t(b1)].head) || !finiteVec3(rig.bones[size_t(b1)].tail))
    return sol;
  const float L1 = boneLen(rig, b0), L2 = boneLen(rig, b1);
  if (L1 < 1e-6f || L2 < 1e-6f) {  // degenerate bone: fall back to bind
    sol.bones = {b0, b1};
    sol.local = {bindLocalMat(rig, b0), bindLocalMat(rig, b1)};
    return sol;
  }

  Vec3 toT = vsub(target, rootPos);
  float d = vlen(toT);
  const float maxD = L1 + L2;
  const float minD = std::fabs(L1 - L2);
  if (d > maxD) {  // unreachable: clamp to full extension
    toT = toT * (maxD / d);
    d = maxD;
  } else if (d < minD) {  // inside the inner sphere: fold as far as possible
    if (d < 1e-6f) {
      toT = snorm(pole) * minD;
    } else {
      toT = toT * (minD / d);
    }
    d = minD;
  }
  if (d < 1e-6f) {  // target exactly at root: keep bind orientation
    sol.bones = {b0, b1};
    sol.local = {bindLocalMat(rig, b0), bindLocalMat(rig, b1)};
    return sol;
  }
  const Vec3 dir = toT * (1.f / d);

  // Bend direction: pole component perpendicular to dir (zero-tolerant).
  Vec3 bend = vsub(pole, dir * dot(pole, dir));
  float bendLen = vlen(bend);
  if (bendLen < 1e-6f) {
    // Pole parallel to dir (or zero): pick any perpendicular.
    const Vec3 ref = std::fabs(dir.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    bend = cross(dir, ref);
    bendLen = vlen(bend);
  }
  bend = bend * (1.f / bendLen);

  // Law of cosines: angle between target dir and the upper-arm direction.
  float cosA = (L1 * L1 + d * d - L2 * L2) / (2.f * L1 * d);
  cosA = cosA < -1.f ? -1.f : (cosA > 1.f ? 1.f : cosA);
  const float sinA = std::sqrt(std::max(0.f, 1.f - cosA * cosA));
  const Vec3 uDir = dir * cosA + bend * sinA;  // already unit
  const Vec3 elbow = rootPos + uDir * L1;
  const Vec3 clampedTarget = rootPos + dir * d;
  const Vec3 fDir = snorm(vsub(clampedTarget, elbow));

  // Model-space delta rotations from bind dirs to solved dirs.
  const Quat qDelta0 = fromTwoVectors(bindDir(rig, b0), uDir);
  const Quat Q0new = qmul(qDelta0, bindModelQuat(rig, b0));
  const Quat qDelta1 = fromTwoVectors(bindDir(rig, b1), fDir);
  const Quat Q1new = qmul(qDelta1, bindModelQuat(rig, b1));

  // To local: parent's (possibly new) model quat -> bone's new model quat.
  const int p0 = rig.bones[size_t(b0)].parent;
  const Quat Qp0 = (p0 < 0 || size_t(p0) >= rig.bones.size()) ? Quat{1, 0, 0, 0}
                                                             : bindModelQuat(rig, p0);
  const Quat q0local = qmul(qconj(Qp0), Q0new);
  const Quat q1local = qmul(qconj(Q0new), Q1new);

  sol.bones = {b0, b1};
  sol.local = {mul(translation(bindLocalPos(rig, b0)), quatToMat4(q0local)),
               mul(translation(bindLocalPos(rig, b1)), quatToMat4(q1local))};
  return sol;
}

// ---- iterative solvers: shared joint-position machinery ----

namespace {

// Joint positions for a chain: joint[0] = head(bones[0]), joint[i+1] = tail(bones[i]).
// Returns false when the chain is unusable.
bool chainJoints(const Rig& rig, const IkChain& chain, std::vector<Vec3>& joints,
                 std::vector<float>& lens) {
  const size_t n = chain.bones.size();
  if (n == 0 || n > 4) return false;
  for (int b : chain.bones)
    if (b < 0 || size_t(b) >= rig.bones.size()) return false;
  joints.resize(n + 1);
  lens.resize(n);
  joints[0] = rig.bones[size_t(chain.bones[0])].head;
  for (size_t i = 0; i < n; ++i) {
    const Bone& bo = rig.bones[size_t(chain.bones[i])];
    joints[i + 1] = bo.tail;
    lens[i] = vlen(vsub(bo.tail, bo.head));
    if (lens[i] < 1e-6f) return false;
  }
  return true;
}

// Convert solved model-space quats to local transforms for the chain bones.
IkSolution quatsToLocal(const Rig& rig, const IkChain& chain, const std::vector<Quat>& qNew) {
  IkSolution sol;
  const size_t n = chain.bones.size();
  sol.bones.assign(chain.bones.begin(), chain.bones.end());
  sol.local.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const int b = chain.bones[i];
    Quat qParent;
    const int actualParent = rig.bones[size_t(b)].parent;
    if (i > 0 && actualParent == chain.bones[i - 1]) {
      qParent = qNew[i - 1];  // parent is the previous chain bone: use its new quat
    } else {
      qParent = (actualParent < 0 || size_t(actualParent) >= rig.bones.size())
                    ? Quat{1, 0, 0, 0}
                    : bindModelQuat(rig, actualParent);
    }
    const Quat ql = qmul(qconj(qParent), qNew[i]);
    sol.local.push_back(mul(translation(bindLocalPos(rig, b)), quatToMat4(ql)));
  }
  return sol;
}

}  // namespace

IkSolution solveCcd(const Rig& rig, const IkChain& chain, Vec3 target, int maxIters,
                    float epsilon) {
  IkSolution sol;
  if (!finiteVec3(target)) return sol;  // non-finite target: no solution, never NaN
  std::vector<Vec3> joints;
  std::vector<float> lens;
  if (!chainJoints(rig, chain, joints, lens)) return sol;
  for (const Vec3 j : joints)
    if (!finiteVec3(j)) return sol;  // corrupt rig joints: no solution
  const size_t n = chain.bones.size();
  if (maxIters > 10) maxIters = 10;
  if (maxIters < 1) maxIters = 1;

  // Model-space quats, starting at bind.
  std::vector<Quat> q(n);
  for (size_t i = 0; i < n; ++i) q[i] = bindModelQuat(rig, chain.bones[i]);

  for (int it = 0; it < maxIters; ++it) {
    for (size_t j = n; j-- > 0;) {  // tip-ward joint first... root-ward: j = n-1..0
      const Vec3 pj = joints[j];
      const Vec3 v1 = vsub(joints[n], pj);  // joint -> tip
      const Vec3 v2 = vsub(target, pj);     // joint -> target
      const float l1 = vlen(v1), l2 = vlen(v2);
      if (l1 < 1e-9f || l2 < 1e-9f) continue;
      const Quat dq = fromTwoVectors(v1 * (1.f / l1), v2 * (1.f / l2));
      q[j] = qmul(dq, q[j]);
      for (size_t k = j + 1; k < n; ++k) q[k] = qmul(dq, q[k]);  // downstream bones only
      for (size_t k = j + 1; k <= n; ++k)
        joints[k] = pj + qrot(dq, vsub(joints[k], pj));  // downstream joints
    }
    if (vlen(vsub(joints[n], target)) < epsilon) break;
  }
  return quatsToLocal(rig, chain, q);
}

IkSolution solveFabrik(const Rig& rig, const IkChain& chain, Vec3 target, int maxIters,
                       float epsilon) {
  IkSolution sol;
  if (!finiteVec3(target)) return sol;  // non-finite target: no solution, never NaN
  std::vector<Vec3> joints;
  std::vector<float> lens;
  if (!chainJoints(rig, chain, joints, lens)) return sol;
  for (const Vec3 j : joints)
    if (!finiteVec3(j)) return sol;  // corrupt rig joints: no solution
  const size_t n = chain.bones.size();
  if (maxIters > 10) maxIters = 10;
  if (maxIters < 1) maxIters = 1;

  const Vec3 root = joints[0];
  float total = 0;
  for (float l : lens) total += l;
  const float dist = vlen(vsub(target, root));

  if (dist > total) {
    // Unreachable: stretch straight at the target.
    const Vec3 dir = vsub(target, root) * (1.f / dist);
    for (size_t i = 0; i < n; ++i) joints[i + 1] = joints[i] + dir * lens[i];
  } else {
    for (int it = 0; it < maxIters; ++it) {
      // Backward: tip to target, pull the chain along.
      joints[n] = target;
      for (size_t i = n; i-- > 0;) {
        const Vec3 d = snorm(vsub(joints[i], joints[i + 1]));
        joints[i] = joints[i + 1] + d * lens[i];
      }
      // Forward: root pinned, push back out.
      joints[0] = root;
      for (size_t i = 0; i < n; ++i) {
        const Vec3 d = snorm(vsub(joints[i + 1], joints[i]));
        joints[i + 1] = joints[i] + d * lens[i];
      }
      if (vlen(vsub(joints[n], target)) < epsilon) break;
    }
  }

  // Joint positions -> model quats -> local.
  std::vector<Quat> q(n);
  for (size_t i = 0; i < n; ++i) {
    const Vec3 d = snorm(vsub(joints[i + 1], joints[i]));
    q[i] = qmul(fromTwoVectors(bindDir(rig, chain.bones[i]), d),
                bindModelQuat(rig, chain.bones[i]));
  }
  return quatsToLocal(rig, chain, q);
}

}  // namespace litholattice::anim
