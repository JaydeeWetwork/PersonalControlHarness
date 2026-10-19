// GamePlayerAdapter implementation. LF line endings.
#include "lithokraft/game/player_adapter.h"

#include <algorithm>
#include <cmath>

#include "litholattice/player/player.h"       // concrete Player (complete type)
#include "litholattice/player/stat.h"         // StatDef
#include "litholattice/player/stat_registry.h"  // touchedStats
#include "lithokraft/content/blocks.h"        // blockRegistry()

namespace lithokraft {

Vec3 playerEyePos(const Player& p) { return playerEyePos(p, kPlayerEyeHeight); }

Vec3 playerEyePos(const Player& p, float eyeHeight) { return {p.pos.x, p.pos.y + eyeHeight, p.pos.z}; }

bool blockIsLiquid(BlockId id) {
  if (id == kAir) return false;
  const BlockDef& def = blockRegistry().def(id);
  return def.behavior == "water" || def.behavior == "liquid" || def.behavior == "lava";
}

GamePlayerAdapter::GamePlayerAdapter(Player& gamePlayer) : game_(gamePlayer) {
  // Standard vitals first (health/oxygen/stamina, 0..64, default 64, on
  // the IVitals registry), then the engine's standard stat (xp) and
  // LithoKraft's own stats (kharge, static) on IStat. Registration is
  // first-wins by name, so this is idempotent against anything the manager
  // registered itself.
  vitals_ = litholattice::registerStandardVitals(manager_.vitals());
  standardStats_ = litholattice::registerStandardStats(manager_.stats());
  kharge_ = manager_.stats().registerStat(litholattice::StatDef{0, "kharge", 0, 64, 0});
  // "static": LithoKraft's XP analogue (JD design 2026-10-12) — earned
  // from mobs and activities, spent at the certificate table. A stat, not
  // a vital (same ruling as xp). XP-sized range, not the 64 pool scale.
  static_ = manager_.stats().registerStat(
      litholattice::StatDef{0, "static", 0, INT32_MAX, 0});
  (void)manager_.localPlayer();  // create the local IPlayer up front
  syncFromGame(1, 0.f);          // 1 = Wafer until the game passes the real dimension
}

litholattice::IPlayer& GamePlayerAdapter::enginePlayer() { return manager_.localPlayer(); }

const litholattice::IPlayer& GamePlayerAdapter::enginePlayer() const {
  // The constructor created the local player; it lives as long as the manager.
  return *manager_.player(manager_.localId());
}

bool GamePlayerAdapter::teleportTripped(float dtSeconds) {
  // Implied speed of the position delta since the last sync; a
  // discontinuity (respawn / dimension switch / scripted move) trips the
  // guard. The baseline refreshes here so every sync path shares it.
  bool tripped = false;
  if (hasPrevPos_ && dtSeconds > 1e-6f) {
    const Vec3 d{game_.pos.x - prevPos_.x, game_.pos.y - prevPos_.y, game_.pos.z - prevPos_.z};
    const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    tripped = dist / dtSeconds > kTeleportSpeed;
  }
  prevPos_ = game_.pos;
  hasPrevPos_ = true;
  return tripped;
}

void GamePlayerAdapter::mirrorState(uint8_t dimensionNumber, double vx, double vy, double vz) {
  litholattice::IPlayer& p = enginePlayer();
  litholattice::PlayerState& st = p.state();
  // PLY-4: onPlayerStateChanged is a CHANGE notification — Game mirrors
  // every frame, and firing it unconditionally spammed listeners 60x/s
  // for a player standing still. Write the fields directly and notify
  // only when something actually changed (position, look, dimension,
  // health, or velocity). The written values are exactly what the old
  // setVelocity() path wrote; only the notification is conditional.
  const uint8_t health = uint8_t(std::clamp(p.vital(vitals_.health), 0, 64));
  const bool changed = st.x != double(game_.pos.x) || st.y != double(game_.pos.y) ||
                       st.z != double(game_.pos.z) || st.yaw != game_.yaw ||
                       st.pitch != game_.pitch || st.dimension != dimensionNumber ||
                       st.health != health || st.vx != vx || st.vy != vy || st.vz != vz;
  st.x = game_.pos.x;
  st.y = game_.pos.y;
  st.z = game_.pos.z;
  st.yaw = game_.yaw;
  st.pitch = game_.pitch;
  st.dimension = dimensionNumber;
  // The manager's routing already mirrors the health VITAL into this
  // field; set it here too so the mirror holds even for direct state
  // readers.
  st.health = health;
  st.vx = vx;
  st.vy = vy;
  st.vz = vz;
  if (changed) p.notifyStateChanged();
}

void GamePlayerAdapter::syncFromGame(uint8_t dimensionNumber, float dtSeconds) {
  // Legacy path (init-time syncs, before the locomotion body is the
  // movement authority): velocity derived from the movement actually
  // applied since the last sync, teleport-guarded.
  double vx = 0, vy = 0, vz = 0;
  if (hasPrevPos_ && dtSeconds > 1e-6f) {
    const Vec3 d{game_.pos.x - prevPos_.x, game_.pos.y - prevPos_.y, game_.pos.z - prevPos_.z};
    const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (dist / dtSeconds <= kTeleportSpeed) {
      vx = d.x / dtSeconds;
      vy = d.y / dtSeconds;
      vz = d.z / dtSeconds;
    }
  }
  prevPos_ = game_.pos;
  hasPrevPos_ = true;
  mirrorState(dimensionNumber, vx, vy, vz);
}

void GamePlayerAdapter::syncFromGame(uint8_t dimensionNumber, float dtSeconds, const Vec3& trueVelocity) {
  // The locomotion body's velocity is the truth; the teleport guard still
  // vetoes it for one sync when the position itself jumped (a teleport
  // tick reports zero velocity once).
  if (teleportTripped(dtSeconds)) {
    mirrorState(dimensionNumber, 0.0, 0.0, 0.0);
    return;
  }
  mirrorState(dimensionNumber, trueVelocity.x, trueVelocity.y, trueVelocity.z);
}

int32_t GamePlayerAdapter::health() const { return enginePlayer().vital(vitals_.health); }
int32_t GamePlayerAdapter::oxygen() const { return enginePlayer().vital(vitals_.oxygen); }
int32_t GamePlayerAdapter::stamina() const { return enginePlayer().vital(vitals_.stamina); }
int32_t GamePlayerAdapter::kharge() const { return enginePlayer().stat(kharge_); }
int32_t GamePlayerAdapter::xp() const { return enginePlayer().stat(standardStats_.xp); }
int32_t GamePlayerAdapter::staticCharge() const { return enginePlayer().stat(static_); }
bool GamePlayerAdapter::alive() const { return enginePlayer().alive(); }

std::vector<std::pair<std::string, double>> GamePlayerAdapter::namedStats() const {
  const litholattice::IPlayer& p = enginePlayer();
  const litholattice::VitalRegistry& vreg = manager_.vitals();
  const litholattice::StatRegistry& reg = manager_.stats();
  std::vector<std::pair<std::string, double>> out;
  auto seen = [&](const char* name) {
    for (const auto& [n, v] : out)
      if (n == name) return true;
    return false;
  };
  auto addVital = [&](uint32_t vitalId) {
    if (!vitalId) return;
    const litholattice::VitalDef* def = vreg.vitalDef(vitalId);
    if (!def || !def->name || seen(def->name)) return;
    out.emplace_back(def->name, double(p.vital(vitalId)));
  };
  auto addStat = [&](uint32_t statId) {
    if (!statId) return;
    const litholattice::StatDef* def = reg.statDef(statId);
    if (!def || !def->name || seen(def->name)) return;
    out.emplace_back(def->name, double(p.stat(statId)));
  };
  addVital(vitals_.health);
  addVital(vitals_.oxygen);
  addVital(vitals_.stamina);
  for (uint32_t id : vreg.touchedVitals(p.playerId())) addVital(id);
  addStat(standardStats_.xp);
  addStat(kharge_);
  addStat(static_);
  for (uint32_t id : reg.touchedStats(p.playerId())) addStat(id);
  return out;
}

std::map<std::string, int32_t> GamePlayerAdapter::statsForSave() const {
  const litholattice::IPlayer& p = enginePlayer();
  const litholattice::StatRegistry& reg = manager_.stats();
  std::map<std::string, int32_t> out;
  for (uint32_t id : reg.touchedStats(p.playerId())) {
    const litholattice::StatDef* def = reg.statDef(id);
    if (def && def->name) out[def->name] = reg.getBase(p.playerId(), id);
  }
  return out;
}

void GamePlayerAdapter::applySavedStats(const std::map<std::string, int32_t>& saved) {
  const uint64_t pid = enginePlayer().playerId();
  litholattice::StatRegistry& reg = manager_.stats();
  for (const auto& [name, value] : saved) {
    const uint32_t id = reg.findStat(name);
    if (id) reg.set(pid, id, value);  // unknown names ignored; set() clamps
  }
}

std::map<std::string, int32_t> GamePlayerAdapter::vitalsForSave() const {
  const litholattice::IPlayer& p = enginePlayer();
  const litholattice::VitalRegistry& reg = manager_.vitals();
  std::map<std::string, int32_t> out;
  for (uint32_t id : reg.touchedVitals(p.playerId())) {
    const litholattice::VitalDef* def = reg.vitalDef(id);
    if (def && def->name) out[def->name] = reg.get(p.playerId(), id);
  }
  return out;
}

void GamePlayerAdapter::applySavedVitals(const std::map<std::string, int32_t>& saved) {
  const uint64_t pid = enginePlayer().playerId();
  litholattice::VitalRegistry& reg = manager_.vitals();
  for (const auto& [name, value] : saved) {
    const uint32_t id = reg.findVital(name);
    if (id) reg.set(pid, id, value);  // unknown names ignored; set() clamps
  }
}

void GamePlayerAdapter::tickVitals(uint64_t ticks, bool headInLiquid, bool sprinting, int targetTps) {
  if (!ticks) return;
  // PLY-1: convert world ticks at targetTps into reference-rate (32 Hz)
  // ticks before the calibrated accumulators below see them. The
  // remainder carries in vitalsRateRem_, so the long-run rate is exact
  // at any TPS; at 32 TPS this is the identity (remainder stays 0).
  {
    const uint64_t tps = targetTps > 0 ? uint64_t(targetTps) : kVitalsReferenceTps;
    if (tps != kVitalsReferenceTps) {
      const uint64_t scaled = vitalsRateRem_ + ticks * kVitalsReferenceTps;
      ticks = scaled / tps;
      vitalsRateRem_ = scaled % tps;
      if (!ticks) return;  // not yet one whole reference tick of world time
    }
  }
  litholattice::IPlayer& p = enginePlayer();
  const uint64_t pid = p.playerId();
  litholattice::IVitals& vitals = p.vitals();
  // A single call never moves a bar by more than its whole range.
  auto points = [](uint64_t accumulated, uint64_t perPoint) {
    return int32_t(std::min<uint64_t>(accumulated / perPoint, 64));
  };
  if (headInLiquid) {
    refillAcc_ = 0;
    if (vitals.get(pid, vitals_.oxygen) > 0) {
      drownAcc_ = 0;
      breathAcc_ += ticks;
      if (const int32_t drain = points(breathAcc_, kOxygenDrainTicks)) {
        breathAcc_ %= kOxygenDrainTicks;
        p.adjustVital(vitals_.oxygen, -drain);
      }
    } else {
      breathAcc_ = 0;
      drownAcc_ += ticks;
      if (const int32_t dmg = points(drownAcc_, kDrownTicks)) {
        drownAcc_ %= kDrownTicks;
        p.adjustVital(vitals_.health, -dmg);
      }
    }
  } else {
    breathAcc_ = 0;
    drownAcc_ = 0;
    refillAcc_ += ticks;
    if (const int32_t refill = points(refillAcc_, kOxygenRefillTicks)) {
      refillAcc_ %= kOxygenRefillTicks;
      p.adjustVital(vitals_.oxygen, refill);
    }
  }
  // Stamina: drain while sprinting, regen otherwise. Mode flips restart the
  // pacing so a partial point never carries across.
  if (sprinting != staminaDraining_) {
    staminaDraining_ = sprinting;
    staminaAcc_ = 0;
  }
  staminaAcc_ += ticks;
  if (sprinting) {
    if (const int32_t drain = points(staminaAcc_, kStaminaDrainTicks)) {
      staminaAcc_ %= kStaminaDrainTicks;
      p.adjustVital(vitals_.stamina, -drain);
    }
  } else {
    if (const int32_t regen = points(staminaAcc_, kStaminaRegenTicks)) {
      staminaAcc_ %= kStaminaRegenTicks;
      p.adjustVital(vitals_.stamina, regen);
    }
  }
}

}  // namespace lithokraft
