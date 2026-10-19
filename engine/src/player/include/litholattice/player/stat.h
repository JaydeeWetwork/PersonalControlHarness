// LithoLattice stats: generic player stat system.
// The engine provides IStat (any tracked number: Kharge, a skill level).
// Games define specific stats (LithoKraft's Kharge is the first).
// Condition pools (health, oxygen, ...) are a separate system: vital.h.
// Abilities (active actions) are a separate system: ability.h.
//
// VALUE MODEL (read before touching): every (player, stat) pair has a BASE
// value — the durable number saves and the network carry — plus any number of
// transient BOOST contributions keyed by source. The EFFECTIVE value is
//     effective = clamp(base + sum(boosts), minValue, maxValue)
// set()/adjust() touch the base only; get() returns the effective value.
// Boosts are never folded into the base, so an ability expiring cannot leave
// the base corrupted (no drift) — world permanence depends on that split.
#pragma once
#include <cstdint>
#include <string_view>

namespace litholattice {

/// A generic player stat: id, value, min/max range.
struct StatDef {
  uint32_t id = 0;
  const char* name = nullptr;  ///< e.g. "kharge"
  int32_t minValue = 0;
  int32_t maxValue = 64;
  int32_t defaultValue = 0;
};

/// Stat values for a player.
class IStat {
 public:
  virtual ~IStat() = default;
  /// Get the EFFECTIVE stat value for a player (base + active boosts,
  /// clamped). Untouched stats read as their default value.
  virtual int32_t get(uint64_t playerId, uint32_t statId) const = 0;
  /// Get the BASE stat value (no boosts). This is what saves serialize.
  virtual int32_t getBase(uint64_t playerId, uint32_t statId) const = 0;
  /// Set the BASE value (clamped to min/max). Boosts are unaffected.
  virtual void set(uint64_t playerId, uint32_t statId, int32_t value) = 0;
  /// Adjust the BASE value by delta (clamped).
  virtual void adjust(uint64_t playerId, uint32_t statId, int32_t delta) = 0;
  /// Add (or replace) a boost contribution from `source` (an opaque key,
  /// e.g. an ability id). get() reflects it immediately.
  virtual void addBoost(uint64_t playerId, uint32_t statId, int32_t amount, uint64_t source) = 0;
  /// Remove the boost contribution from `source`, if any.
  virtual void removeBoost(uint64_t playerId, uint32_t statId, uint64_t source) = 0;
  /// Purge every value and boost for a player (player left the game).
  virtual void removePlayer(uint64_t playerId) = 0;
  /// Register a new stat type. Returns the stat ID (0 on failure).
  /// Registration is first-wins by name: re-registering an existing name
  /// returns the existing id and keeps the original def.
  virtual uint32_t registerStat(const StatDef& def) = 0;
  /// Look up a registered stat by name. Returns 0 when unknown.
  virtual uint32_t findStat(std::string_view name) const = 0;
  /// The def for a stat id, or nullptr when unknown. The pointer stays valid
  /// for the life of the registry (defs are never removed or renumbered).
  virtual const StatDef* statDef(uint32_t statId) const = 0;
};

}  // namespace litholattice
