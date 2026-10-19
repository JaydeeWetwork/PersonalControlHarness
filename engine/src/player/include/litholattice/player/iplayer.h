// IPlayer: the engine-level per-player interface.
//
// One IPlayer is one participant (human, remote peer, or agent pilot)
// as the ENGINE sees them: identity, the synced PlayerState (position /
// velocity / look / dimension), condition pools through IVitals, custom
// stats through IStat, and active verbs through IAbility. Games layer
// their own data on top (inventory, gear, LithoKraft's Kharge-driven
// systems) — the engine never needs to know about it.
//
// NOTIFICATIONS: IPlayerListener is the change hook UI and network sync
// consume. Vital, stat, and ability changes fire it automatically (routed
// through the shared registries by PlayerManager, or fired directly by the
// concrete Player's own mutators when no manager routes them). PlayerState
// is a plain struct, so direct state() edits cannot be intercepted — after
// editing state, call notifyStateChanged(). Listeners are borrowed
// pointers: removeListener() before the listener dies.
#pragma once

#include <cstdint>
#include <string>

#include "litholattice/player/ability.h"
#include "litholattice/player/player_state.h"
#include "litholattice/player/stat.h"
#include "litholattice/player/vital.h"

namespace litholattice {

class IPlayer;

/// Change notifications for one player. All methods default to no-ops so a
/// listener overrides only what it consumes.
class IPlayerListener {
 public:
  virtual ~IPlayerListener() = default;
  /// A vital's value changed (old != new guaranteed).
  virtual void onVitalChanged(IPlayer& /*player*/, uint32_t /*vitalId*/, int32_t /*oldValue*/,
                              int32_t /*newValue*/) {}
  /// A stat's EFFECTIVE value changed (old != new guaranteed).
  virtual void onStatChanged(IPlayer& /*player*/, uint32_t /*statId*/, int32_t /*oldValue*/,
                             int32_t /*newValue*/) {}
  /// An ability became active or inactive (activation, deactivation, expiry).
  virtual void onAbilityChanged(IPlayer& /*player*/, uint32_t /*abilityId*/, bool /*active*/) {}
  /// state() was edited and notifyStateChanged() was called.
  virtual void onPlayerStateChanged(IPlayer& /*player*/) {}
};

class IPlayer {
 public:
  virtual ~IPlayer() = default;

  virtual uint64_t playerId() const = 0;
  virtual const std::string& displayName() const = 0;
  virtual void setDisplayName(std::string name) = 0;

  /// The synced movement/presence state. Edit freely, then call
  /// notifyStateChanged() so listeners (and net sync) hear about it.
  virtual PlayerState& state() = 0;
  virtual const PlayerState& state() const = 0;

  /// Velocity in blocks/second (PlayerState::vx/vy/vz; the entity physics
  /// convention — position integrates velocity * dt). First-class on the
  /// interface because consumers key off it directly: input/controls write
  /// it, the camera system reads it (follow lag, speed-based FOV kick),
  /// animation reads it, and it serializes with the rest of PlayerState for
  /// net sync. Default implementations ride on state(); setVelocity()
  /// fires notifyStateChanged(). Implementations may override.
  virtual void velocity(double& vx, double& vy, double& vz) const {
    const PlayerState& s = state();
    vx = s.vx;
    vy = s.vy;
    vz = s.vz;
  }
  virtual void setVelocity(double vx, double vy, double vz) {
    PlayerState& s = state();
    s.vx = vx;
    s.vy = vy;
    s.vz = vz;
    notifyStateChanged();
  }

  /// The shared registries this player is keyed into (same instances for
  /// every player; per-player data lives inside, keyed by playerId()).
  virtual IVitals& vitals() = 0;
  virtual const IVitals& vitals() const = 0;
  virtual IStat& stats() = 0;
  virtual const IStat& stats() const = 0;
  virtual IAbility& abilities() = 0;
  virtual const IAbility& abilities() const = 0;

  /// Current value of a vital for this player (clamped to its def range).
  virtual int32_t vital(uint32_t vitalId) const = 0;
  /// Set/adjust this player's vital value (fires onVitalChanged when the
  /// value moves).
  virtual void setVital(uint32_t vitalId, int32_t value) = 0;
  virtual void adjustVital(uint32_t vitalId, int32_t delta) = 0;

  /// Effective value of a stat for this player (base + boosts, clamped).
  virtual int32_t stat(uint32_t statId) const = 0;
  /// Set/adjust this player's BASE stat value (fires onStatChanged when
  /// the effective value moves).
  virtual void setStat(uint32_t statId, int32_t value) = 0;
  virtual void adjustStat(uint32_t statId, int32_t delta) = 0;

  /// Alive is derived, never stored: health (the standard vital, when
  /// registered) above its minimum; falls back to PlayerState::health > 0
  /// when no health vital exists.
  virtual bool alive() const = 0;

  virtual void addListener(IPlayerListener* listener) = 0;
  virtual void removeListener(IPlayerListener* listener) = 0;
  /// Fire onPlayerStateChanged on all listeners. Call after editing state().
  virtual void notifyStateChanged() = 0;
};

}  // namespace litholattice
