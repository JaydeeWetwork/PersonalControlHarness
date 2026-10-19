// Voxel raycast for audio occlusion.
//
// WHAT: DDA voxel traversal from listener to sound source.
// WHY: Clean-room occlusion — no DLLs, no Steam Audio. Our own raycast
//      through the voxel world to check if walls block sound.
//
// HOW: Amanatides & Woo DDA. Steps through voxels along the ray,
//      calls isSolid(x,y,z) for each. Returns true if any solid voxel
//      blocks the path. Max distance capped at 64 blocks (hearing range).
#pragma once
#include <cmath>
#include <functional>

namespace litholattice {

/// Check if the path from (x0,y0,z0) to (x1,y1,z1) is blocked by solid voxels.
/// isSolid(x,y,z) returns true for opaque blocks. Clean-room DDA.
inline bool voxelRaycastBlocked(float x0, float y0, float z0,
                                 float x1, float y1, float z1,
                                 std::function<bool(int,int,int)> isSolid) {
  const float dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;
  const float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
  if (dist < 0.001f) return false;
  if (dist > 64.0f) return true;  // beyond hearing range = blocked

  // DDA setup
  int x = int(std::floor(x0)), y = int(std::floor(y0)), z = int(std::floor(z0));
  const int ex = int(std::floor(x1)), ey = int(std::floor(y1)), ez = int(std::floor(z1));
  if (x == ex && y == ey && z == ez) return false;  // same voxel: no wall between

  const float rdx = 1.0f / (std::abs(dx) < 1e-9f ? 1e-9f : dx);
  const float rdy = 1.0f / (std::abs(dy) < 1e-9f ? 1e-9f : dy);
  const float rdz = 1.0f / (std::abs(dz) < 1e-9f ? 1e-9f : dz);

  const int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
  float tMaxX = (sx > 0 ? (x + 1 - x0) : (x0 - x)) * std::abs(rdx);
  float tMaxY = (sy > 0 ? (y + 1 - y0) : (y0 - y)) * std::abs(rdy);
  float tMaxZ = (sz > 0 ? (z + 1 - z0) : (z0 - z)) * std::abs(rdz);
  const float tDeltaX = std::abs(rdx), tDeltaY = std::abs(rdy), tDeltaZ = std::abs(rdz);

  // Skip the starting voxel (listener's own position).
  int steps = 0;
  const int maxSteps = 192;  // 64 blocks * 3 axes, safety cap
  while (steps++ < maxSteps) {
    if (tMaxX < tMaxY && tMaxX < tMaxZ) {
      x += sx; tMaxX += tDeltaX;
    } else if (tMaxY < tMaxZ) {
      y += sy; tMaxY += tDeltaY;
    } else {
      z += sz; tMaxZ += tDeltaZ;
    }
    if (x == ex && y == ey && z == ez) break;  // reached target
    if (isSolid(x, y, z)) return true;  // blocked
  }
  return false;  // clear path
}

}  // namespace litholattice
