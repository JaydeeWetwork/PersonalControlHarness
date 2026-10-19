// Procedural rig generation: voxel model -> Rig (skeleton extraction + skinning).
//
// Worker 1 of the IK/boning system. Builds against rig.h (Bone, Rig, Influence;
// caps kMaxBones=32, kMaxInfluences=4 — never raised here).
//
// Pipeline (all deterministic: same voxels -> same rig, every time):
//   1. Chamfer distance transform over the voxel volume (medialness weights).
//   2. Weighted PCA (weight = distance value) -> spine axis = longest medial path.
//   3. Slice along the axis -> 1-4 spine bones ("spine", "spine_1", ...);
//      top segment above the highest limb -> "head" bone.
//   4. Limb detection: voxels far from the axis (or outside the torso t-range)
//      cluster by 26-connectivity; each limb gets 2-3 bones along its own PCA
//      axis ("limb_0_upper", "limb_0_lower", ...). Thin limbs (< 8 voxels)
//      stay rigid; smallest limbs drop first if over kMaxBones.
//   5. Skinning: per-bone Dijkstra flood-fill through the filled volume (6-
//      connectivity), K-best (K=4) per voxel, inverse-square falloff,
//      weights normalized to 1. A limb's voxels follow their own bone, never
//      a nearer bone across empty space.
//
// extractRig() and computeSkinning() must be called as a pair, on the same
// thread: extraction retains the voxel grid for the flood-fill skinning step.
// (The production path extracts, skins, then caches the Rig with the model.)
// Performance: extraction runs in well under 200 ms on a 32^3 model; this is
// load-time work, never per-frame. IK itself runs at render rate (see rig.h).
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "litholattice/anim/rig.h"
#include "litholattice/core/math.h"  // Vec3

namespace litholattice::anim {

/// One filled voxel of the input model.
struct VoxelCell {
  int x = 0, y = 0, z = 0;
};

/// Input to extraction: a simple filled-voxel grid. Convertible from a
/// LithoStampModel (x, y, z cells; the palette/material byte is ignored —
/// rigging cares about shape, not paint).
struct VoxelGrid {
  int size = 0;                  ///< cubic grid edge; cells live in [0, size)
  std::vector<VoxelCell> cells;  ///< filled cells
};

/// Build a Rig from a voxel model. Returns nullopt for an empty/invalid grid.
/// The returned Rig has bones populated; per-vertex influences are filled by a
/// paired computeSkinning() call. Deterministic: identical input -> identical
/// Rig (bone names, order, positions bit-identical).
std::optional<Rig> extractRig(const VoxelGrid& grid);

/// Fill rig.influences for `vertices` (model space, voxel units; matches the
/// meshed model's vertex order). Each vertex gets <= kMaxInfluences entries
/// whose weights sum to 1. Must follow extractRig() for the same model on the
/// same thread.
void computeSkinning(Rig& rig, const std::vector<Vec3>& vertices);

/// Self-test entry point (wired into the engine `anim` ctest module).
int runRiggenSelfTest();

}  // namespace litholattice::anim
