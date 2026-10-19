// StatRegistry: the concrete IStat. One registry serves every player;
// per-player data is keyed by player id in ordered maps, so iteration (and
// therefore serialization) is deterministic by (player, stat id).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "litholattice/player/stat.h"

namespace litholattice {

class StatRegistry final : public IStat {
 public:
  /// Fired synchronously whenever a player's EFFECTIVE value changes,
  /// whatever the cause (set/adjust/boost add/boost remove/expiry-driven
  /// boost removal). Not fired when the effective value is unchanged, so
  /// listeners can forward straight to UI/network without deduping.
  using ChangeFn = std::function<void(uint64_t playerId, uint32_t statId, int32_t oldEffective,
                                      int32_t newEffective)>;
  void setChangeCallback(ChangeFn fn) { changeFn_ = std::move(fn); }

  uint32_t registerStat(const StatDef& def) override;
  int32_t get(uint64_t playerId, uint32_t statId) const override;
  int32_t getBase(uint64_t playerId, uint32_t statId) const override;
  void set(uint64_t playerId, uint32_t statId, int32_t value) override;
  void adjust(uint64_t playerId, uint32_t statId, int32_t delta) override;
  void addBoost(uint64_t playerId, uint32_t statId, int32_t amount, uint64_t source) override;
  void removeBoost(uint64_t playerId, uint32_t statId, uint64_t source) override;
  void removePlayer(uint64_t playerId) override;
  uint32_t findStat(std::string_view name) const override;
  const StatDef* statDef(uint32_t statId) const override;

  size_t statCount() const { return defs_.size(); }
  /// Ids of every stat this player has a stored base value for, ascending.
  std::vector<uint32_t> touchedStats(uint64_t playerId) const;

  /// Stat-block serialization: [u32 count][count x {u32 statId, i32 base}]
  /// in stat-id order. BASE values only — boosts are transient by design
  /// (an ability active at save time re-applies its boost on reactivation,
  /// and serializing boosts would double-apply them). Returns bytes written,
  /// or 0 when maxLen is too small (nothing is written then).
  uint32_t serializeStats(uint64_t playerId, uint8_t* out, uint32_t maxLen) const;
  /// Inverse of serializeStats. Replaces the player's stored base values
  /// (unknown stat ids are skipped for forward tolerance; values clamp to
  /// their def's range). Fail-closed: the whole block is validated before
  /// anything is applied, so a truncated block changes nothing. Returns
  /// bytes read, or 0 on error.
  uint32_t deserializeStats(uint64_t playerId, const uint8_t* in, uint32_t len);

 private:
  struct OwnedDef {
    StatDef def;        ///< def.name points into `name` below
    std::string name;   ///< owned copy: callers may pass temporary strings
  };
  int32_t baseOrDefault(uint64_t playerId, uint32_t statId, const StatDef& def) const;
  int32_t effective(uint64_t playerId, uint32_t statId, const StatDef& def) const;
  void fireIfChanged(uint64_t playerId, uint32_t statId, int32_t oldEffective);

  std::map<uint32_t, OwnedDef> defs_;        // stat id -> def (id order)
  std::map<std::string, uint32_t> byName_;   // name -> stat id
  uint32_t nextId_ = 1;                      // 0 is the "no stat" sentinel
  // playerId -> statId -> base value (only stats ever written)
  std::map<uint64_t, std::map<uint32_t, int32_t>> base_;
  // playerId -> statId -> (source -> amount)
  std::map<uint64_t, std::map<uint32_t, std::map<uint64_t, int32_t>>> boosts_;
  ChangeFn changeFn_;
};

}  // namespace litholattice
