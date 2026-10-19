// ProjectileManager implementation (P2c 2026-10-13, review ENT-1).
// Semantics are documented in projectile.h; this file is the one place
// they are enforced.
#include "litholattice/projectile/projectile.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace litholattice {

namespace {

uint64_t chunkKey(int cx, int cz) {
  return (uint64_t(uint32_t(cx)) << 32) | uint64_t(uint32_t(cz));
}
int floorDiv16(double v) { return int(std::floor(v / 16.0)); }

}  // namespace

uint64_t ProjectileManager::spawn(const Projectile& p, IProjectileBehavior* behavior) {
  Projectile q = p;
  if (q.id == 0) {
    while (entries_.count(nextId_)) ++nextId_;
    q.id = nextId_++;
  } else if (entries_.count(q.id)) {
    return 0;  // duplicate caller id: first wins, nothing changes
  }
  Entry e;
  e.p = q;
  e.behavior = behavior;
  e.traveledExact = double(q.traveled);
  const uint64_t id = q.id;
  entries_.emplace(id, e);
  order_.push_back(id);
  // Warm-up: request the path's columns immediately, not one tick late.
  loadChunksAhead(q.x, q.y, q.z, q.vx, q.vy, q.vz, q.maxRange ? q.maxRange : uint32_t(kChunkLookaheadBlocks));
  return id;
}

void ProjectileManager::eraseEntry(uint64_t id) {
  entries_.erase(id);
  order_.erase(std::remove(order_.begin(), order_.end(), id), order_.end());
}

void ProjectileManager::despawn(uint64_t id) { eraseEntry(id); }

const Projectile* ProjectileManager::get(uint64_t id) const {
  const auto it = entries_.find(id);
  return it == entries_.end() ? nullptr : &it->second.p;
}

void ProjectileManager::clear() {
  entries_.clear();
  order_.clear();
}

void ProjectileManager::requestColumn(int cx, int cz) {
  if (!chunkReq_) return;
  if (requestedChunks_.insert(chunkKey(cx, cz)).second) chunkReq_(cx, cz);
}

void ProjectileManager::loadChunksAhead(double x, double y, double z, double dx, double dy, double dz,
                                        uint32_t distance) {
  if (!chunkReq_ || distance == 0) return;
  const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (!(len > 0.0)) return;
  // Sample the segment every half column, endpoints included: a straight
  // ray crosses a new column at most every ~11 blocks (diagonal), so an
  // 8-block stride cannot skip one.
  const double step = 8.0;
  const int n = int(std::ceil(double(distance) / step));
  for (int i = 0; i <= n; ++i) {
    const double t = std::min(double(distance), i * step) / len;
    requestColumn(floorDiv16(x + dx * t), floorDiv16(z + dz * t));
  }
}

void ProjectileManager::tick() {
  // Snapshot the id order: behaviors may spawn/despawn during their own
  // onTick (a hit that fires a return projectile, a splitter, ...).
  const std::vector<uint64_t> ids = order_;
  for (const uint64_t id : ids) {
    const auto it0 = entries_.find(id);
    if (it0 == entries_.end()) continue;  // despawned earlier this tick
    Entry& e = it0->second;
    Projectile& p = e.p;

    // Chunk loading ahead of the path (per tick, deduplicated).
    loadChunksAhead(p.x, p.y, p.z, p.vx, p.vy, p.vz, uint32_t(kChunkLookaheadBlocks));

    // Boomerang steering: on the return leg, aim at the owner when the
    // seam can say where the owner is (speed preserved).
    if (p.returning && ownerPos_ && p.ownerId != 0) {
      double ox = 0, oy = 0, oz = 0;
      if (ownerPos_(p.ownerId, ox, oy, oz)) {
        const double dx = ox - p.x, dy = oy - p.y, dz = oz - p.z;
        const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
        const double speed = std::sqrt(p.vx * p.vx + p.vy * p.vy + p.vz * p.vz);
        if (d > 1e-9 && speed > 0.0) {
          p.vx = dx / d * speed;
          p.vy = dy / d * speed;
          p.vz = dz / d * speed;
        }
      }
    }

    // -- Swept movement -------------------------------------------------
    const double speed = std::sqrt(p.vx * p.vx + p.vy * p.vy + p.vz * p.vz);
    bool hitBlock = false, stopped = false;
    uint64_t hitEntity = 0;
    if (speed > 0.0) {
      const int substeps = std::max(1, int(std::ceil(speed / kMaxSubstepBlocks)));
      const double sx = p.vx / substeps, sy = p.vy / substeps, sz = p.vz / substeps;
      const double subLen = speed / substeps;
      for (int i = 0; i < substeps; ++i) {
        const double nx = p.x + sx, ny = p.y + sy, nz = p.z + sz;
        // Entity hit at the substep endpoint (owner excluded by contract).
        if (entity_) {
          if (const uint64_t hit = entity_(nx, ny, nz, p.ownerId)) {
            hitEntity = hit;
            p.x = nx; p.y = ny; p.z = nz;
            e.traveledExact += subLen;
            stopped = true;
            break;
          }
        }
        // Block hit: the voxel containing the new point.
        if (voxel_) {
          const VoxelInfo v = voxel_(int(std::floor(nx)), int(std::floor(ny)), int(std::floor(nz)));
          if (v.solid) {
            hitBlock = true;
            if (p.penetration > v.density) {
              // Pierces: pass through, keep sweeping (the hit is still
              // reported to the behavior once, below).
              p.x = nx; p.y = ny; p.z = nz;
              e.traveledExact += subLen;
              continue;
            }
            // Stops at the contact point: the last clear position.
            stopped = true;
            break;
          }
        }
        p.x = nx; p.y = ny; p.z = nz;
        e.traveledExact += subLen;
      }
    }
    p.traveled = uint32_t(e.traveledExact);

    // -- Behavior --------------------------------------------------------
    bool keep = true;
    if (e.behavior) keep = e.behavior->onTick(p, hitBlock, hitEntity);

    // -- Despawn / turn-around rules -------------------------------------
    bool dead = !keep;
    if (!dead && stopped && !p.returning && p.returnsToOwner) {
      // Boomerang bounce: a kept-alive projectile that hit something on
      // its outbound leg turns around instead of dying in the wall.
      p.returning = true;
      p.vx = -p.vx; p.vy = -p.vy; p.vz = -p.vz;
      e.traveledExact = 0;
      p.traveled = 0;
    } else if (!dead && stopped) {
      dead = true;  // unpierced hit, no (living) boomerang turn
    }
    if (!dead && speed > 0.0 && p.maxRange > 0 && e.traveledExact >= double(p.maxRange)) {
      if (p.returnsToOwner && !p.returning) {
        p.returning = true;
        p.vx = -p.vx; p.vy = -p.vy; p.vz = -p.vz;
        e.traveledExact = 0;
        p.traveled = 0;
      } else {
        dead = true;  // range exhausted (on the return leg too)
      }
    }
    if (dead) eraseEntry(id);
  }
}

}  // namespace litholattice
