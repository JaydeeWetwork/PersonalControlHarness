// The concrete Player (see player.h for the notification-routing design).
#include "litholattice/player/player.h"

#include <algorithm>

#include "litholattice/player/vitals.h"

namespace litholattice {

Player::Player(uint64_t playerId, std::string displayName, VitalRegistry& vitals,
               StatRegistry& stats, AbilityRegistry& abilities)
    : name_(std::move(displayName)), vitals_(vitals), stats_(stats), abilities_(abilities) {
  state_.playerId = playerId;
}

void Player::setVital(uint32_t vitalId, int32_t value) {
  // Same routing rule as setStat: routed players are notified by the
  // manager's registry routing; standalone players direct-fire, and only
  // when the value actually moved.
  const int32_t old = routed_ ? 0 : vitals_.get(state_.playerId, vitalId);
  vitals_.set(state_.playerId, vitalId, value);
  if (!routed_) {
    const int32_t now = vitals_.get(state_.playerId, vitalId);
    if (now != old)
      for (IPlayerListener* l : listeners_) l->onVitalChanged(*this, vitalId, old, now);
  }
}

void Player::adjustVital(uint32_t vitalId, int32_t delta) {
  const int32_t old = routed_ ? 0 : vitals_.get(state_.playerId, vitalId);
  vitals_.adjust(state_.playerId, vitalId, delta);
  if (!routed_) {
    const int32_t now = vitals_.get(state_.playerId, vitalId);
    if (now != old)
      for (IPlayerListener* l : listeners_) l->onVitalChanged(*this, vitalId, old, now);
  }
}

void Player::setStat(uint32_t statId, int32_t value) {
  // Routed players are notified by the manager's registry routing (which
  // fires inside stats_.set); only standalone players direct-fire, and only
  // when the effective value actually moved.
  const int32_t old = routed_ ? 0 : stats_.get(state_.playerId, statId);
  stats_.set(state_.playerId, statId, value);
  if (!routed_) {
    const int32_t now = stats_.get(state_.playerId, statId);
    if (now != old)
      for (IPlayerListener* l : listeners_) l->onStatChanged(*this, statId, old, now);
  }
}

void Player::adjustStat(uint32_t statId, int32_t delta) {
  const int32_t old = routed_ ? 0 : stats_.get(state_.playerId, statId);
  stats_.adjust(state_.playerId, statId, delta);
  if (!routed_) {
    const int32_t now = stats_.get(state_.playerId, statId);
    if (now != old)
      for (IPlayerListener* l : listeners_) l->onStatChanged(*this, statId, old, now);
  }
}

bool Player::alive() const {
  // The health vital is authoritative when registered (registerStandardVitals
  // is the expected setup); before that, the synced state field decides.
  if (const uint32_t health = vitals_.findVital(kHealthVitalName))
    return vitals_.get(state_.playerId, health) > vitals_.vitalDef(health)->minValue;
  return state_.health > 0;
}

void Player::addListener(IPlayerListener* listener) {
  if (listener && std::find(listeners_.begin(), listeners_.end(), listener) == listeners_.end())
    listeners_.push_back(listener);
}

void Player::removeListener(IPlayerListener* listener) {
  listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener), listeners_.end());
}

void Player::notifyStateChanged() {
  for (IPlayerListener* l : listeners_) l->onPlayerStateChanged(*this);
}

void Player::dispatchVitalChanged(uint32_t vitalId, int32_t oldValue, int32_t newValue) {
  for (IPlayerListener* l : listeners_) l->onVitalChanged(*this, vitalId, oldValue, newValue);
}

void Player::dispatchStatChanged(uint32_t statId, int32_t oldValue, int32_t newValue) {
  for (IPlayerListener* l : listeners_) l->onStatChanged(*this, statId, oldValue, newValue);
}

void Player::dispatchAbilityChanged(uint32_t abilityId, bool active) {
  for (IPlayerListener* l : listeners_) l->onAbilityChanged(*this, abilityId, active);
}

}  // namespace litholattice
