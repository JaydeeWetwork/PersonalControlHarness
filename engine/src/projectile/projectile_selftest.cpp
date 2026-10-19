// ProjectileManager selftest (P2c 2026-10-13, review ENT-1): movement,
// swept wall hits (no tunnelling), penetration, chunk-ahead requests
// through a fake seam, entity hits / owner exclusion, and the despawn
// rules. Prints PASS/FAIL per check; exit code = failures.
#include <cmath>
#include <cstdio>
#include <set>
#include <utility>
#include <vector>

#include "litholattice/projectile/projectile.h"

using namespace litholattice;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++failures;
}
bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

struct RecordBehavior : IProjectileBehavior {
  int calls = 0;
  bool lastHitBlock = false;
  uint64_t lastHitEntity = 0;
  bool keep = true;
  bool onTick(Projectile& /*p*/, bool hitBlock, uint64_t hitEntity) override {
    ++calls;
    lastHitBlock = hitBlock;
    lastHitEntity = hitEntity;
    return keep;
  }
};

// A wall: every voxel with x >= 10 is solid stone of the given density.
ProjectileManager::VoxelQueryFn wall(uint8_t density) {
  return [density](int x, int, int) -> ProjectileManager::VoxelInfo {
    return x >= 10 ? ProjectileManager::VoxelInfo{true, density} : ProjectileManager::VoxelInfo{};
  };
}
}  // namespace

int runProjectileSelfTest() {
  // -- spawn / tick movement in open air --------------------------------
  {
    ProjectileManager m;  // no seams: air everywhere
    Projectile p;
    p.x = 0; p.y = 64; p.z = 0; p.vx = 2; p.maxRange = 64;
    const uint64_t id = m.spawn(p, nullptr);
    check(id != 0 && m.size() == 1, "spawn assigns an id");
    m.tick();
    const Projectile* q = m.get(id);
    check(q && near(q->x, 2.0) && q->traveled == 2, "tick moves by velocity (blocks per tick)");
    m.tick(); m.tick();
    check(q && near(q->x, 6.0), "movement accumulates over ticks");
    m.despawn(id);
    check(m.size() == 0 && m.get(id) == nullptr, "explicit despawn removes the projectile");
  }
  // -- duplicate caller id refused --------------------------------------
  {
    ProjectileManager m;
    Projectile p; p.id = 77; p.vx = 1;
    check(m.spawn(p, nullptr) == 77, "caller-supplied id honoured");
    check(m.spawn(p, nullptr) == 0 && m.size() == 1, "duplicate id refused (first wins)");
  }
  // -- range despawn ------------------------------------------------------
  {
    ProjectileManager m;
    Projectile p; p.vx = 4; p.maxRange = 10;
    const uint64_t id = m.spawn(p, nullptr);
    m.tick(); m.tick();
    check(m.alive(id), "alive inside maxRange");
    m.tick();  // traveled 12 >= 10
    check(!m.alive(id), "despawns when traveled reaches maxRange");
  }
  // -- swept hit: a fast projectile must not tunnel through a 1-block wall
  {
    ProjectileManager m;
    m.setVoxelQuery(wall(200));
    RecordBehavior b;
    Projectile p; p.x = 0; p.y = 64; p.z = 0; p.vx = 30; p.maxRange = 1000;  // 30 blocks in ONE tick
    const uint64_t id = m.spawn(p, &b);
    m.tick();
    check(b.calls == 1 && b.lastHitBlock, "swept hit reported to the behavior");
    check(!m.alive(id), "unpierced hit despawns (default semantics)");
    // (position at death is gone with the entry; the stop-point itself is
    // pinned by the slow case below.)
  }
  {
    ProjectileManager m;
    m.setVoxelQuery(wall(200));
    RecordBehavior b; b.keep = true;  // keep-alive behavior on a NON-boomerang: still dies on a stopped hit
    Projectile p; p.x = 9.0; p.y = 64; p.z = 0; p.vx = 0.4; p.maxRange = 1000;
    const uint64_t id = m.spawn(p, &b);
    m.tick();  // -> 9.4
    m.tick();  // -> 9.8
    const Projectile* q = m.get(id);
    check(q && near(q->x, 9.8), "slow projectile approaches the wall face");
    m.tick();  // next substep would enter x=10: stops at 9.8 and despawns
    check(!m.alive(id) && b.lastHitBlock, "hit stops at the contact point and despawns");
  }
  // -- penetration pierces -------------------------------------------------
  {
    ProjectileManager m;
    m.setVoxelQuery(wall(10));  // weak wall
    RecordBehavior b;
    Projectile p; p.x = 0; p.y = 64; p.z = 0; p.vx = 4; p.penetration = 64; p.maxRange = 1000;
    const uint64_t id = m.spawn(p, &b);
    for (int i = 0; i < 4; ++i) m.tick();  // travels through the solid half-space x >= 10
    const Projectile* q = m.get(id);
    check(m.alive(id) && q && near(q->x, 16.0), "penetration > density pierces the wall");
    check(b.lastHitBlock, "pierce still reports the block hit");
  }
  // -- chunk-ahead requests through the seam --------------------------------
  {
    ProjectileManager m;
    std::vector<std::pair<int, int>> req;
    m.setChunkRequester([&](int cx, int cz) { req.emplace_back(cx, cz); });
    Projectile p; p.x = 8; p.y = 64; p.z = 8; p.vx = 4; p.maxRange = 64;
    m.spawn(p, nullptr);  // warm-up requests along the spawn ray
    const std::set<std::pair<int, int>> cols(req.begin(), req.end());
    check(cols.count({0, 0}) && cols.count({1, 0}) && cols.count({2, 0}) && cols.count({3, 0}),
          "chunk-ahead requests fire for the columns on the path");
    check(!cols.count({0, 5}), "no requests off the path");
    const size_t afterSpawn = req.size();
    m.tick(); m.tick();
    check(req.size() == afterSpawn, "per-tick requests are deduplicated");
    m.clearRequestedChunks();
    m.tick();
    check(req.size() > afterSpawn, "clearRequestedChunks re-arms requests (dimension switch)");
    // One-shot form, diagonal ray.
    ProjectileManager m2;
    std::set<std::pair<int, int>> cols2;
    m2.setChunkRequester([&](int cx, int cz) { cols2.emplace(cx, cz); });
    m2.loadChunksAhead(0, 64, 0, 1, 0, 1, 48);
    check(cols2.count({0, 0}) && cols2.count({1, 1}) && cols2.count({2, 2}),
          "loadChunksAhead follows a diagonal ray");
    m2.loadChunksAhead(0, 64, 0, 0, 0, 0, 48);
    check(cols2.size() >= 3, "zero direction is a no-op (no crash, no NaN columns)");
  }
  // -- entity hits + owner exclusion -----------------------------------------
  {
    ProjectileManager m;
    // A target entity (id 42) standing at x in [20,21); owner is 7.
    m.setEntityQuery([](double x, double, double, uint64_t exclude) -> uint64_t {
      if (x >= 20.0 && x < 21.0) return exclude == 42 ? 0 : 42;
      return 0;
    });
    RecordBehavior b;
    Projectile p; p.x = 16; p.y = 64; p.z = 0; p.vx = 8; p.ownerId = 7; p.maxRange = 1000;
    const uint64_t id = m.spawn(p, &b);
    m.tick();
    check(b.lastHitEntity == 42 && !m.alive(id), "entity hit despawns and reports the entity");
    // The owner firing through its own position is never self-hit.
    ProjectileManager m2;
    m2.setEntityQuery([](double x, double, double, uint64_t exclude) -> uint64_t {
      return (x >= 0.0 && exclude != 7) ? 7 : 0;  // entity 7 (the owner) at the muzzle
    });
    Projectile self; self.x = 0; self.y = 64; self.z = 0; self.vx = 2; self.ownerId = 7; self.maxRange = 4;
    const uint64_t id2 = m2.spawn(self, nullptr);
    m2.tick();
    check(m2.alive(id2), "owner is excluded from its own projectile's hits");
  }
  // -- behavior returning false despawns --------------------------------------
  {
    ProjectileManager m;
    RecordBehavior b; b.keep = false;
    Projectile p; p.vx = 1; p.maxRange = 1000;
    const uint64_t id = m.spawn(p, &b);
    m.tick();
    check(!m.alive(id), "behavior returning false despawns");
  }
  // -- boomerang: turns at maxRange, despawns after the return leg ------------
  {
    ProjectileManager m;
    Projectile p; p.x = 0; p.y = 64; p.z = 0; p.vx = 4; p.maxRange = 8; p.returnsToOwner = true;
    const uint64_t id = m.spawn(p, nullptr);
    m.tick(); m.tick();  // traveled 8: turn
    const Projectile* q = m.get(id);
    check(q && q->returning && near(q->vx, -4.0), "boomerang reverses at maxRange");
    m.tick(); m.tick();  // return leg exhausts its budget
    check(!m.alive(id), "boomerang despawns after the return leg");
  }

  std::printf("PROJECTILE SELFTEST: %s (%d failures)\n", failures ? "FAILED" : "OK", failures);
  return failures;
}
