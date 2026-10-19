// EntityManager + moveCollide self-tests (ctest: engine_entity).
// Pins H-ENT-5 (persistent sorted ids, behaviour exception boundary) and
// H-ENT-6 (moveCollide substepping) from review Appendix H. Exit = failures.
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>

#include "litholattice/entity/entity.h"
#include "litholattice/entity/physics.h"

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fails;
}

using namespace litholattice;
using Solid = std::function<bool(int, int, int)>;

struct NoopBehavior final : IEntityBehavior {
  void onTick(Entity&, double) override {}
};
struct ThrowBehavior final : IEntityBehavior {
  void onTick(Entity&, double) override { throw std::runtime_error("boom"); }
};
struct CountBehavior final : IEntityBehavior {
  int* ticks = nullptr;
  void onTick(Entity&, double) override { ++*ticks; }
};

}  // namespace

int main() {
  std::printf("entity self-test\n");

  // H-ENT-5: a throwing behaviour is contained — it does not propagate, the
  // offender is culled in the same tick, and later entities still tick.
  {
    int counted = 0;
    EntityManager mgr([&](uint32_t type) -> std::unique_ptr<IEntityBehavior> {
      if (type == 1) return std::make_unique<ThrowBehavior>();
      auto b = std::make_unique<CountBehavior>();
      b->ticks = &counted;
      return b;
    });
    const uint64_t bad = mgr.spawn(1, 0, 0, 0);
    const uint64_t good = mgr.spawn(2, 1, 0, 0);
    bool threw = false;
    try {
      mgr.tick(1.0 / 60.0);
    } catch (...) {
      threw = true;
    }
    check(!threw, "entity: throwing behaviour does not propagate out of tick()");
    check(mgr.get(bad) == nullptr, "entity: throwing behaviour's entity is culled that tick");
    check(mgr.get(good) != nullptr && counted == 1 && mgr.get(good)->age == 1,
          "entity: entities after the thrower still tick (age + behaviour)");
    check(mgr.size() == 1, "entity: size reflects the cull");
  }

  // H-ENT-5: querySphere returns id order regardless of spawn/despawn history.
  {
    EntityManager mgr([](uint32_t) { return std::make_unique<NoopBehavior>(); });
    const uint64_t a = mgr.spawn(1, 0, 0, 0);
    const uint64_t b = mgr.spawn(1, 0.5, 0, 0);
    const uint64_t c = mgr.spawn(1, 1.0, 0, 0);
    mgr.despawn(b);
    mgr.tick(0.0);  // culls b
    const uint64_t d = mgr.spawn(1, 0.25, 0, 0);
    auto hits = mgr.querySphere(0, 0, 0, 4.0);
    check(hits.size() == 3 && hits[0]->id == a && hits[1]->id == c && hits[2]->id == d,
          "entity: querySphere is in id order after cull + respawn");
  }

  // H-ENT-6: at terminal fall speed with a 250 ms hitch dt (6 blocks in one
  // step), an entity must land ON a 1-block floor, not tunnel through it.
  {
    const Solid floor = [](int, int y, int) { return y == 5; };  // one block thick
    Entity e;
    e.x = 0.5; e.y = 10.0; e.z = 0.5;
    e.vy = -24.0;  // terminal velocity
    moveCollide(e, floor, 0.15f, 0.3f, 0.25);
    check(e.onGround && e.y == 6.0, "entity: moveCollide substeps — no tunnelling at hitch dt");
    // Ordinary frame dt is unchanged: a small fall moves exactly v*dt.
    Entity e2;
    e2.x = 0.5; e2.y = 10.0; e2.z = 0.5;
    e2.vy = -24.0;
    moveCollide(e2, floor, 0.15f, 0.3f, 1.0 / 60.0);
    check(!e2.onGround && std::fabs(e2.y - 9.6) < 1e-9,
          "entity: moveCollide single-step at frame dt is unchanged");
  }

  std::printf(g_fails ? "ENTITY SELFTEST FAILED (%d)\n" : "ENTITY SELFTEST OK\n", g_fails);
  return g_fails;
}
