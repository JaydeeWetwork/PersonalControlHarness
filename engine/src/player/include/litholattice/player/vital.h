// LithoLattice vitals: the player's condition pools (health, oxygen,
// stamina, and whatever a mod or a new dimension adds next).
//
// LAYERING: IStat, IAbility, and IVitals are peer foundational interfaces —
// they predate IPlayer, and IPlayer composes all three. Vitals are NOT a
// special case of stats: a stat is any tracked number (LithoKraft's Kharge,
// a skill level), while a vital is a condition pool with a floor, a ceiling,
// and a resting default — the numbers a HUD draws and survival systems
// drain and refill. Keeping vitals in their own registry is the modding
// extension point: a mod, a game, or a dimension the engine has never heard
// of registers its own vital by name (first-wins, like stats and abilities)
// and every IPlayer immediately carries it — no engine change, no stat
// namespace pollution.
//
// VALUE MODEL: one durable value per (player, vital), clamped to the def's
// [minValue, maxValue], untouched players read the def's default. There is
// deliberately no boost layer here (contrast stat.h): abilities boost
// STATS; systems that change a vital (drowning, healing, a dimension's
// environmental drain) adjust the value directly, so what saves and the
// network carry is always the whole story — world permanence depends on
// there being no transient layer to drift.
#pragma once
#include <cstdint>
#include <string_view>

namespace litholattice {

/// A vital definition: id, name, range, resting default.
struct VitalDef {
  uint32_t id = 0;
  const char* name = nullptr;  ///< e.g. "health", "oxygen"
  int32_t minValue = 0;
  int32_t maxValue = 64;
  int32_t defaultValue = 0;
};

/// Vital values for every player, plus the vital-type registry.
class IVitals {
 public:
  virtual ~IVitals() = default;
  /// Current value for a player (clamped). Untouched vitals read as their
  /// default value; unknown ids read 0.
  virtual int32_t get(uint64_t playerId, uint32_t vitalId) const = 0;
  /// Set the value (clamped to the def's min/max). Unknown ids are ignored.
  virtual void set(uint64_t playerId, uint32_t vitalId, int32_t value) = 0;
  /// Adjust the value by delta (clamped).
  virtual void adjust(uint64_t playerId, uint32_t vitalId, int32_t delta) = 0;
  /// Purge every value for a player (player left the game).
  virtual void removePlayer(uint64_t playerId) = 0;
  /// Register a new vital type. Returns the vital ID (0 on failure).
  /// Registration is first-wins by name: re-registering an existing name
  /// returns the existing id and keeps the original def. This is the call
  /// the player class, games, mods, and dimension code use to add vitals
  /// the engine did not ship with.
  virtual uint32_t registerVital(const VitalDef& def) = 0;
  /// Look up a registered vital by name. Returns 0 when unknown.
  virtual uint32_t findVital(std::string_view name) const = 0;
  /// The def for a vital id, or nullptr when unknown. The pointer stays
  /// valid for the life of the registry (defs are never removed or
  /// renumbered).
  virtual const VitalDef* vitalDef(uint32_t vitalId) const = 0;
};

}  // namespace litholattice
