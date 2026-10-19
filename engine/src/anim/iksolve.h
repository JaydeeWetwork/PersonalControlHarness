// IK solvers for procedural rigs (Worker 2).
//
// Pure functions: no sim state, no side effects. IK runs at render rate and
// never feeds back into the 512 TPS sim (determinism boundary).
//
// Conventions (from rig.h): bones point from head to tail; children attach at
// the parent's tail. Bone local frames: origin at head, +Y along head->tail.
// A solver's output is local (parent-space) transforms for the chain bones;
// splice them into bindLocalPoses() output and run computeSkinningMatrices()
// for the model-space matrices the GPU skinning uniform block wants.
#pragma once
#include <string>

#include <vector>

#include "litholattice/anim/rig.h"  // Bone, Rig, IkChain, caps

namespace litholattice::anim {

/// IK solution: local (parent-space) transforms for the solved chain bones.
struct IkSolution {
  std::vector<int> bones;   ///< bone indices, root..tip (mirrors IkChain::bones)
  std::vector<Mat4> local;  ///< local transform per bone, same order as bones
};

/// Analytic two-bone IK (shoulder->elbow->wrist), closed form.
/// chain.bones must have exactly 2 entries. rootPos is the chain root's head
/// position in the space target/pole are given in (rig model space when the
/// chain root's ancestors are at bind). pole is the elbow bend-direction hint
/// (a direction, not a position; need not be normalized; zero is tolerated).
/// Unreachable targets clamp to max extension; the straight-line degenerate
/// case uses the pole direction and never produces NaN.
IkSolution solveTwoBone(const Rig& rig, const IkChain& chain, Vec3 rootPos, Vec3 target,
                        Vec3 pole);

/// Cyclic coordinate descent for chains up to 4 joints.
/// Starts from the bind pose; iterates root-ward from the tip, stopping early
/// when the tip lands within epsilon of the target. maxIters caps at 10.
IkSolution solveCcd(const Rig& rig, const IkChain& chain, Vec3 target, int maxIters = 10,
                    float epsilon = 0.05f);

/// Forward-and-backward reaching IK, same interface as CCD.
/// Position-based; typically converges in fewer iterations than CCD.
IkSolution solveFabrik(const Rig& rig, const IkChain& chain, Vec3 target, int maxIters = 10,
                       float epsilon = 0.05f);

/// Bind-pose local transforms for every bone: local[i] maps the parent bone's
/// model space to bone i's model space (translation at head, +Y along
/// head->tail). For the root, the parent space is rig model space.
std::vector<Mat4> bindLocalPoses(const Rig& rig);

/// Compose the hierarchy into model-space bone matrices, one per bone in
/// Rig::bones order: out[i] = out[parent[i]] * localPoses[i].
/// Suitable for upload as the GPU skinning uniform block.
std::vector<Mat4> computeSkinningMatrices(const Rig& rig, const std::vector<Mat4>& localPoses);

/// Validate a rig before solving: bone count within cap, root present, parents
/// in range and acyclic, influence indices/weights sane. Returns false with
/// *err describing the first problem. Authored rig templates MUST pass this;
/// riggen output always does.
bool validateRig(const Rig& rig, std::string* err = nullptr);

/// Self-test entry point (wired into engine_tests as module "anim").
int runAnimSelfTest();

}  // namespace litholattice::anim
