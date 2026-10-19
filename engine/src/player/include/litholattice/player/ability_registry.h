// AbilityRegistry: the concrete IAbility. Applies boosts through the IStat
// it was constructed with, using the ability id as the boost source key —
// one active instance per (player, ability) is the binary model, so the
// source key can never collide with another ability's contribution.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>

#include "litholattice/player/ability.h"
#include "litholattice/player/stat.h"

namespace litholattice {

class AbilityRegistry final : public IAbility {
 public:
  /// Fired synchronously on activate / deactivate / expiry (not on grant).
  using ChangeFn = std::function<void(uint64_t playerId, uint32_t abilityId, bool active)>;

  explicit AbilityRegistry(IStat& stats) : stats_(stats) {}
  void setChangeCallback(ChangeFn fn) { changeFn_ = std::move(fn); }

  uint32_t registerAbility(const AbilityDef& def) override;
  void grant(uint64_t playerId, uint32_t abilityId) override;
  void revoke(uint64_t playerId, uint32_t abilityId) override;
  bool activate(uint64_t playerId, uint32_t abilityId) override;
  void deactivate(uint64_t playerId, uint32_t abilityId) override;
  bool isActive(uint64_t playerId, uint32_t abilityId) const override;
  bool isGranted(uint64_t playerId, uint32_t abilityId) const override;
  void tick(uint32_t dtTicks) override;
  void removePlayer(uint64_t playerId) override;
  uint32_t findAbility(std::string_view name) const override;
  const AbilityDef* abilityDef(uint32_t abilityId) const override;

  size_t abilityCount() const { return defs_.size(); }
  /// Engine ticks elapsed since this registry was created (ability time).
  uint64_t nowTicks() const { return now_; }
  /// Ticks until the player can activate again; 0 when ready (or unknown).
  uint32_t cooldownRemaining(uint64_t playerId, uint32_t abilityId) const;

 private:
  struct OwnedDef {
    AbilityDef def;     ///< def.name points into `name` below
    std::string name;
  };
  struct ActiveState {
    uint64_t expiryTick = 0;  ///< tick at which it expires (timed only)
    bool permanent = true;    ///< durationTicks == 0 at activation
  };
  // Ends an active ability: removes the boost, records the cooldown, fires
  // the change callback. No-op when not active.
  void endActive(uint64_t playerId, uint32_t abilityId, bool startCooldown);

  IStat& stats_;
  std::map<uint32_t, OwnedDef> defs_;
  std::map<std::string, uint32_t> byName_;
  uint32_t nextId_ = 1;  // 0 is the "no ability" sentinel
  uint64_t now_ = 0;     ///< ability time, advanced only by tick()
  std::map<uint64_t, std::set<uint32_t>> granted_;                 // player -> abilities
  std::map<uint64_t, std::map<uint32_t, ActiveState>> active_;     // player -> ability -> state
  std::map<uint64_t, std::map<uint32_t, uint64_t>> cooldownUntil_; // player -> ability -> tick
  ChangeFn changeFn_;
};

}  // namespace litholattice
