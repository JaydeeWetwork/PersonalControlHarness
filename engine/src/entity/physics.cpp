#include "litholattice/entity/physics.h"
#include <algorithm>
#include <cmath>

namespace litholattice {
namespace {

// True when the AABB [x-hw, x+hw] x [y, y+h] x [z-hw, z+hw] touches a solid voxel.
bool aabbCollides(double x, double y, double z, float hw, float h,
                  const std::function<bool(int, int, int)>& isSolid) {
  const int x0 = int(std::floor(x - hw)), x1 = int(std::floor(x + hw));
  const int y0 = int(std::floor(y)), y1 = int(std::floor(y + h));
  const int z0 = int(std::floor(z - hw)), z1 = int(std::floor(z + hw));
  for (int vx = x0; vx <= x1; ++vx)
    for (int vy = y0; vy <= y1; ++vy)
      for (int vz = z0; vz <= z1; ++vz)
        if (isSolid(vx, vy, vz)) return true;
  return false;
}

}  // namespace

namespace {

// One axis-swept step of the full move (the original single-step body).
bool moveCollideStep(Entity& e, const std::function<bool(int x, int y, int z)>& isSolid,
                     float halfWidth, float height, double dt) {
  bool hit = false;
  // X axis
  {
    const double nx = e.x + e.vx * dt;
    if (aabbCollides(nx, e.y, e.z, halfWidth, height, isSolid)) {
      e.x = e.vx > 0 ? std::floor(nx + halfWidth) - halfWidth - 1e-4
                     : std::floor(nx - halfWidth) + 1 + halfWidth + 1e-4;
      e.vx = 0;
      hit = true;
    } else {
      e.x = nx;
    }
  }
  // Z axis
  {
    const double nz = e.z + e.vz * dt;
    if (aabbCollides(e.x, e.y, nz, halfWidth, height, isSolid)) {
      e.z = e.vz > 0 ? std::floor(nz + halfWidth) - halfWidth - 1e-4
                     : std::floor(nz - halfWidth) + 1 + halfWidth + 1e-4;
      e.vz = 0;
      hit = true;
    } else {
      e.z = nz;
    }
  }
  // Y axis
  e.onGround = false;
  {
    const double ny = e.y + e.vy * dt;
    if (aabbCollides(e.x, ny, e.z, halfWidth, height, isSolid)) {
      if (e.vy <= 0) {
        e.y = std::floor(ny) + 1;  // rest on top of the voxel
        e.onGround = true;
      } else {
        e.y = std::floor(ny + height) - height - 1e-4;  // bonk head
      }
      e.vy = 0;
      hit = true;
    } else {
      e.y = ny;
    }
  }
  return hit;
}

}  // namespace

bool moveCollide(Entity& e, const std::function<bool(int x, int y, int z)>& isSolid,
                 float halfWidth, float height, double dt) {
  // H-ENT-6 substepping: a single step tests only the DESTINATION AABB, so a
  // large dt (frame hitch: terminal fall 24 b/s x 250 ms = 6 blocks) tunnels
  // through a 1-block floor between start and destination. Split the move so
  // no substep travels more than ~0.5 blocks on any axis. Ordinary frame dt
  // (<= ~0.4 blocks/step) stays a single step — bit-identical to before.
  const double stepX = std::fabs(e.vx * dt), stepY = std::fabs(e.vy * dt),
               stepZ = std::fabs(e.vz * dt);
  const double longest = std::max(stepX, std::max(stepY, stepZ));
  int steps = 1;
  if (longest > 0.5) steps = std::min(64, int(std::ceil(longest / 0.5)));
  const double subDt = dt / steps;
  bool hit = false;
  for (int i = 0; i < steps; ++i) {
    hit = moveCollideStep(e, isSolid, halfWidth, height, subDt) || hit;
    // Once at rest on the ground with no remaining velocity anywhere, the
    // remaining substeps cannot move the entity; stop early.
    if (e.vx == 0 && e.vy == 0 && e.vz == 0) break;
  }
  return hit;
}

}  // namespace litholattice
