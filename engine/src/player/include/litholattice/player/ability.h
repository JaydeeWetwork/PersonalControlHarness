// LithoLattice abilities: active verbs that boost stats.
// Stats are passive values. Abilities are binary (have/don't, active/inactive)
// actions that modify stats through IStat boosts (see stat.h's value model:
// a boost is a source-keyed contribution to the effective value, never folded
// into the base — deactivation removes exactly what activation added).
//
// TIME: abilities run on ENGINE TICKS, never wall clock. The game calls
// tick(dtTicks) once per sim step (the engine ticks at 512 TPS). Durations
// and cooldowns are counted in those ticks, so replays and saves stay
// deterministic.
#pragma once
#include <cstdint>
#include <string_view>

namespace litholattice {

/// An ability definition: id, name, which stat it boosts and by how much.
struct AbilityDef {
  uint32_t id = 0;
  const char* name = nullptr;  ///< e.g. "Sprintor" (-OR naming)
  uint32_t targetStatId = 0;   ///< which IStat this boosts
  int32_t boostAmount = 0;     ///< added to the stat while active
  uint32_t durationTicks = 0;  ///< 0 = permanent (passive ability)
  uint32_t cooldownTicks = 0;  ///< ticks before reuse (0 = none)
};

/// Ability system: binary state per player (active/inactive).
class IAbility {
 public:
  virtual ~IAbility() = default;
  /// Register a new ability. Returns the ability ID (0 on failure).
  /// First-wins by name, same rule as IStat::registerStat.
  virtual uint32_t registerAbility(const AbilityDef& def) = 0;
  /// Grant an ability to a player (they "have" it). Unknown ids are ignored.
  virtual void grant(uint64_t playerId, uint32_t abilityId) = 0;
  /// Revoke an ability: removes the grant and deactivates it if active.
  virtual void revoke(uint64_t playerId, uint32_t abilityId) = 0;
  /// Activate an ability (binary: on). Applies the boost through IStat.
  /// Returns false when the player does not have the ability, the ability
  /// is on cooldown, or its target stat is not registered. Re-activating an
  /// already-active ability refreshes its duration and succeeds.
  virtual bool activate(uint64_t playerId, uint32_t abilityId) = 0;
  /// Deactivate (binary: off). Removes the boost and starts the cooldown.
  virtual void deactivate(uint64_t playerId, uint32_t abilityId) = 0;
  /// Is this ability currently active for the player?
  virtual bool isActive(uint64_t playerId, uint32_t abilityId) const = 0;
  /// Does the player have (was granted) this ability?
  virtual bool isGranted(uint64_t playerId, uint32_t abilityId) const = 0;
  /// Advance ability time by dtTicks engine ticks: timed abilities expire
  /// (boost removed, cooldown starts). Expiry order is deterministic
  /// (player id, then ability id).
  virtual void tick(uint32_t dtTicks) = 0;
  /// Purge every grant/active/cooldown for a player.
  virtual void removePlayer(uint64_t playerId) = 0;
  /// Look up a registered ability by name. Returns 0 when unknown.
  virtual uint32_t findAbility(std::string_view name) const = 0;
  /// The def for an ability id, or nullptr when unknown. Stable pointer,
  /// same lifetime rule as IStat::statDef.
  virtual const AbilityDef* abilityDef(uint32_t abilityId) const = 0;
};

}  // namespace litholattice
