// LithoLattice player state: engine-side, serializable for multiplayer.
// The game extends this with inventory, gear, etc. (via the sync hook).
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "litholattice/player/ability_registry.h"
#include "litholattice/player/stat_registry.h"
#include "litholattice/player/vital_registry.h"

namespace litholattice {

class Player;  // concrete IPlayer (player.h); the manager owns instances

/// Player state, synced in multiplayer. Powers of two where it makes sense.
struct PlayerState {
  uint64_t playerId = 0;  ///< unique per player
  double x = 0, y = 0, z = 0;      ///< position
  double vx = 0, vy = 0, vz = 0;   ///< velocity
  float yaw = 0, pitch = 0;        ///< look direction
  uint8_t dimension = 1;           ///< current dimension (1 = Wafer)
  uint8_t health = 64;             ///< 0-64 (the 64 theme; 0 = dead)

  /// Fixed wire size of serialize()/deserialize(). The layout is explicit
  /// little-endian (never a struct memcpy), so it is stable across
  /// platforms and compilers. Trailing bytes beyond kSerializedSize are
  /// ignored on read: future fields append without breaking old readers.
  static constexpr uint32_t kSerializedSize = 68;
  /// Serializes to a byte buffer (for network). Returns bytes written
  /// (kSerializedSize), or 0 when maxLen is too small (nothing written).
  uint32_t serialize(uint8_t* out, uint32_t maxLen) const;
  /// Deserializes from a byte buffer. Returns bytes read (kSerializedSize),
  /// 0 on error (buffer too short — the state is left untouched).
  uint32_t deserialize(const uint8_t* in, uint32_t len);
};

/// Player manager: tracks all players (local + remote in multiplayer).
/// Owns the shared VitalRegistry/StatRegistry/AbilityRegistry and one
/// concrete Player per participant, and routes registry change callbacks
/// into each player's IPlayerListeners.
///
/// HEALTH MIRROR: PlayerState::health is the net-synced field; the health
/// VITAL is the engine's working value. The manager keeps them in step:
/// addOrUpdate() writes the synced health into the vital, and vital
/// changes write the health back into the state field — so either side
/// can be read and both agree.
class PlayerManager {
 public:
  PlayerManager();
  ~PlayerManager();
  PlayerManager(const PlayerManager&) = delete;
  PlayerManager& operator=(const PlayerManager&) = delete;

  /// The local player's state (created on first call, id = localId()).
  PlayerState& local();
  /// A player's last known state. Unknown ids return a shared empty state
  /// (playerId 0): check the id when it matters, or use player()/has().
  const PlayerState& get(uint64_t playerId) const;
  /// Insert or overwrite a player's state (from network / save load).
  /// Also creates the Player object when the id is new.
  void addOrUpdate(const PlayerState& state);
  /// Remove a player entirely: state, Player object, and every vital /
  /// stat / ability record keyed to them are purged.
  void remove(uint64_t playerId);

  // -- Extended API (the seam above is the original declaration) ----------
  VitalRegistry& vitals() { return vitals_; }
  const VitalRegistry& vitals() const { return vitals_; }
  StatRegistry& stats() { return stats_; }
  const StatRegistry& stats() const { return stats_; }
  AbilityRegistry& abilities() { return abilities_; }
  const AbilityRegistry& abilities() const { return abilities_; }

  /// The local player as an IPlayer (created on first call).
  Player& localPlayer();
  /// Add a player with a display name; returns the existing one when the
  /// id is already present (name updated). Id 0 is reserved ("nobody") and
  /// is mapped to a fresh id — check the returned playerId().
  Player& addPlayer(uint64_t playerId, std::string displayName);
  Player* player(uint64_t playerId);
  const Player* player(uint64_t playerId) const;
  bool has(uint64_t playerId) const { return players_.count(playerId) != 0; }
  size_t count() const { return players_.size(); }
  /// All player ids, ascending (deterministic iteration order).
  std::vector<uint64_t> playerIds() const;
  uint64_t localId() const { return localId_; }
  void setLocalId(uint64_t playerId) { localId_ = playerId; }
  /// Advance ability time (durations/cooldowns) by dtTicks engine ticks.
  void tick(uint32_t dtTicks);
  /// Fire onPlayerStateChanged for a player after direct state edits.
  void notifyStateChanged(uint64_t playerId);

 private:
  Player& ensurePlayer(uint64_t playerId);
  /// The health vital's id, resolved once and cached in vitalsHealth_
  /// (0 until the standard vitals are registered). PLY-4.
  uint32_t healthVitalId();
  void routeVitalChanged(uint64_t playerId, uint32_t vitalId, int32_t oldValue, int32_t newValue);
  void routeStatChanged(uint64_t playerId, uint32_t statId, int32_t oldValue, int32_t newValue);
  void routeAbilityChanged(uint64_t playerId, uint32_t abilityId, bool active);

  VitalRegistry vitals_;
  StatRegistry stats_;
  AbilityRegistry abilities_;
  std::map<uint64_t, std::unique_ptr<Player>> players_;  // id order
  uint64_t localId_ = 1;  // 0 is reserved; the first real player is 1
  uint32_t vitalsHealth_ = 0;  // health vital id once vitals are registered
};

}  // namespace litholattice
