// Player: the concrete IPlayer. Owns the PlayerState and display name;
// borrows the shared VitalRegistry/StatRegistry/AbilityRegistry it is
// keyed into.
//
// Notification routing: when a PlayerManager owns this player it installs
// the registries' change callbacks and routes them here (attachRouting()).
// A standalone Player has no routing, so its own mutators fire listeners
// directly — the two paths are mutually exclusive (routed players never
// direct-fire), so a listener never hears the same change twice.
#pragma once

#include <string>
#include <vector>

#include "litholattice/player/ability_registry.h"
#include "litholattice/player/iplayer.h"
#include "litholattice/player/stat_registry.h"
#include "litholattice/player/vital_registry.h"

namespace litholattice {

class Player final : public IPlayer {
 public:
  Player(uint64_t playerId, std::string displayName, VitalRegistry& vitals, StatRegistry& stats,
         AbilityRegistry& abilities);

  uint64_t playerId() const override { return state_.playerId; }
  const std::string& displayName() const override { return name_; }
  void setDisplayName(std::string name) override { name_ = std::move(name); }

  PlayerState& state() override { return state_; }
  const PlayerState& state() const override { return state_; }
  // velocity()/setVelocity(): IPlayer defaults ride on state() — no override.

  IVitals& vitals() override { return vitals_; }
  const IVitals& vitals() const override { return vitals_; }
  IStat& stats() override { return stats_; }
  const IStat& stats() const override { return stats_; }
  IAbility& abilities() override { return abilities_; }
  const IAbility& abilities() const override { return abilities_; }

  int32_t vital(uint32_t vitalId) const override { return vitals_.get(state_.playerId, vitalId); }
  void setVital(uint32_t vitalId, int32_t value) override;
  void adjustVital(uint32_t vitalId, int32_t delta) override;

  int32_t stat(uint32_t statId) const override { return stats_.get(state_.playerId, statId); }
  void setStat(uint32_t statId, int32_t value) override;
  void adjustStat(uint32_t statId, int32_t delta) override;

  bool alive() const override;

  void addListener(IPlayerListener* listener) override;
  void removeListener(IPlayerListener* listener) override;
  void notifyStateChanged() override;

  // -- Routing plumbing (PlayerManager calls these; games should not) -----
  /// Mark this player as registry-routed by a manager: its listeners are
  /// then fired by the manager's routing, not by its own mutators.
  void attachRouting() { routed_ = true; }
  void dispatchVitalChanged(uint32_t vitalId, int32_t oldValue, int32_t newValue);
  void dispatchStatChanged(uint32_t statId, int32_t oldValue, int32_t newValue);
  void dispatchAbilityChanged(uint32_t abilityId, bool active);

 private:
  PlayerState state_;
  std::string name_;
  VitalRegistry& vitals_;
  StatRegistry& stats_;
  AbilityRegistry& abilities_;
  std::vector<IPlayerListener*> listeners_;
  bool routed_ = false;
};

}  // namespace litholattice
