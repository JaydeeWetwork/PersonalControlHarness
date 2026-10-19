// ILocomotion self-tests (ctest: engine_locomotion). Exit code = failure count.
//
// Synthetic isSolid worlds only — floor planes, a wall, a cliff, a 1x1
// tunnel, low ceilings — no game code. Tolerances: the sweep clamps
// positions to voxel boundaries (plus a 1e-4 epsilon in physics), so
// resting positions are asserted to 1e-6..1e-2 as noted per check; speeds
// come from an exponential approach whose residual after the settle times
// used here is far below the 2% bands asserted.
#include <cmath>
#include <cstdio>
#include <functional>

#include "litholattice/entity/locomotion.h"

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fails;
}

using namespace litholattice;
using Solid = std::function<bool(int, int, int)>;

const Solid kFloor = [](int, int y, int) { return y < 0; };  // top surface y=0

void run(LocomotionController& c, Entity& e, const LocomotionInput& in, const Solid& s,
         double dt, int steps) {
  for (int i = 0; i < steps; ++i) c.step(e, in, s, dt);
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

}  // namespace

int main() {
  const double kDt = 1.0 / 60.0;
  LocomotionInput none{};

  // -- Accessors + input-driven poses in the open ---------------------------
  {
    LocomotionController c;
    Entity e;
    e.x = 0; e.y = 0; e.z = 0;
    check(c.mode() == LocomotionMode::Walk && c.pose() == LocomotionPose::Stand,
          "locomotion: defaults to Walk/Stand");
    check(near(c.currentHeight(), 1.8, 1e-12) && near(c.eyeHeight(), 1.62, 1e-12) &&
              near(c.halfWidth(), 0.3, 1e-12),
          "locomotion: stand height/eye/halfWidth = 1.8/1.62/0.3");
    LocomotionInput sneak{}; sneak.sneak = true;
    run(c, e, sneak, kFloor, kDt, 2);
    check(c.pose() == LocomotionPose::Sneak && near(c.currentHeight(), 1.5, 1e-12) &&
              near(c.eyeHeight(), 1.27, 1e-12),
          "locomotion: sneak pose = 1.5 / eye 1.27");
    LocomotionInput crawl{}; crawl.crawl = true;
    run(c, e, crawl, kFloor, kDt, 2);
    check(c.pose() == LocomotionPose::Crawl && near(c.currentHeight(), 0.6, 1e-12) &&
              near(c.eyeHeight(), 0.4, 1e-12),
          "locomotion: crawl pose = 0.6 / eye 0.4");
    run(c, e, none, kFloor, kDt, 2);
    check(c.pose() == LocomotionPose::Stand, "locomotion: releasing inputs returns to Stand");
    c.setMode(LocomotionMode::Fly);
    check(c.mode() == LocomotionMode::Fly, "locomotion: setMode/mode round-trip");
  }

  // -- Free fall: gravity, terminal velocity, no tunneling at coarse dt -----
  {
    LocomotionController c;
    Entity e;
    e.x = 0; e.y = 80; e.z = 0;
    double minVy = 0;
    double vyAt2s = 0, yAt2s = 0;
    for (int i = 1; i <= 120; ++i) {  // dt = 1/20: hostile step, 3 blocks/step at terminal
      c.step(e, none, kFloor, 1.0 / 20.0);
      minVy = std::min(minVy, e.vy);
      if (i == 40) { vyAt2s = e.vy; yAt2s = e.y; }
    }
    check(minVy >= -60.0 - 1e-9, "locomotion: fall speed never exceeds terminal velocity");
    check(near(vyAt2s, -60.0, 1e-9) && yAt2s > 5.0,
          "locomotion: terminal velocity reached mid-fall");
    check(near(e.y, 0.0, 1e-6) && e.onGround && e.vy == 0.0,
          "locomotion: fall lands on the floor (no tunneling), grounded, vy zeroed");
  }

  // -- Sustained speeds: walk / sprint / sneak / crawl / wish clamp ---------
  {
    auto speedAfter = [&](const LocomotionInput& in, double seconds) {
      LocomotionController c;
      Entity e;
      e.x = 0; e.y = 0; e.z = 0;
      run(c, e, in, kFloor, kDt, 5);  // settle onto the floor
      run(c, e, in, kFloor, kDt, int(seconds / kDt));
      return std::hypot(e.vx, e.vz);
    };
    LocomotionInput walk{}; walk.wishX = 1;
    check(near(speedAfter(walk, 2.0), 4.317, 0.09), "locomotion: walk speed ~= 4.317");
    LocomotionInput sprint = walk; sprint.sprint = true;
    check(near(speedAfter(sprint, 2.0), 4.317 * 1.3, 0.11),
          "locomotion: sprint speed ~= walk * 1.3");
    LocomotionInput sneak = walk; sneak.sneak = true;
    check(near(speedAfter(sneak, 2.0), 1.31, 0.03), "locomotion: sneak speed ~= 1.31");
    LocomotionInput crawl = walk; crawl.crawl = true;
    check(near(speedAfter(crawl, 2.0), 1.31, 0.03), "locomotion: crawl speed ~= 1.31");
    LocomotionInput big{}; big.wishX = 3; big.wishZ = 4;  // magnitude 5 -> clamped to 1
    check(near(speedAfter(big, 2.0), 4.317, 0.09),
          "locomotion: over-length wish direction clamps to walk speed");
  }

  // -- Wall slide ------------------------------------------------------------
  {
    const Solid wall = [](int x, int y, int) { return y < 0 || (x >= 5 && y <= 3); };
    LocomotionController c;
    Entity e;
    e.x = 3; e.y = 0; e.z = 0;
    LocomotionInput diag{};
    diag.wishX = diag.wishZ = 0.7071067811865476;  // 45 degrees, magnitude 1
    run(c, e, diag, wall, kDt, 120);
    check(near(e.x, 4.6999, 1e-3), "locomotion: wall clamps x at the face");
    check(e.z > 3.0, "locomotion: diagonal walk slides along the wall (z advances)");
    check(e.onGround && near(e.y, 0.0, 1e-6), "locomotion: wall slide stays grounded");
  }

  // -- Jump: apex ~= jumpHeight, returns to ground ---------------------------
  {
    LocomotionController c;
    Entity e;
    e.x = 0; e.y = 0; e.z = 0;
    run(c, e, none, kFloor, 1.0 / 240.0, 30);  // settle (fine dt: apex is an integral)
    LocomotionInput jump{}; jump.jump = true;
    c.step(e, jump, kFloor, 1.0 / 240.0);      // one held step is enough to leave the ground
    double apex = e.y;
    for (int i = 0; i < 480 && !e.onGround; ++i) {
      c.step(e, none, kFloor, 1.0 / 240.0);
      apex = std::max(apex, e.y);
    }
    check(apex >= 1.125 && apex <= 1.375, "locomotion: jump apex ~= 1.25 blocks (+/-10%)");
    check(e.onGround && near(e.y, 0.0, 1e-6), "locomotion: jump returns to the ground");
  }

  // -- Sneak edge-guard vs walking off ---------------------------------------
  {
    const Solid cliff = [](int x, int y, int) { return y < 0 && x < 4; };
    {  // sneaking: must stop at the edge and stay supported
      LocomotionController c;
      Entity e;
      e.x = 1; e.y = 0; e.z = 0;
      LocomotionInput in{}; in.wishX = 1; in.sneak = true;
      double minY = 0;
      for (int i = 0; i < 600; ++i) {
        c.step(e, in, cliff, kDt);
        minY = std::min(minY, e.y);
      }
      LocomotionInput hold{}; hold.sneak = true;  // settle: no wish, keep sneaking
      run(c, e, hold, cliff, kDt, 15);
      check(e.onGround && near(e.y, 0.0, 0.01),
            "locomotion: sneak edge-guard keeps the body on the platform");
      check(e.x > 3.9 && e.x < 4.36, "locomotion: sneak stops at the edge lip");
      check(minY > -0.05, "locomotion: sneak never dips into a fall at the edge");
    }
    {  // walking (no sneak): the same edge is a fall
      LocomotionController c;
      Entity e;
      e.x = 1; e.y = 0; e.z = 0;
      LocomotionInput in{}; in.wishX = 1;
      run(c, e, in, cliff, kDt, 180);
      check(!e.onGround && e.y < -2.0 && e.x > 4.5,
            "locomotion: walking off the same edge falls normally");
    }
    {  // sneak step-down of one block: support within 1.05 -> legal, no restore
      const Solid stepDown = [](int x, int y, int) {
        return (x < 4 && y < 0) || (x >= 4 && y < -1);  // floor drops 1 block at x=4
      };
      LocomotionController c;
      Entity e;
      e.x = 1; e.y = 0; e.z = 0;
      LocomotionInput in{}; in.wishX = 1; in.sneak = true;
      run(c, e, in, stepDown, kDt, 600);
      check(e.onGround && near(e.y, -1.0, 1e-6) && e.x > 5.0,
            "locomotion: sneak steps down a 1-block drop (support within 1.05)");
    }
  }

  // -- 1x1 tunnel: forced crawl, stand denied, stand resumes -----------------
  {
    const Solid tunnel = [](int x, int y, int z) {
      if (y < 0) return true;
      return y >= 1 && x >= 3 && x <= 9 && z >= -1 && z <= 1;  // ceiling slab: 1-block gap
    };
    LocomotionController c;
    Entity e;
    e.x = 0; e.y = 0; e.z = 0;
    LocomotionInput walk{}; walk.wishX = 1;  // no sneak/crawl held: Stand is desired throughout
    bool crawlInTunnel = false, flatInTunnel = true;
    for (int i = 0; i < 1500; ++i) {
      c.step(e, walk, tunnel, kDt);
      if (e.x > 4.5 && e.x < 8.5) {
        if (c.pose() == LocomotionPose::Crawl && near(c.currentHeight(), 0.6, 1e-12) &&
            near(c.eyeHeight(), 0.4, 1e-12))
          crawlInTunnel = true;
        if (!near(e.y, 0.0, 1e-6)) flatInTunnel = false;
      }
    }
    check(crawlInTunnel, "locomotion: 1-block tunnel forces Crawl (0.6 / eye 0.4), stand denied");
    check(flatInTunnel, "locomotion: crawling body stays on the tunnel floor");
    check(e.x > 11.0 && c.pose() == LocomotionPose::Stand,
          "locomotion: Stand resumes after exiting the tunnel");
  }

  // -- Low ceiling caps the pose at Sneak ------------------------------------
  // A true 1.5-block gap at integer feet is not voxel-representable (unit
  // voxels make gaps of 1 or 2), so this exercises pose resolution at a
  // raised feet position in Fly hover (pose logic is mode-independent):
  // feet at y=0.3 under a ceiling whose underside is y=2 leaves 1.7 blocks —
  // Stand (1.8) does not fit, Sneak (1.5) does.
  {
    const Solid lowCeil = [](int x, int y, int z) {
      if (y < 0) return true;
      return y >= 2 && x >= -2 && x <= 2 && z >= -2 && z <= 2;
    };
    LocomotionController c;
    c.setMode(LocomotionMode::Fly);
    Entity e;
    e.x = 0; e.y = 0.3; e.z = 0;
    run(c, e, none, lowCeil, kDt, 5);
    check(c.pose() == LocomotionPose::Sneak && near(c.currentHeight(), 1.5, 1e-12) &&
              near(c.eyeHeight(), 1.27, 1e-12),
          "locomotion: 1.7-block gap caps the pose at Sneak");
    check(near(e.y, 0.3, 1e-12), "locomotion: fly hover holds altitude under the ceiling");
  }

  // -- Fly: rise / hover / descend / ceiling ---------------------------------
  {
    LocomotionController c;
    c.setMode(LocomotionMode::Fly);
    Entity e;
    e.x = 0; e.y = 0; e.z = 0;
    run(c, e, none, kFloor, kDt, 120);
    check(e.y == 0.0 && e.vy == 0.0, "locomotion: fly hovers with no gravity sag");
    LocomotionInput up{}; up.jump = true;
    bool vyOk = true;
    for (int i = 0; i < 30; ++i) {
      c.step(e, up, kFloor, kDt);
      if (!near(e.vy, 10.9, 1e-9)) vyOk = false;
    }
    check(vyOk && e.y > 5.3 && e.y < 5.6, "locomotion: fly rises at flyVerticalSpeed under jump");
    LocomotionInput down{}; down.sneak = true;
    const double yBefore = e.y;
    run(c, e, down, kFloor, kDt, 30);
    check(near(e.vy, -10.9, 1e-9) && yBefore - e.y > 5.3,
          "locomotion: fly descends at flyVerticalSpeed under sneak");
    check(c.pose() == LocomotionPose::Sneak,
          "locomotion: fly sneak input also requests Sneak pose (documented coupling)");
  }
  {
    const Solid ceil = [](int, int y, int) { return y >= 3; };
    LocomotionController c;
    c.setMode(LocomotionMode::Fly);
    Entity e;
    e.x = 0; e.y = 1; e.z = 0;
    LocomotionInput up{}; up.jump = true;
    double maxY = e.y;
    for (int i = 0; i < 120; ++i) {
      c.step(e, up, ceil, kDt);
      maxY = std::max(maxY, e.y);
    }
    check(near(e.y, 1.1999, 1e-3) && maxY <= 1.2005,
          "locomotion: fly is stopped by a ceiling (collides, no pass-through)");
  }

  // -- Determinism: identical inputs replay identical motion ------------------
  {
    auto script = [kDt] {
      const Solid wall = [](int x, int y, int) { return y < 0 || (x >= 5 && y <= 3); };
      LocomotionController c;
      Entity e;
      e.x = 0; e.y = 3; e.z = 0;
      LocomotionInput in{}; in.wishX = 0.8; in.wishZ = 0.6;
      run(c, e, in, wall, kDt, 90);
      in.jump = true;
      c.step(e, in, wall, kDt);
      in.jump = false; in.sprint = true;
      run(c, e, in, wall, kDt, 200);
      return e;
    };
    const Entity a = script(), b = script();
    check(a.x == b.x && a.y == b.y && a.z == b.z && a.vx == b.vx && a.vy == b.vy && a.vz == b.vz,
          "locomotion: same inputs replay bit-identical motion");
  }

  std::printf("locomotion selftest: %s (%d failure%s)\n", g_fails ? "FAILED" : "OK", g_fails,
              g_fails == 1 ? "" : "s");
  return g_fails;
}
