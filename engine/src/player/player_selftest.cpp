// IPlayer system self-tests (ctest: engine_player). Exit code = failure count.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "litholattice/player/ability_registry.h"
#include "litholattice/player/iplayer.h"
#include "litholattice/player/player.h"
#include "litholattice/player/player_state.h"
#include "litholattice/player/standard_stats.h"
#include "litholattice/player/stat_registry.h"
#include "litholattice/player/vital_registry.h"
#include "litholattice/player/vitals.h"

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fails;
}

struct RecordingListener : litholattice::IPlayerListener {
  int vitalEvents = 0;
  int statEvents = 0;
  int abilityEvents = 0;
  int stateEvents = 0;
  uint32_t lastVital = 0;
  uint32_t lastStat = 0;
  int32_t lastOld = 0, lastNew = 0;
  bool lastActive = false;
  void onVitalChanged(litholattice::IPlayer&, uint32_t vitalId, int32_t o, int32_t n) override {
    ++vitalEvents;
    lastVital = vitalId;
    lastOld = o;
    lastNew = n;
  }
  void onStatChanged(litholattice::IPlayer&, uint32_t statId, int32_t o, int32_t n) override {
    ++statEvents;
    lastStat = statId;
    lastOld = o;
    lastNew = n;
  }
  void onAbilityChanged(litholattice::IPlayer&, uint32_t, bool active) override {
    ++abilityEvents;
    lastActive = active;
  }
  void onPlayerStateChanged(litholattice::IPlayer&) override { ++stateEvents; }
};

}  // namespace

int main() {
  using namespace litholattice;
  const uint64_t kP1 = 1, kP2 = 2;

  // -- Stats: registration, defaults, clamp, adjust -------------------------
  // (Health/oxygen/stamina are VITALS now — tested in the IVitals section
  // below. Stats under test here are plain game-style numbers.)
  StatRegistry stats;
  const uint32_t might = stats.registerStat(StatDef{0, "might", 0, 64, 64});
  const uint32_t focus = stats.registerStat(StatDef{0, "focus", 0, 64, 0});
  check(might != 0 && focus != 0 && might != focus, "stat: ids assigned");
  check(stats.findStat("might") == might, "stat: findStat by name");

  check(stats.get(kP1, might) == 64, "stat: untouched reads default 64");
  stats.set(kP1, might, 100);
  check(stats.get(kP1, might) == 64, "stat: set clamps to max");
  stats.set(kP1, might, -5);
  check(stats.get(kP1, might) == 0, "stat: set clamps to min");
  stats.adjust(kP1, might, 30);
  check(stats.get(kP1, might) == 30 && stats.getBase(kP1, might) == 30, "stat: adjust");
  stats.adjust(kP1, might, -100);
  check(stats.get(kP1, might) == 0, "stat: adjust clamps at min");
  check(stats.get(kP2, might) == 64, "stat: players are independent");

  // Duplicate registration: first-wins, original def kept.
  const uint32_t dup = stats.registerStat(StatDef{0, "might", 0, 10, 5});
  check(dup == might, "stat: duplicate name returns existing id");
  check(stats.statDef(might)->maxValue == 64, "stat: duplicate keeps original def");
  check(stats.registerStat(StatDef{0, "", 0, 64, 0}) == 0, "stat: empty name rejected");
  check(stats.registerStat(StatDef{0, "bad", 10, 5, 0}) == 0, "stat: min > max rejected");
  check(stats.get(kP1, 9999) == 0 && stats.statDef(9999) == nullptr, "stat: unknown id inert");

  // -- Boost composition: effective = clamp(base + boosts), base untouched --
  stats.set(kP1, might, 40);
  stats.addBoost(kP1, might, 10, /*source=*/77);
  check(stats.get(kP1, might) == 50 && stats.getBase(kP1, might) == 40,
        "boost: get returns effective, base unchanged");
  stats.addBoost(kP1, might, 4, /*source=*/78);
  check(stats.get(kP1, might) == 54, "boost: distinct sources stack");
  stats.addBoost(kP1, might, 20, /*source=*/77);
  check(stats.get(kP1, might) == 64, "boost: same source replaces (and clamps)");
  stats.removeBoost(kP1, might, 77);
  check(stats.get(kP1, might) == 44, "boost: removeBoost removes exactly its source");
  stats.removeBoost(kP1, might, 78);
  check(stats.get(kP1, might) == 40, "boost: all removed returns to base");

  // -- Vitals (IVitals): standard set + modding-style custom registration ---
  VitalRegistry vitals;
  const StandardVitals vit = registerStandardVitals(vitals);
  check(vit.health != 0 && vit.oxygen != 0 && vit.stamina != 0, "vitals: ids assigned");
  check(vitals.findVital("health") == vit.health, "vitals: findVital by name");
  check(registerStandardVitals(vitals).health == vit.health, "vitals: re-register idempotent");
  check(vitals.findVital("might") == 0, "vitals: separate namespace from stats");

  check(vitals.get(kP1, vit.health) == 64, "vital: untouched reads default 64");
  vitals.set(kP1, vit.health, 100);
  check(vitals.get(kP1, vit.health) == 64, "vital: set clamps to max");
  vitals.set(kP1, vit.health, -5);
  check(vitals.get(kP1, vit.health) == 0, "vital: set clamps to min");
  vitals.adjust(kP1, vit.health, 30);
  check(vitals.get(kP1, vit.health) == 30, "vital: adjust");
  vitals.adjust(kP1, vit.health, -100);
  check(vitals.get(kP1, vit.health) == 0, "vital: adjust clamps at min");
  check(vitals.get(kP2, vit.health) == 64, "vital: players are independent");

  // Duplicate registration: first-wins, original def kept.
  const uint32_t vdup = vitals.registerVital(VitalDef{0, "health", 0, 10, 5});
  check(vdup == vit.health, "vital: duplicate name returns existing id");
  check(vitals.vitalDef(vit.health)->maxValue == 64, "vital: duplicate keeps original def");
  check(vitals.registerVital(VitalDef{0, "", 0, 64, 0}) == 0, "vital: empty name rejected");
  check(vitals.registerVital(VitalDef{0, "bad", 10, 5, 0}) == 0, "vital: min > max rejected");
  check(vitals.get(kP1, 9999) == 0 && vitals.vitalDef(9999) == nullptr, "vital: unknown id inert");

  // THE EXTENSIBILITY POINT: a vital the engine did not ship with (a mod's,
  // a new dimension's) registers by name and behaves like a standard one.
  const uint32_t pool = vitals.registerVital(VitalDef{0, "test_pool", 0, 100, 50});
  check(pool != 0 && pool != vit.health, "vital: custom vital registers with its own id");
  check(vitals.get(kP1, pool) == 50, "vital: custom vital reads its own default");
  vitals.set(kP1, pool, 250);
  check(vitals.get(kP1, pool) == 100, "vital: custom vital clamps to its own max");
  vitals.adjust(kP2, pool, -20);
  check(vitals.get(kP2, pool) == 30 && vitals.get(kP1, pool) == 100,
        "vital: custom vital is per-player");

  // -- Vital serialization round-trip ---------------------------------------
  vitals.set(kP1, vit.health, 33);
  vitals.set(kP1, vit.oxygen, 12);
  uint8_t vbuf[256];
  const uint32_t vwritten = vitals.serializeVitals(kP1, vbuf, sizeof(vbuf));
  check(vwritten == 4 + 3 * 8, "vital serialize: size = header + touched vitals");
  VitalRegistry vitals2;
  registerStandardVitals(vitals2);
  vitals2.registerVital(VitalDef{0, "test_pool", 0, 100, 50});
  check(vitals2.deserializeVitals(kP1, vbuf, vwritten) == vwritten, "vital deserialize: bytes read");
  check(vitals2.get(kP1, vit.health) == 33 && vitals2.get(kP1, vit.oxygen) == 12 &&
            vitals2.get(kP1, pool) == 100,
        "vital serialize: round-trip exact");
  check(vitals2.deserializeVitals(kP1, vbuf, vwritten - 1) == 0,
        "vital deserialize: truncated rejected");
  check(vitals2.get(kP1, vit.health) == 33, "vital deserialize: truncation changed nothing");
  check(vitals.serializeVitals(kP1, vbuf, 4) == 0, "vital serialize: small buffer writes nothing");

  // -- Abilities: grant/activate/boost/expiry/cooldown ----------------------
  AbilityRegistry abilities(stats);
  const uint32_t sprint = abilities.registerAbility(
      AbilityDef{0, "Sprintor", might, /*boost=*/16, /*duration=*/100, /*cooldown=*/50});
  check(sprint != 0, "ability: registered");
  check(!abilities.activate(kP1, sprint), "ability: activate without grant fails");
  abilities.grant(kP1, sprint);
  check(abilities.isGranted(kP1, sprint) && !abilities.isActive(kP1, sprint),
        "ability: granted, not yet active");
  stats.set(kP1, might, 40);
  check(abilities.activate(kP1, sprint), "ability: activate succeeds");
  check(stats.get(kP1, might) == 56 && stats.getBase(kP1, might) == 40,
        "ability: boost visible in stat get, base intact");
  abilities.tick(99);
  check(abilities.isActive(kP1, sprint), "ability: still active before duration elapses");
  abilities.tick(1);
  check(!abilities.isActive(kP1, sprint) && stats.get(kP1, might) == 40,
        "ability: expires on tick, boost removed");
  check(!abilities.activate(kP1, sprint), "ability: cooldown blocks reactivation");
  check(abilities.cooldownRemaining(kP1, sprint) == 50, "ability: cooldown counts in ticks");
  abilities.tick(50);
  check(abilities.activate(kP1, sprint), "ability: activates after cooldown");
  abilities.deactivate(kP1, sprint);
  check(stats.get(kP1, might) == 40 && abilities.cooldownRemaining(kP1, sprint) == 50,
        "ability: manual deactivate starts cooldown too");

  const uint32_t passive = abilities.registerAbility(
      AbilityDef{0, "Enduror", might, /*boost=*/8, /*duration=*/0, /*cooldown=*/0});
  abilities.grant(kP1, passive);
  check(abilities.activate(kP1, passive), "ability: permanent activates");
  abilities.tick(100000);
  check(abilities.isActive(kP1, passive) && stats.get(kP1, might) == 48,
        "ability: permanent never expires");
  abilities.revoke(kP1, passive);
  check(!abilities.isGranted(kP1, passive) && stats.get(kP1, might) == 40,
        "ability: revoke deactivates and strips boost");

  // -- Stat serialization round-trip ----------------------------------------
  stats.set(kP1, might, 33);
  stats.set(kP1, focus, 12);
  uint8_t buf[256];
  const uint32_t written = stats.serializeStats(kP1, buf, sizeof(buf));
  check(written == 4 + 2 * 8, "stat serialize: size = header + touched stats");
  StatRegistry stats2;
  const uint32_t might2 = stats2.registerStat(StatDef{0, "might", 0, 64, 64});
  const uint32_t focus2 = stats2.registerStat(StatDef{0, "focus", 0, 64, 0});
  check(might2 == might && focus2 == focus, "stat serialize: same registration order, same ids");
  check(stats2.deserializeStats(kP1, buf, written) == written, "stat deserialize: bytes read");
  check(stats2.getBase(kP1, might) == 33 && stats2.getBase(kP1, focus) == 12,
        "stat serialize: round-trip exact");
  check(stats2.deserializeStats(kP1, buf, written - 1) == 0, "stat deserialize: truncated rejected");
  check(stats2.getBase(kP1, might) == 33, "stat deserialize: truncation changed nothing");
  check(stats.serializeStats(kP1, buf, 4) == 0, "stat serialize: small buffer writes nothing");

  // -- Standard stats: XP is a STAT, not a vital ----------------------------
  {
    StatRegistry xpStats;
    VitalRegistry xpVitals;
    registerStandardVitals(xpVitals);
    const StandardStats ss = registerStandardStats(xpStats);
    check(ss.xp != 0, "xp: id assigned");
    check(xpStats.findStat("xp") == ss.xp, "xp: findStat by name");
    check(xpVitals.findVital("xp") == 0, "xp: NOT a vital (JD classification)");
    check(registerStandardStats(xpStats).xp == ss.xp, "xp: re-register idempotent");
    check(xpStats.get(kP1, ss.xp) == 0, "xp: untouched reads default 0");
    xpStats.adjust(kP1, ss.xp, 150);
    check(xpStats.get(kP1, ss.xp) == 150, "xp: adjust accumulates");
    xpStats.set(kP1, ss.xp, 100000);
    check(xpStats.get(kP1, ss.xp) == 100000, "xp: not clamped to the 64 scale");
    xpStats.adjust(kP1, ss.xp, -200000);
    check(xpStats.get(kP1, ss.xp) == 0, "xp: clamps at 0");
    check(xpStats.get(kP2, ss.xp) == 0, "xp: players are independent");
  }

  // -- PlayerState serialization round-trip ----------------------------------
  PlayerState s;
  s.playerId = 42;
  s.x = 123.5; s.y = -64.25; s.z = 1e9;
  s.vx = 0.125; s.vy = -0.5; s.vz = 3.75;
  s.yaw = 1.5f; s.pitch = -0.25f;
  s.dimension = 3; s.health = 17;
  uint8_t wire[PlayerState::kSerializedSize];
  check(s.serialize(wire, sizeof(wire)) == PlayerState::kSerializedSize, "state serialize: size");
  PlayerState r;
  check(r.deserialize(wire, sizeof(wire)) == PlayerState::kSerializedSize, "state deserialize: size");
  check(r.playerId == 42 && r.x == 123.5 && r.y == -64.25 && r.z == 1e9 && r.vx == 0.125 &&
            r.vy == -0.5 && r.vz == 3.75 && r.yaw == 1.5f && r.pitch == -0.25f &&
            r.dimension == 3 && r.health == 17,
        "state serialize: round-trip exact");
  check(r.deserialize(wire, PlayerState::kSerializedSize - 1) == 0 && r.playerId == 42,
        "state deserialize: truncated rejected, state untouched");
  check(s.serialize(wire, 8) == 0, "state serialize: small buffer rejected");

  // -- PlayerManager: local/add/get/remove, routing, cleanup -----------------
  PlayerManager mgr;
  const StandardVitals mv = registerStandardVitals(mgr.vitals());
  RecordingListener listener;
  Player& local = mgr.localPlayer();
  check(local.playerId() == mgr.localId() && mgr.count() == 1, "manager: local created");
  local.addListener(&listener);
  check(&mgr.local() == &local.state(), "manager: local() is localPlayer().state()");

  local.setVital(mv.health, 20);
  check(listener.vitalEvents == 1 && listener.lastVital == mv.health && listener.lastOld == 64 &&
            listener.lastNew == 20,
        "manager: listener fires on vital change (routed, once)");
  check(local.alive(), "manager: alive at health 20");
  check(local.state().health == 20, "manager: health vital mirrored into state field");
  local.setVital(mv.health, 0);
  check(!local.alive(), "manager: dead at health 0 (derived)");
  check(local.state().health == 0, "manager: health mirror follows to 0");

  PlayerState remote;
  remote.playerId = 7;
  remote.x = 5; remote.health = 44;
  mgr.addOrUpdate(remote);
  check(mgr.has(7) && mgr.count() == 2, "manager: addOrUpdate adds remote player");
  check(mgr.get(7).x == 5 && mgr.vitals().get(7, mv.health) == 44,
        "manager: addOrUpdate syncs state + health vital");
  check(mgr.get(999).playerId == 0, "manager: unknown id returns nobody sentinel");
  const auto ids = mgr.playerIds();
  check(ids.size() == 2 && ids[0] < ids[1], "manager: playerIds ascending");

  // Give the remote player stats + vitals + an active ability, then remove
  // them: nothing of theirs may survive in the registries.
  const uint32_t charge = mgr.stats().registerStat(StatDef{0, "charge", 0, 64, 0});
  const uint32_t dash = mgr.abilities().registerAbility(
      AbilityDef{0, "Dashor", charge, /*boost=*/10, /*duration=*/0, /*cooldown=*/0});
  mgr.abilities().grant(7, dash);
  mgr.stats().set(7, charge, 60);
  mgr.abilities().activate(7, dash);
  mgr.vitals().set(7, mv.oxygen, 3);
  check(mgr.stats().get(7, charge) == 64,
        "manager: remote boost clamps at max (base 60 + 10)");
  mgr.notifyStateChanged(7);
  mgr.remove(7);
  check(!mgr.has(7) && mgr.stats().touchedStats(7).empty() &&
            mgr.vitals().touchedVitals(7).empty(),
        "manager: remove purges player + stat + vital values");
  check(!mgr.abilities().isGranted(7, dash) && !mgr.abilities().isActive(7, dash),
        "manager: remove purges ability records");

  // State-change notification + standalone Player direct-fire path.
  local.state().x = 99;
  local.notifyStateChanged();
  check(listener.stateEvents == 1, "manager: notifyStateChanged fires listener");

  VitalRegistry soloVitals;
  StatRegistry soloStats;
  AbilityRegistry soloAbilities(soloStats);
  const StandardVitals sv = registerStandardVitals(soloVitals);
  const uint32_t soloMight = soloStats.registerStat(StatDef{0, "might", 0, 64, 64});
  Player solo(9, "Solo", soloVitals, soloStats, soloAbilities);
  RecordingListener soloListener;
  solo.addListener(&soloListener);
  solo.setVital(sv.health, 10);
  check(soloListener.vitalEvents == 1 && soloListener.lastNew == 10,
        "player: standalone setVital fires listener directly");
  solo.setStat(soloMight, 12);
  check(soloListener.statEvents == 1 && soloListener.lastNew == 12,
        "player: standalone setStat fires listener directly");
  check(solo.displayName() == "Solo" && solo.playerId() == 9, "player: identity");
  solo.removeListener(&soloListener);
  solo.setVital(sv.health, 5);
  check(soloListener.vitalEvents == 1, "player: removed listener stays silent");

  std::printf("player selftest: %s (%d failure%s)\n", g_fails ? "FAILED" : "OK", g_fails,
              g_fails == 1 ? "" : "s");
  return g_fails;
}
