// AbilityRegistry implementation. Time advances only through tick(), so
// durations and cooldowns are pure functions of the tick sequence.
#include "litholattice/player/ability_registry.h"

#include <utility>
#include <vector>

namespace litholattice {

uint32_t AbilityRegistry::registerAbility(const AbilityDef& def) {
  if (!def.name || def.name[0] == '\0') return 0;
  if (const uint32_t existing = findAbility(def.name)) return existing;  // first-wins, as stats
  uint32_t id = def.id;
  if (id == 0) {
    id = nextId_;
  } else if (defs_.count(id)) {
    return 0;  // explicit id collision: fail closed
  }
  if (id >= nextId_) nextId_ = id + 1;
  OwnedDef owned;
  owned.name = def.name;
  owned.def = def;
  owned.def.id = id;
  owned.def.name = owned.name.c_str();
  defs_.emplace(id, std::move(owned));
  byName_[defs_[id].name] = id;
  return id;
}

uint32_t AbilityRegistry::findAbility(std::string_view name) const {
  const auto it = byName_.find(std::string(name));
  return it == byName_.end() ? 0 : it->second;
}

const AbilityDef* AbilityRegistry::abilityDef(uint32_t abilityId) const {
  const auto it = defs_.find(abilityId);
  return it == defs_.end() ? nullptr : &it->second.def;
}

void AbilityRegistry::grant(uint64_t playerId, uint32_t abilityId) {
  if (!abilityDef(abilityId)) return;  // unknown ability: fail closed, no record
  granted_[playerId].insert(abilityId);
}

void AbilityRegistry::revoke(uint64_t playerId, uint32_t abilityId) {
  endActive(playerId, abilityId, /*startCooldown=*/false);
  const auto git = granted_.find(playerId);
  if (git != granted_.end()) {
    git->second.erase(abilityId);
    if (git->second.empty()) granted_.erase(git);
  }
  const auto cit = cooldownUntil_.find(playerId);
  if (cit != cooldownUntil_.end()) {
    cit->second.erase(abilityId);
    if (cit->second.empty()) cooldownUntil_.erase(cit);
  }
}

bool AbilityRegistry::isGranted(uint64_t playerId, uint32_t abilityId) const {
  const auto it = granted_.find(playerId);
  return it != granted_.end() && it->second.count(abilityId) != 0;
}

bool AbilityRegistry::isActive(uint64_t playerId, uint32_t abilityId) const {
  const auto it = active_.find(playerId);
  return it != active_.end() && it->second.count(abilityId) != 0;
}

bool AbilityRegistry::activate(uint64_t playerId, uint32_t abilityId) {
  const AbilityDef* def = abilityDef(abilityId);
  if (!def || !isGranted(playerId, abilityId)) return false;
  if (!stats_.statDef(def->targetStatId)) return false;  // nowhere to put the boost
  const auto cit = cooldownUntil_.find(playerId);
  if (cit != cooldownUntil_.end()) {
    const auto it = cit->second.find(abilityId);
    if (it != cit->second.end() && now_ < it->second) return false;  // still cooling down
  }
  ActiveState st;
  st.permanent = def->durationTicks == 0;
  st.expiryTick = st.permanent ? 0 : now_ + def->durationTicks;
  const bool wasActive = isActive(playerId, abilityId);
  active_[playerId][abilityId] = st;
  // Source key = ability id. Re-activation replaces the contribution, so a
  // refresh can never double-stack its own boost.
  stats_.addBoost(playerId, def->targetStatId, def->boostAmount, abilityId);
  if (!wasActive && changeFn_) changeFn_(playerId, abilityId, true);
  return true;
}

void AbilityRegistry::endActive(uint64_t playerId, uint32_t abilityId, bool startCooldown) {
  const auto pit = active_.find(playerId);
  if (pit == active_.end() || !pit->second.count(abilityId)) return;
  pit->second.erase(abilityId);
  if (pit->second.empty()) active_.erase(pit);
  if (const AbilityDef* def = abilityDef(abilityId)) {
    stats_.removeBoost(playerId, def->targetStatId, abilityId);
    if (startCooldown && def->cooldownTicks > 0)
      cooldownUntil_[playerId][abilityId] = now_ + def->cooldownTicks;
  }
  if (changeFn_) changeFn_(playerId, abilityId, false);
}

void AbilityRegistry::deactivate(uint64_t playerId, uint32_t abilityId) {
  endActive(playerId, abilityId, /*startCooldown=*/true);
}

void AbilityRegistry::tick(uint32_t dtTicks) {
  now_ += dtTicks;
  // Collect first: expiring mutates active_ (and fires callbacks that could
  // re-enter the registry). Map order keeps expiry deterministic:
  // player id ascending, then ability id ascending.
  std::vector<std::pair<uint64_t, uint32_t>> expired;
  for (const auto& [playerId, abilities] : active_)
    for (const auto& [abilityId, st] : abilities)
      if (!st.permanent && st.expiryTick <= now_) expired.emplace_back(playerId, abilityId);
  for (const auto& [playerId, abilityId] : expired)
    endActive(playerId, abilityId, /*startCooldown=*/true);
}

void AbilityRegistry::removePlayer(uint64_t playerId) {
  // Remove boosts explicitly: just dropping active_ would leak the
  // contributions in the stat registry forever.
  const auto pit = active_.find(playerId);
  if (pit != active_.end()) {
    const auto snapshot = pit->second;
    for (const auto& [abilityId, st] : snapshot)
      if (const AbilityDef* def = abilityDef(abilityId))
        stats_.removeBoost(playerId, def->targetStatId, abilityId);
  }
  active_.erase(playerId);
  granted_.erase(playerId);
  cooldownUntil_.erase(playerId);
}

uint32_t AbilityRegistry::cooldownRemaining(uint64_t playerId, uint32_t abilityId) const {
  const auto pit = cooldownUntil_.find(playerId);
  if (pit == cooldownUntil_.end()) return 0;
  const auto it = pit->second.find(abilityId);
  if (it == pit->second.end() || it->second <= now_) return 0;
  return static_cast<uint32_t>(it->second - now_);
}

}  // namespace litholattice
