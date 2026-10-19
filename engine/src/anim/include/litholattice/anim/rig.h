// Procedural rigging for Litho Formula entities (IK/boning system).
//
// Locked design (JD 2026-10-12):
// - 32 bones per mob max (fits a 2KB uniform block; power of two).
// - 4 influences per vertex max (GPU-friendly skinning).
// - IK runs at render rate, NEVER at the 512 TPS sim rate. Poses derive from
//   sim state one-way; they never feed back (determinism).
// - Procedural rigs AND authored rig templates both supported.
// - Skeleton extraction is deterministic per model: same voxels, same rig.
//
// Two workers build against this header:
//   Worker 1 (riggen): skeleton extraction + skinning (voxel model -> Rig).
//   Worker 2 (iksolve): two-bone analytic + CCD/FABRIK solvers (Rig -> pose).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "litholattice/core/math.h"  // Vec3, Mat4

namespace litholattice::anim {

/// Hard cap: 32 bones per rig (uniform-block budget).
inline constexpr int kMaxBones = 32;
/// Hard cap: 4 skinning influences per vertex.
inline constexpr int kMaxInfluences = 4;
/// Hard cap: 4 simultaneously active IK chains per mob.
inline constexpr int kMaxIkChains = 4;

/// One bone: a joint (head) and the bone's far end (tail) in model space.
/// Children attach at their parent's tail by convention.
struct Bone {
  std::string name;   ///< "spine", "upper_arm_l", ...
  int parent = -1;    ///< index into Rig::bones; -1 for the root
  Vec3 head{0, 0, 0}; ///< joint position, model space (voxels)
  Vec3 tail{0, 0, 0}; ///< bone end, model space (voxels)
};

/// One skinning influence: which bone moves this vertex, and how much.
struct Influence {
  uint8_t bone = 0;   ///< index into Rig::bones
  float weight = 0.f; ///< sums to 1 across a vertex's influences
};

/// A rig: the skeleton plus per-vertex skinning weights.
/// Produced by Worker 1 (extraction+skinning or authored template load),
/// consumed by Worker 2 (IK solvers) and the renderers (GPU skinning).
struct Rig {
  std::vector<Bone> bones;  ///< <= kMaxBones; bones[0] is the root
  /// Per-vertex skinning: influences[v] has <= kMaxInfluences entries.
  /// Vertex indexing matches the meshed model's vertex order.
  std::vector<std::vector<Influence>> influences;

  bool valid() const { return !bones.empty() && bones.size() <= size_t(kMaxBones); }
  size_t boneCount() const { return bones.size(); }
};

/// A chain of bone indices for IK (root .. tip, in order).
struct IkChain {
  std::vector<int> bones;  ///< <= 4 entries for CCD/FABRIK; exactly 2 for analytic
};

}  // namespace litholattice::anim
