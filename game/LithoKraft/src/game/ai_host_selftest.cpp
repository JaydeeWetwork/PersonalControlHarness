// AiHost smoke self-test (app selftest section): proves the production
// construction path for AiWorld (brief B6) without a window:
//   * ai.enabled cvar registered, default off;
//   * a bound-but-disabled host constructs NOTHING and advance() is a
//     no-op (default behaviour is safe/unchanged);
//   * enabling constructs the AiWorld over the bound live map through
//     the host, with the engine's clamped AI rate;
//   * advance(simTicks, worldTps) drives AI ticks at that clamped rate
//     (never per sim tick), and a mob spawned through the host's world
//     paths across the fixture floor and moves;
//   * disabling destroys the world again; unbinding while enabled too.
#include "lithokraft/game/ai_host.h"

#include <cstdio>
#include <memory>

#include "litholattice/console/concmd.h"
#include "lithokraft/content/blocks.h"  // SimRole, kGameClimbable, kGameFluid (P0 fixtures)
#include "lithokraft/game/settings_cvars.h"

namespace lithokraft {

int runAiHostSelfTest() {
  using namespace litholattice;
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
  };

  {  // The gate: ai.enabled exists, Bool, default "0" (off), archived.
    console::ConsoleRegistry reg;
    registerAiCvars(reg);
    const console::CVar* cv = reg.findCVar("ai.enabled");
    check(cv && cv->value == "0", "ai host: ai.enabled cvar registered, default 0 (off)");
  }

  // Fixture live map: 4x4 columns (x,z in 0..63), solid floor at y=9,
  // walkable at y=10 — the engine AI tests' floor arrangement.
  BlockRegistry blocks;
  const BlockId rock = blocks.add({"Rock", kSolid | kOpaque, Shape::Cube, "rock"});
  ChunkMap w;
  w.setRegistry(&blocks);
  for (int cz = 0; cz < 4; ++cz)
    for (int cx = 0; cx < 4; ++cx) {
      auto c = std::make_unique<ChunkColumn>();
      c->cx = cx;
      c->cz = cz;
      w.insert(std::move(c));
    }
  for (int z = 0; z < 64; ++z)
    for (int x = 0; x < 64; ++x) w.set(x, 9, z, rock);

  AiHost host;
  host.bindWorld(w, blocks, nullptr, {2, 10, 2});
  check(!host.enabled() && !host.constructed() && host.world() == nullptr,
        "ai host: bound but disabled constructs no AiWorld (default is safe)");
  host.advance(32, 32);
  check(!host.constructed(), "ai host: advance() while disabled is a no-op (no construction, no effect)");

  host.setEnabled(true);
  check(host.enabled() && host.constructed() && host.world() != nullptr && host.nav() != nullptr,
        "ai host: enabling constructs the AiWorld + WalkCache over the live map");
  check(host.world() && host.world()->config().hz >= 8 && host.world()->config().hz <= 16,
        "ai host: AiWorld rate is the engine-clamped 8..16 Hz");

  // Spawn + goal through the production object; advance drives it.
  auto walk = ai::makeWalk();
  const auto id = host.world()->spawn({2, 10, 2}, walk.get());
  check(id.has_value(), "ai host: spawn through the production AiWorld works");
  host.world()->setGoal(*id, {10, 10, 2});
  host.setObserver({2, 10, 2});
  host.advance(32, 32);  // 1 s of world time at 32 TPS
  const uint64_t ticks1 = host.world()->stats().ticks;
  check(ticks1 == 10, "ai host: advance(32 ticks, 32 TPS) ran 10 AI ticks at 10 Hz (not 32)");
  const ai::Cell pos1 = host.world()->mob(*id)->pos;
  check(pos1.x > 2 && pos1.y == 10, "ai host: the spawned mob pathed and moved toward its goal");
  host.advance(512, 512);  // 1 s at 512 TPS: same AI rate, never 512 Hz
  check(host.world()->stats().ticks == ticks1 + 10,
        "ai host: advance(512 ticks, 512 TPS) added 10 AI ticks (rate independent of world TPS)");

  host.setEnabled(false);
  check(!host.constructed() && host.world() == nullptr, "ai host: disabling destroys the AiWorld");
  host.setEnabled(true);
  check(host.constructed() && host.world()->stats().mobs == 0,
        "ai host: re-enabling constructs a fresh, empty AiWorld (no stale state)");
  host.unbindWorld();
  check(!host.constructed() && host.world() == nullptr, "ai host: unbind while enabled destroys the AiWorld");

  // ---- P0 (2026-10-14): real NavRules + the edit-invalidation feed ----
  {
    // Fixture registry exercising every branch of gameNavRules: a track
    // block (SimRole::Wire), a climbable block (kGameClimbable) and a fluid
    // block (kGameFluid). Game content carries no climb/fluid blocks yet,
    // so the fixture defines its own -- the predicates are what is tested.
    BlockRegistry fb;
    const BlockId rockB = fb.add({"Rock", kSolid | kOpaque, Shape::Cube, "rock"});
    BlockDef wireDef{"Trace", kTransparent, Shape::Custom, "trace"};
    wireDef.role = uint16_t(SimRole::Wire);
    const BlockId wireB = fb.add(wireDef);
    const BlockId ladB = fb.add({"Ladder", kTransparent | kGameClimbable, Shape::Cross, "ladder"});
    const BlockId fluB = fb.add({"Fluid", kTransparent | kGameFluid, Shape::Cube, "fluid"});
    ChunkMap fw;
    fw.setRegistry(&fb);
    for (int cz = 0; cz < 4; ++cz)
      for (int cx = 0; cx < 4; ++cx) {
        auto c = std::make_unique<ChunkColumn>();
        c->cx = cx;
        c->cz = cz;
        fw.insert(std::move(c));
      }
    for (int z = 0; z < 64; ++z)
      for (int x = 0; x < 64; ++x) fw.set(x, 9, z, rockB);
    fw.set(10, 10, 10, wireB);
    fw.set(12, 10, 10, ladB);
    fw.set(14, 10, 10, fluB);

    AiHost h2;
    h2.bindWorld(fw, fb, nullptr, {2, 10, 2});
    h2.setEnabled(true);
    const ai::WalkCache* nv = h2.nav();
    check(nv && (nv->bits(10, 10, 10) & ai::kNavTrack), "ai host P0: SimRole::Wire cell carries the track bit (real NavRules)");
    check(nv && (nv->bits(12, 10, 10) & ai::kNavClimb), "ai host P0: kGameClimbable cell carries the climb bit");
    check(nv && (nv->bits(14, 10, 10) & ai::kNavFluid), "ai host P0: kGameFluid cell carries the fluid bit");

    // Climb through the production host cache: the rules now reach it.
    for (int y = 10; y <= 13; ++y) fw.set(20, y, 20, ladB);
    h2.advance(1, 512);  // drains the invalidation posts for those edits
    auto climbMode = ai::makeClimb();
    check(ai::findPath(*nv, {{19, 10, 20}, {20, 13, 20}, climbMode.get(), 8}).complete,
          "ai host P0: climb mode ascends a ladder column through the host cache");

    // The feed itself: an edit lands in the world; the cache is stale by
    // design until advance() drains the posts at the AI-tick boundary.
    check((nv->bits(7, 10, 40) & ai::kNavPassable) != 0, "ai host P0: cell reads passable before the edit");
    fw.set(7, 10, 40, rockB);
    check((nv->bits(7, 10, 40) & ai::kNavPassable) != 0, "ai host P0: cache stays stale until the tick boundary (no mid-plan churn)");
    h2.advance(1, 512);
    check((nv->bits(7, 10, 40) & ai::kNavPassable) == 0, "ai host P0: advance() drains edit posts -- cell is solid in the cache");

    // Path level: a 2-high wall across the corridor; after the drain the
    // same search detours instead of walking through the wall cells.
    for (int z = 32; z < 48; ++z) {
      fw.set(7, 10, z, rockB);
      fw.set(7, 11, z, rockB);
    }
    h2.advance(1, 512);
    const ai::Path detour = ai::findPath(*nv, {{2, 10, 40}, {12, 10, 40}, walk.get(), 16});
    bool throughWall = false;
    for (const ai::Cell& c : detour.cells)
      throughWall = throughWall || (c.x == 7 && c.y <= 11 && c.z >= 32 && c.z < 48);
    check(detour.complete && !throughWall, "ai host P0: after the drain, walk detours around the new wall");
    h2.unbindWorld();
  }

  // ---- B-H1 (2026-10-15): the invalidation queue is bounded by ZERO
  // while ai.enabled is off (the default), and still feeds when on ----
  {
    BlockRegistry rb;
    const BlockId rockC = rb.add({"Rock", kSolid | kOpaque, Shape::Cube, "rock"});
    ChunkMap rw;
    rw.setRegistry(&rb);
    for (int cz = 0; cz < 4; ++cz)
      for (int cx = 0; cx < 4; ++cx) {
        auto c = std::make_unique<ChunkColumn>();
        c->cx = cx;
        c->cz = cz;
        rw.insert(std::move(c));
      }
    for (int z = 0; z < 64; ++z)
      for (int x = 0; x < 64; ++x) rw.set(x, 9, z, rockC);

    AiHost h3;
    h3.bindWorld(rw, rb, nullptr, {2, 10, 2});  // disabled: the default session
    check(h3.pendingInvalidations() == 0, "ai host B-H1: queue starts empty while disabled");
    // Drive edits through the REAL path (ChunkMap::set/insert/erase fire
    // the installed listener on this thread, exactly as the sim thread
    // and the streamer do in play): 1000 sets + an insert + an erase.
    for (int i = 0; i < 1000; ++i) rw.set(1 + (i % 60), 10, 1 + ((i / 60) % 60), rockC);
    {
      auto c = std::make_unique<ChunkColumn>();
      c->cx = 9;
      c->cz = 9;
      rw.insert(std::move(c));
    }
    rw.erase(9, 9);
    check(h3.pendingInvalidations() == 0,
          "ai host B-H1: 1000+ edits while disabled queue NOTHING (posts dropped at the sink)");
    h3.advance(32, 32);  // still a no-op, and must not resurrect the queue
    check(h3.pendingInvalidations() == 0, "ai host B-H1: queue still empty after advance() while disabled");

    // Enabled path unchanged: posts queue, advance() drains them into
    // the cache, and the edited cell reads solid afterwards.
    h3.setEnabled(true);
    check(h3.pendingInvalidations() == 0, "ai host B-H1: enabling starts from an empty queue");
    rw.set(30, 10, 30, rockC);
    check(h3.pendingInvalidations() == 1, "ai host B-H1: an edit while enabled queues exactly one post");
    check(h3.nav() && (h3.nav()->bits(30, 10, 30) & ai::kNavPassable) != 0,
          "ai host B-H1: cache is stale before the drain (post queued, not yet applied)");
    h3.advance(1, 512);
    check(h3.pendingInvalidations() == 0, "ai host B-H1: advance() drains the queue to empty");
    check(h3.nav() && (h3.nav()->bits(30, 10, 30) & ai::kNavPassable) == 0,
          "ai host B-H1: the drained post invalidated the edited cell (enabled path intact)");

    // Disable again: the drop resumes immediately, no regrowth.
    h3.setEnabled(false);
    for (int i = 0; i < 100; ++i) rw.set(2, 10, 2 + (i % 50), rockC);
    check(h3.pendingInvalidations() == 0, "ai host B-H1: edits after disabling queue nothing again");
    h3.unbindWorld();
  }
  return fails;
}

}  // namespace lithokraft
