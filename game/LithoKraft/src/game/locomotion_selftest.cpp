// Locomotion self-test (app selftest section): proves the game-side wiring
// of the engine's ILocomotion without a window —
//   * the player body falls under gravity and lands on a floor (onGround,
//     resting exactly at the floor's top);
//   * walking into a wall stops at halfWidth from it, never penetrating;
//   * a 1-block tunnel forces the Crawl pose + crawl eye height with NO
//     crawl input (pure geometry), and the body emerges standing;
//   * Fly mode rises on Jump and ignores gravity (no sag with no input);
//   * the player adapter mirrors the body's TRUE velocity after a walk
//     tick, and reports zero on a teleport tick (the guard survives the
//     true-velocity handoff);
//   * the Crawl action exists in the actions system's own pattern (enum,
//     info table, Gameplay context, default bindings).
// Everything is driven with a fixed dt (1/32 s, the default tick rate)
// against a hand-built ChunkMap: deterministic, no threads, no RNG.
#include "lithokraft/game/player_locomotion.h"

#include <cmath>
#include <cstdio>
#include <memory>

#include "litholattice/world/chunk.h"
#include "litholattice/world/chunk_map.h"
#include "lithokraft/content/blocks.h"
#include "lithokraft/game/actions.h"
#include "lithokraft/game/player.h"
#include "lithokraft/game/player_adapter.h"

namespace lithokraft {
namespace {

using namespace litholattice;

constexpr double kDt = 1.0 / 32.0;  // one sim tick at the default rate
constexpr double kFloorTop = 64.0;  // floor blocks sit at y=63

/// A 32x32 test plot: solid floor at y=63, a wall at x=10 (z 8..15), and a
/// 1-block-high tunnel (ceiling at y=65) over x 18..25, z 20..27.
ChunkMap makeTestWorld() {
  ChunkMap map;
  map.setRegistry(&blockRegistry());
  for (int cx = 0; cx < 2; ++cx)
    for (int cz = 0; cz < 2; ++cz) {
      auto c = std::make_unique<ChunkColumn>();
      c->cx = cx;
      c->cz = cz;
      map.insert(std::move(c));
    }
  for (int x = 0; x < 32; ++x)
    for (int z = 0; z < 32; ++z) map.set(x, 63, z, kWaferSilicon);
  for (int y = 64; y < 68; ++y)
    for (int z = 8; z < 16; ++z) map.set(10, y, z, kWaferSilicon);
  for (int x = 18; x < 26; ++x)
    for (int z = 20; z < 28; ++z) map.set(x, 65, z, kWaferSilicon);
  return map;
}

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

}  // namespace

int runLocomotionSelfTest() {
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
  };

  ChunkMap map = makeTestWorld();
  const auto isSolid = [&map](int x, int y, int z) {
    return blockRegistry().has(map.get(x, y, z), kSolid);
  };
  LocomotionInput none{};

  // --- Fall + land -------------------------------------------------------
  {
    PlayerLocomotion loco;
    loco.place({4.5, 70.0, 4.5});
    loco.tick(none, isSolid, kDt);
    check(!loco.onGround() && loco.body().y < 70.0,
          "locomotion: the body falls under gravity (airborne, descending)");
    for (int i = 0; i < 240 && !loco.onGround(); ++i) loco.tick(none, isSolid, kDt);
    check(loco.onGround(), "locomotion: the body lands (onGround)");
    check(near(loco.body().y, kFloorTop, 1e-9) && near(loco.body().vy, 0.0, 1e-9),
          "locomotion: the body rests exactly at floor height with zero fall speed");
    for (int i = 0; i < 32; ++i) loco.tick(none, isSolid, kDt);
    check(loco.onGround() && near(loco.body().y, kFloorTop, 1e-9),
          "locomotion: the landed body stays put (no sinking, no jitter)");
  }

  // --- Wall stop ---------------------------------------------------------
  {
    PlayerLocomotion loco;
    loco.place({6.5, kFloorTop, 12.0});
    LocomotionInput walk{};
    walk.wishX = 1.0;
    double maxX = 0.0;
    for (int i = 0; i < 240; ++i) {
      loco.tick(walk, isSolid, kDt);
      if (loco.body().x > maxX) maxX = loco.body().x;
    }
    // Wall face at x=10, body halfWidth 0.3 (contract default).
    check(near(loco.body().x, 10.0 - 0.3, 0.011) && maxX <= 10.0 - 0.3 + 1e-9,
          "locomotion: walking into a wall stops at halfWidth from it, never inside");
    check(loco.onGround() && near(loco.body().y, kFloorTop, 1e-9),
          "locomotion: the wall-stopped body stays grounded at floor height");
  }

  // --- Forced crawl through a 1-block tunnel (no crawl input) ------------
  {
    PlayerLocomotion loco;
    loco.place({14.5, kFloorTop, 24.0});
    LocomotionInput walk{};
    walk.wishX = 1.0;  // crawl input deliberately never set: geometry forces it
    bool sawCrawl = false, crawlEyeOk = false;
    for (int i = 0; i < 900 && loco.body().x < 28.0; ++i) {
      loco.tick(walk, isSolid, kDt);
      if (loco.body().x > 19.0 && loco.body().x < 25.0) {
        if (loco.pose() == LocomotionPose::Crawl) sawCrawl = true;
        if (near(loco.eyeHeight(), 0.4f, 1e-6f)) crawlEyeOk = true;
      }
    }
    check(loco.body().x >= 28.0, "locomotion: the body traverses the 1-block tunnel");
    check(sawCrawl, "locomotion: the tunnel forces the Crawl pose (zero crawl input)");
    check(crawlEyeOk, "locomotion: the eye height inside the tunnel is the crawl height (0.4)");
    check(loco.pose() == LocomotionPose::Stand && near(loco.eyeHeight(), 1.62f, 1e-6f),
          "locomotion: the body emerges from the tunnel standing (eye 1.62)");
  }

  // --- Deliberate crawl request ------------------------------------------
  {
    PlayerLocomotion loco;
    loco.place({28.5, kFloorTop, 28.5});
    LocomotionInput crawl{};
    crawl.crawl = true;
    for (int i = 0; i < 8; ++i) loco.tick(crawl, isSolid, kDt);
    check(loco.pose() == LocomotionPose::Crawl && near(loco.eyeHeight(), 0.4f, 1e-6f),
          "locomotion: a deliberate crawl request drops the body to the Crawl pose");
  }

  // --- Fly mode ------------------------------------------------------------
  {
    PlayerLocomotion loco;
    loco.setMode(LocomotionMode::Fly);
    loco.place({28.5, kFloorTop, 4.5});
    LocomotionInput rise{};
    rise.jump = true;
    for (int i = 0; i < 32; ++i) loco.tick(rise, isSolid, kDt);
    check(loco.body().y > kFloorTop + 4.0 && loco.body().vy > 10.0,
          "locomotion: fly mode rises on Jump at fly vertical speed");
    const double yAtHover = loco.body().y;
    for (int i = 0; i < 64; ++i) loco.tick(none, isSolid, kDt);
    check(near(loco.body().y, yAtHover, 1e-9),
          "locomotion: fly mode ignores gravity (no sag with no input)");
    LocomotionInput sink{};
    sink.sneak = true;
    for (int i = 0; i < 32; ++i) loco.tick(sink, isSolid, kDt);
    check(loco.body().y < yAtHover - 4.0, "locomotion: fly mode descends on Sneak");
  }

  // --- Adapter: true velocity + teleport guard -----------------------------
  {
    Player p;
    PlayerLocomotion loco;
    loco.place({4.5, kFloorTop, 28.5});
    p.pos = loco.position();
    GamePlayerAdapter adapter(p);  // ctor syncs once (baseline)
    LocomotionInput walk{};
    walk.wishX = 1.0;
    for (int i = 0; i < 48; ++i) loco.tick(walk, isSolid, kDt);
    p.pos = loco.position();
    adapter.syncFromGame(1, 1.f / 60.f, loco.velocity());
    const PlayerState& st = adapter.enginePlayer().state();
    check(near(st.vx, loco.body().vx, 1e-6) && near(st.vy, loco.body().vy, 1e-6) &&
              near(st.vz, loco.body().vz, 1e-6) && st.vx > 3.0,
          "locomotion: the adapter mirrors the body's TRUE velocity after a walk tick");
    check(near(st.x, loco.body().x, 1e-6) && near(st.y, loco.body().y, 1e-6),
          "locomotion: the adapter position follows the body");
    // Teleport tick: the game position jumps far; even with a live body
    // velocity offered, the guard reports zero for this one sync.
    p.pos = {200.f, float(kFloorTop), 28.5f};
    adapter.syncFromGame(1, 1.f / 60.f, loco.velocity());
    const PlayerState& tp = adapter.enginePlayer().state();
    check(tp.vx == 0.0 && tp.vy == 0.0 && tp.vz == 0.0 && near(tp.x, 200.0, 1e-9),
          "locomotion: a teleport tick reports zero velocity once (position still mirrors)");
    // The legacy derived-velocity sync (init-time path) still derives.
    Player p2;
    p2.pos = {0.f, float(kFloorTop), 0.f};
    GamePlayerAdapter adapter2(p2);
    p2.pos.x += 0.05f;
    adapter2.syncFromGame(1, 1.f / 60.f);
    check(near(adapter2.enginePlayer().state().vx, 3.0, 0.05),
          "locomotion: the adapter's legacy derived-velocity sync is unchanged");
  }

  // --- Vitals TPS normalisation (P2b-2, review PLY-1) ----------------------
  {
    // Equal WALL-CLOCK time underwater must drain equal oxygen at any
    // TPS: 16 s at 32 TPS = 512 world ticks; 16 s at 512 TPS = 8192.
    // Pre-fix, tickVitals counted raw ticks, so the 512 TPS player
    // drained 16x faster (and drowned 16x sooner).
    Player pA, pB;
    GamePlayerAdapter a(pA), b(pB);
    a.tickVitals(512, true, false, 32);
    b.tickVitals(8192, true, false, 512);
    check(a.oxygen() == b.oxygen(),
          "vitals (PLY-1): equal wall-clock oxygen drain at 32 vs 512 TPS");
    // The 32 TPS calibration is bit-exact: 64 points at 1 per 8 ticks =
    // the full bar in exactly 512 ticks (~16 s of air, as documented).
    check(a.oxygen() == 0,
          "vitals (PLY-1): 32 TPS calibration unchanged (full 64-point bar drains in 512 ticks)");
    // Refill is normalised too: 4 s surfaced refills the same at both rates.
    Player pC, pD;
    GamePlayerAdapter c(pC), d(pD);
    c.tickVitals(512, true, false, 32);   // empty the bar at 32 TPS
    d.tickVitals(8192, true, false, 512); // empty the bar at 512 TPS
    c.tickVitals(128, false, false, 32);  // 4 s surfaced
    d.tickVitals(2048, false, false, 512);
    check(c.oxygen() == d.oxygen() && c.oxygen() > 0,
          "vitals (PLY-1): equal wall-clock oxygen refill at 32 vs 512 TPS");
    // Many small calls (the per-frame pattern) carry the fractional
    // remainder exactly: 8192 x 1 tick at 512 TPS == one 8192-tick call.
    Player pE;
    GamePlayerAdapter e(pE);
    for (int i = 0; i < 8192; ++i) e.tickVitals(1, true, false, 512);
    check(e.oxygen() == b.oxygen(),
          "vitals (PLY-1): per-tick calls at 512 TPS accumulate to the same drain (no fractional loss)");
  }

  // --- Crawl action (the actions system's own pattern) ---------------------
  {
    const ActionMap map2 = defaultActionMap();
    check(!map2.bindings(Action::Crawl).empty(),
          "locomotion: the Crawl action has default bindings (C / DpadRight)");
    check(actionName(Action::Crawl) == "Crawl",
          "locomotion: the Crawl action is in the action info table");
    bool inGameplay = false;
    for (const ContextRules& r : inputContexts())
      if (r.name == "Gameplay")
        for (const ActionId id : r.actions)
          if (id == actionId(Action::Crawl)) inGameplay = true;
    check(inGameplay, "locomotion: the Crawl action is in the Gameplay input context");
  }

  return fails;
}

}  // namespace lithokraft
