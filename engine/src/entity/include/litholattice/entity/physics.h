// Engine entity physics: AABB-vs-voxel move+collide (Minecraft-style).
// The game supplies solidity via isSolid; the engine owns the sweep math.
// Gravity stays in behaviors (different entities fall differently); this only moves and collides.
#pragma once
#include <functional>
#include "litholattice/entity/entity.h"

namespace litholattice {

/// Moves the entity by its velocity * dt, one axis at a time, clamping against solid voxels.
/// (x, y, z) is the entity's feet center; it occupies [x-hw, x+hw] x [y, y+h] x [z-hw, z+hw].
/// Sets e.onGround when landing. Returns true if any axis collided.
/// Substepped (H-ENT-6): the move is split so no substep travels more than
/// ~0.5 blocks on any axis (max 64 substeps), so a large dt cannot tunnel
/// through a 1-block floor; ordinary frame dt is a single step, unchanged.
bool moveCollide(Entity& e, const std::function<bool(int x, int y, int z)>& isSolid,
                 float halfWidth, float height, double dt);

}  // namespace litholattice
