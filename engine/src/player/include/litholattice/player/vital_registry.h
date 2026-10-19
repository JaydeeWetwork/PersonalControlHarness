// VitalRegistry: the concrete IVitals. One registry serves every player;
// per-player data is keyed by player id in ordered maps, so iteration (and
// therefore serialization) is deterministic by (player, vital id).
// Mirrors StatRegistry's patterns minus the boost layer (see vital.h).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "litholattice/player/vital.h"

namespace litholattice {

class VitalRegistry final : public IVitals {
 public:
  /// Fired synchronously whenever a player's vital value changes (set /
  /// adjust). Not fired when the value is unchanged, so listeners can
  /// forward straight to UI/network without deduping.
  using ChangeFn = std::function<void(uint64_t playerId, uint32_t vitalId, int32_t oldValue,
                                      int32_t newValue)>;
  void setChangeCallback(ChangeFn fn) { changeFn_ = std::move(fn); }

  uint32_t registerVital(const VitalDef& def) override;
  int32_t get(uint64_t playerId, uint32_t vitalId) const override;
  void set(uint64_t playerId, uint32_t vitalId, int32_t value) override;
  void adjust(uint64_t playerId, uint32_t vitalId, int32_t delta) override;
  void removePlayer(uint64_t playerId) override;
  uint32_t findVital(std::string_view name) const override;
  const VitalDef* vitalDef(uint32_t vitalId) const override;

  size_t vitalCount() const { return defs_.size(); }
  /// Ids of every vital this player has a stored value for, ascending.
  std::vector<uint32_t> touchedVitals(uint64_t playerId) const;

  /// Vital-block serialization: [u32 count][count x {u32 vitalId, i32 value}]
  /// in vital-id order — same shape as StatRegistry's stat block. Returns
  /// bytes written, or 0 when maxLen is too small (nothing is written then).
  uint32_t serializeVitals(uint64_t playerId, uint8_t* out, uint32_t maxLen) const;
  /// Inverse of serializeVitals. Replaces the player's stored values
  /// (unknown vital ids are skipped for forward tolerance; values clamp to
  /// their def's range). Fail-closed: the whole block is validated before
  /// anything is applied, so a truncated block changes nothing. Returns
  /// bytes read, or 0 on error. Does not fire the change callback (games
  /// call notifyStateChanged after a load).
  uint32_t deserializeVitals(uint64_t playerId, const uint8_t* in, uint32_t len);

 private:
  struct OwnedDef {
    VitalDef def;       ///< def.name points into `name` below
    std::string name;   ///< owned copy: callers may pass temporary strings
  };
  int32_t valueOrDefault(uint64_t playerId, uint32_t vitalId, const VitalDef& def) const;

  std::map<uint32_t, OwnedDef> defs_;        // vital id -> def (id order)
  std::map<std::string, uint32_t> byName_;   // name -> vital id
  uint32_t nextId_ = 1;                      // 0 is the "no vital" sentinel
  // playerId -> vitalId -> value (only vitals ever written)
  std::map<uint64_t, std::map<uint32_t, int32_t>> values_;
  ChangeFn changeFn_;
};

}  // namespace litholattice
