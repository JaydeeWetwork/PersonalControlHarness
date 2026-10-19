// Deceitor self-test (app selftest section): drives the production
// brain + director against a fixture AiWorld (the ai_host_selftest
// floor) and a real EntityManager for ground drops. Every scenario is
// deterministic: AiWorld RNG is seeded per (world seed, mob id, tick)
// and the fixture player never moves except as scripted.
//
//   * liked-items table (titanium oxide liked; plain materials not);
//   * ordinary barter: offer consumed, fed set, return tossed from the
//     ordinary pool after the beat; the approach face is the Smile;
//   * liked barter: enthusiastic-pool return on the shorter beat;
//   * betrayal: unfed + back turned eventually turns hostile and the
//     chase lands damage; facing the mob never triggers it; a recent
//     barter suppresses it inside the unfed window;
//   * failed click, both branches: throw within grace diverts to
//     barter; no throw -> hostile at grace expiry;
//   * pack retaliation: attacking one enrages the pack in radius, and
//     only the pack.
#include "lithokraft/game/deceitor.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "litholattice/entity/entity.h"
#include "litholattice/world/chunk_map.h"
#include "lithokraft/content/blocks.h"
#include "lithokraft/content/items.h"
#include "lithokraft/content/silica.h"
#include "lithokraft/game/entities.h"

namespace lithokraft {

namespace {

struct Fixture {
  // Fixture floor map (own registry): 8x8 columns, solid y=9, walk y=10.
  BlockRegistry blocks;
  ChunkMap map;
  // Declaration order is a lifetime contract: `world` (declared last)
  // destructs FIRST, so its mob brains unregister from `dir` while the
  // director is still alive. (Game upholds the same order: the
  // director member precedes aiHost_.)
  DeceitorDirector dir;
  EntityManager entities{[](uint32_t) { return std::unique_ptr<IEntityBehavior>{}; }};
  std::unique_ptr<ai::WalkCache> nav;
  std::unique_ptr<ai::AiWorld> world;

  // Scripted player.
  double px = 4.5, py = 10.0, pz = 8.5, fx = 0.0, fz = -1.0;  // facing -z
  int damageTaken = 0;
  std::vector<BarterReturn> returns;  // tossReturn record

  Fixture() {
    const BlockId rock = blocks.add({"Rock", kSolid | kOpaque, Shape::Cube, "rock"});
    map.setRegistry(&blocks);
    for (int cz = 0; cz < 8; ++cz)
      for (int cx = 0; cx < 8; ++cx) {
        auto c = std::make_unique<ChunkColumn>();
        c->cx = cx;
        c->cz = cz;
        map.insert(std::move(c));
      }
    for (int z = 0; z < 128; ++z)
      for (int x = 0; x < 128; ++x) map.set(x, 9, z, rock);
    nav = std::make_unique<ai::WalkCache>(map, blocks, ai::NavRules{});
    world = std::make_unique<ai::AiWorld>(*nav, ai::AiConfig{});
    world->setObservers({{4, 10, 8}});

    DeceitorHooks h;
    h.playerPose = [this](double& x, double& y, double& z, double& fx, double& fz) {
      x = px; y = py; z = pz; fx = this->fx; fz = this->fz;
      return true;
    };
    h.nearestDrop = [this](double x, double y, double z, double radius) -> std::optional<DeceitorDrop> {
      DeceitorDrop best;
      double bestD = radius;
      bool found = false;
      for (Entity* e : entities.querySphere(x, y, z, radius, kEntityItem)) {
        const double dx = e->x - x, dz = e->z - z;
        const double d = std::sqrt(dx * dx + dz * dz);
        if (!found || d < bestD) {
          found = true;
          bestD = d;
          best = DeceitorDrop{e->id, e->data, uint16_t(e->count), e->x, e->y, e->z};
        }
      }
      return found ? std::optional<DeceitorDrop>(best) : std::nullopt;
    };
    h.consumeDrop = [this](uint64_t id) { entities.despawn(id); entities.tick(0.0); };
    h.tossReturn = [this](double x, double y, double z, uint32_t itemId, uint16_t count, double, double,
                          double) {
      returns.push_back({itemId, count});
      const uint64_t id = entities.spawn(kEntityItem, x, y, z);
      if (Entity* e = entities.get(id)) {
        e->data = itemId;
        e->count = count;
      }
    };
    h.hurtPlayer = [this](int dmg) { damageTaken += dmg; };
    dir.setHooks(std::move(h));
  }

  uint64_t drop(uint32_t itemId, double x, double z) {
    const uint64_t id = entities.spawn(kEntityItem, x, 10.0, z);
    if (Entity* e = entities.get(id)) {
      e->data = itemId;
      e->count = 1;
    }
    return id;
  }
  void ticks(int n) {
    for (int i = 0; i < n; ++i) world->tick();
  }
  DeceitorBrain* brain(uint32_t id) { return dir.brain(id); }
  bool itemPresent(uint64_t entityId) { return entities.get(entityId) != nullptr; }
};

bool inPool(uint32_t id, std::initializer_list<uint32_t> pool) {
  for (const uint32_t p : pool)
    if (p == id) return true;
  return false;
}

}  // namespace

int runDeceitorSelfTest() {
  using namespace litholattice;
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
  };

  // -- liked-items table ------------------------------------------------
  check(deceitorLikes(kItemTitaniumOxide), "deceitor: titanium oxide is a liked item (JD-named)");
  check(!deceitorLikes(uint32_t(kWaferSilicon)), "deceitor: Wafer Silicon is not a liked item");
  check(!deceitorLikes(kItemSilicaSeed), "deceitor: silica seed is not a liked item");

  {  // -- ordinary barter + the approach smile -------------------------
    Fixture f;
    const auto id = f.dir.spawn(*f.world, {4, 10, 4});
    check(id.has_value(), "deceitor: spawn through the director works");
    f.ticks(1);  // brain registers; player 4 away, facing it
    DeceitorBrain* b = f.brain(*id);
    check(b != nullptr, "deceitor: brain registered itself with the director");
    check(b && b->face() == DeceitorFace::Smile,
          "deceitor: approach face is the Smile (identical to a Receiptor's)");
    const uint64_t dropId = f.drop(uint32_t(kWaferSilicon), 4.5, 6.5);
    f.ticks(30);
    check(!f.itemPresent(dropId), "deceitor: barter offer on the ground is consumed");
    check(b && b->everFed(), "deceitor: a completed barter marks the mob fed");
    check(b && b->mood() == DeceitorMood::Neutral, "deceitor: barter leaves the mob neutral");
    f.ticks(15);  // ordinary beat is 10 AI ticks
    check(f.returns.size() == 1, "deceitor: exactly one barter return is tossed");
    check(f.returns.size() == 1 &&
              inPool(f.returns[0].itemId, {uint32_t(kWaferSilicon), uint32_t(kDirt), uint32_t(kStone),
                                           uint32_t(kGlass), kItemSilicaSeed}),
          "deceitor: ordinary return comes from the ordinary pool (existing items only)");
  }

  {  // -- liked barter: enthusiastic pool, shorter beat ----------------
    Fixture f;
    const auto id = f.dir.spawn(*f.world, {4, 10, 4});
    f.ticks(1);
    DeceitorBrain* b = f.brain(*id);
    const uint64_t dropId = f.drop(kItemTitaniumOxide, 4.5, 6.5);
    int ticksToReturn = -1;
    for (int i = 0; i < 40 && ticksToReturn < 0; ++i) {
      f.ticks(1);
      if (!f.returns.empty()) ticksToReturn = i;
    }
    check(!f.itemPresent(dropId) && b && b->everFed(), "deceitor: liked offer is consumed and feeds");
    check(f.returns.size() == 1 &&
              inPool(f.returns[0].itemId, {uint32_t(kChipOR), uint32_t(kLamp),
                                           uint32_t(kResonatorCrystal), uint32_t(kCapacitorCan),
                                           uint32_t(kGlass)}),
          "deceitor: liked return comes from the enthusiastic pool");
    check(ticksToReturn >= 0 && ticksToReturn < 30,
          "deceitor: liked barter completes promptly (walk + short beat)");
  }

  {  // -- betrayal: unfed + back turned -> hostile -> damage -----------
    Fixture f;
    const auto id = f.dir.spawn(*f.world, {4, 10, 4});
    f.fz = 1.0;  // player faces +z: back turned to the mob at z=4
    f.ticks(1);
    DeceitorBrain* b = f.brain(*id);
    check(b && b->face() == DeceitorFace::Neutral,
          "deceitor: no smile is shown to a turned back (mask needs a face)");
    bool hostile = false;
    for (int i = 0; i < 4000 && !hostile; ++i) {
      f.ticks(1);
      hostile = b && b->mood() == DeceitorMood::Hostile;
    }
    check(hostile, "deceitor: unfed + back turned eventually triggers betrayal (seeded roll)");
    check(b && b->face() == DeceitorFace::Scowl, "deceitor: open hostility drops the mask (Scowl)");
    f.ticks(600);
    check(f.damageTaken > 0, "deceitor: the betrayed mob chases and lands melee damage");
  }

  {  // -- no betrayal while facing --------------------------------------
    Fixture f;
    const auto id = f.dir.spawn(*f.world, {4, 10, 4});
    f.ticks(1);
    DeceitorBrain* b = f.brain(*id);
    f.ticks(3000);  // far beyond the 600-tick unfed window
    check(b && b->mood() != DeceitorMood::Hostile,
          "deceitor: no betrayal while the player faces the mob, even unfed");
  }

  {  // -- no betrayal while fed -----------------------------------------
    Fixture f;
    const auto id = f.dir.spawn(*f.world, {4, 10, 4});
    f.ticks(1);
    DeceitorBrain* b = f.brain(*id);
    f.drop(uint32_t(kWaferSilicon), 4.5, 6.5);
    f.ticks(30);
    check(b && b->everFed(), "deceitor (fed setup): barter completed");
    f.fz = 1.0;  // back turned now — but inside the fed window
    f.ticks(500);
    check(b && b->mood() != DeceitorMood::Hostile,
          "deceitor: no betrayal inside the fed window, back turned or not");
  }

  {  // -- failed click, diverted by a quick throw ------------------------
    Fixture f;
    const auto id = f.dir.spawn(*f.world, {4, 10, 4});
    f.ticks(1);
    DeceitorBrain* b = f.brain(*id);
    const bool opened = f.dir.playerInteract(*id, f.world->stats().ticks);
    check(!opened, "deceitor: interacting opens NO interface (the tell vs a Receiptor)");
    check(b && b->mood() == DeceitorMood::Provoked, "deceitor: a failed click provokes (grace runs)");
    f.drop(uint32_t(kWaferSilicon), 4.5, 6.5);  // the immediate throw
    f.ticks(30);                                // inside the 30-tick grace
    check(b && b->mood() == DeceitorMood::Neutral && b->everFed(),
          "deceitor: a throw inside the grace window diverts to barter, not hostility");
  }

  {  // -- failed click, no throw: hostile at expiry ----------------------
    Fixture f;
    const auto id = f.dir.spawn(*f.world, {4, 10, 4});
    f.ticks(1);
    DeceitorBrain* b = f.brain(*id);
    f.dir.playerInteract(*id, f.world->stats().ticks);
    f.ticks(60);  // grace is 30 AI ticks
    check(b && b->mood() == DeceitorMood::Hostile,
          "deceitor: a failed click with no throw turns hostile at grace expiry");
  }

  {  // -- pack retaliation ------------------------------------------------
    Fixture f;
    const auto a = f.dir.spawn(*f.world, {4, 10, 4});
    const auto bId = f.dir.spawn(*f.world, {8, 10, 8});
    const auto c = f.dir.spawn(*f.world, {12, 10, 4});
    const auto far = f.dir.spawn(*f.world, {100, 10, 100});
    f.ticks(4);  // brains register (the far mob thinks at LOD rate 2+)
    f.dir.playerAttacked(*f.world, *a);
    check(f.brain(*a) && f.brain(*a)->mood() == DeceitorMood::Hostile,
          "deceitor: the attacked mob turns hostile");
    check(f.brain(*bId) && f.brain(*bId)->mood() == DeceitorMood::Hostile &&
              f.brain(*c) && f.brain(*c)->mood() == DeceitorMood::Hostile,
          "deceitor: pack retaliation — nearby Deceitors go hostile together");
    check(f.brain(*far) && f.brain(*far)->mood() == DeceitorMood::Neutral,
          "deceitor: a Deceitor outside the pack radius stays neutral");
  }

  return fails;
}

}  // namespace lithokraft
