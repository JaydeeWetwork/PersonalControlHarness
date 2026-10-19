// GamePlayerAdapter: presents LithoKraft's local player as an engine IPlayer.
//
// The game keeps its own Player (creative-flight camera + inventory, player.h).
// The engine's player system (litholattice/player: PlayerManager, IPlayer,
// the IVitals/IStat/IAbility peer registries) is the integration surface
// the rest of the engine speaks — MCP snapshots, saves, future net sync.
// This adapter owns the PlayerManager for the local player and mirrors the
// game player into it once per frame (syncFromGame), so engine consumers
// always read current state. The direction is game -> engine: gameplay
// movement stays in Player::update (MCP control also enters through the
// real input path), nothing engine-side moves the game player.
//
// Vitals vs stats: the constructor registers the engine's standard vitals
// on IVitals (health, oxygen, stamina — 0..64, default 64) plus, on IStat,
// the engine's standard stat "xp" (JD's ruling: XP is a stat, not a vital)
// and LithoKraft's own stats: "kharge" (0..64, default 0: accumulated
// charge for lithography, a resource the player builds up, so a fresh
// spawn starts empty rather than full — unlike the vitals, which start
// full) and "static" (0..INT32_MAX, default 0: LithoKraft's XP analogue,
// earned from mobs and activities and spent at the certificate table —
// JD design 2026-10-12. It gets the XP range, not the 64 pool scale,
// because it accumulates and is spent in XP-sized amounts).
//
// Threading: game thread only, like Player itself. Not thread-safe.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "litholattice/player/iplayer.h"         // IPlayer
#include "litholattice/player/player_state.h"    // PlayerManager, PlayerState
#include "litholattice/player/standard_stats.h"  // StandardStats
#include "litholattice/player/vitals.h"          // StandardVitals
#include "litholattice/world/block.h"          // BlockId
#include "lithokraft/game/player.h"

namespace lithokraft {

/// Eye height above the player's feet position. Matches the MCP senses
/// adapter's kEyeHeight (mcp_senses_adapter.cpp): Player::pos is the feet,
/// the senses camera sits this far above it.
inline constexpr float kPlayerEyeHeight = 1.62f;

/// The world position of the player's eyes; head-in-liquid tests sample the
/// block cell here. Uses the standing eye height (kPlayerEyeHeight).
Vec3 playerEyePos(const Player& p);
/// Pose-aware overload: the eye rides at the locomotion controller's
/// current eye height (Stand 1.62 / Sneak 1.27 / Crawl 0.4), so the
/// head-in-liquid cell follows the view when sneaking or crawling.
Vec3 playerEyePos(const Player& p, float eyeHeight);

/// True when `id` is a liquid cell for vitals purposes. Liquids are
/// game-defined (the worldgen <-> fluid seam in procgen/water.h identifies
/// water by block id / BlockDef behavior): a block counts as liquid when its
/// behavior tag is "water", "liquid" or "lava". LithoKraft registers no
/// liquid blocks yet, so today this is always false — the oxygen machinery
/// below is wired and live, and starts draining the day a liquid block lands.
bool blockIsLiquid(BlockId id);

class GamePlayerAdapter {
 public:
  /// Vitals pacing, in sim world ticks (32/s at the default TPS). One stat
  /// point per N ticks, so a full 64-point bar is 64*N ticks of budget.
  static constexpr uint64_t kOxygenDrainTicks = 8;    ///< under liquid: ~16 s of air
  static constexpr uint64_t kOxygenRefillTicks = 2;   ///< surfaced: full air in ~4 s
  static constexpr uint64_t kDrownTicks = 16;         ///< at 0 oxygen: 1 health / 0.5 s
  static constexpr uint64_t kStaminaDrainTicks = 16;  ///< sprinting: 1 point / 0.5 s
  static constexpr uint64_t kStaminaRegenTicks = 8;   ///< otherwise: 1 point / 0.25 s

  explicit GamePlayerAdapter(Player& gamePlayer);

  /// The engine view of the local player (id = the manager's local id).
  litholattice::IPlayer& enginePlayer();
  const litholattice::IPlayer& enginePlayer() const;
  litholattice::PlayerManager& manager() { return manager_; }
  const litholattice::PlayerManager& manager() const { return manager_; }

  /// Mirror the game player into the engine PlayerState (position, look,
  /// dimension, health field). Call once per frame, after the frame's
  /// movement. `dimensionNumber`: the MCP-style numeric dimension
  /// (1 = Wafer). `dtSeconds`: the frame's dt — the legacy form derives
  /// velocity from the movement actually applied since the last sync
  /// ((pos - prevPos) / dt); kept for init-time syncs before the
  /// locomotion body exists.
  void syncFromGame(uint8_t dimensionNumber, float dtSeconds);
  /// True-velocity form: the locomotion body IS the movement authority, so
  /// its velocity is mirrored verbatim instead of being re-derived from
  /// position deltas. The teleport guard still applies: a position delta
  /// implying > kTeleportSpeed blocks/s is a discontinuity (respawn,
  /// dimension switch, setPlayerPosition) and the reported velocity is 0
  /// for that sync instead of a one-frame spike.
  void syncFromGame(uint8_t dimensionNumber, float dtSeconds, const Vec3& trueVelocity);
  static constexpr float kTeleportSpeed = 512.f;  ///< blocks/s; real movement is < 12

  // -- Vitals (IVitals) and stats (IStat) ----------------------------------
  int32_t health() const;
  int32_t oxygen() const;
  int32_t stamina() const;
  int32_t kharge() const;  ///< the lithography charge stat (effective: base + boosts)
  int32_t xp() const;      ///< the engine standard stat (effective value)
  int32_t staticCharge() const;  ///< "static": the XP analogue spent at the certificate table
  bool alive() const;
  uint32_t khargeStatId() const { return kharge_; }
  uint32_t xpStatId() const { return standardStats_.xp; }
  uint32_t staticStatId() const { return static_; }
  const litholattice::StandardStats& standardStats() const { return standardStats_; }
  const litholattice::StandardVitals& vitals() const { return vitals_; }

  /// Every vital/stat worth showing a consumer: the standard vitals, xp,
  /// kharge, and static always, plus any other vital or stat this player
  /// has a stored value for. (name, value) pairs: vitals first, then
  /// stats, each in registration order. (The name predates the vitals/stats
  /// split; the MCP snapshot field it feeds is also called "stats".)
  std::vector<std::pair<std::string, double>> namedStats() const;

  // -- Save / load ----------------------------------------------------------
  /// Base values of every stat this player has touched, keyed by stat NAME
  /// (names are the permanent key: registry ids are registration-order and
  /// may shift as stats are added). Untouched stats are omitted — they read
  /// as their default on load. Vitals persist separately (vitalsForSave):
  /// health/oxygen/stamina are vitals now, not stats.
  std::map<std::string, int32_t> statsForSave() const;
  /// Values of every vital this player has a stored value for, keyed by
  /// vital NAME (same permanence reasoning as stats). Vitals have no boost
  /// layer, so the stored value is the whole story.
  std::map<std::string, int32_t> vitalsForSave() const;
  /// Restore saved stat base values by name. Unknown names are ignored (a
  /// stat removed since the save), missing stats keep their defaults,
  /// values clamp to the stat's range.
  void applySavedStats(const std::map<std::string, int32_t>& saved);
  /// Restore saved vital values by name; same tolerance rules as stats.
  void applySavedVitals(const std::map<std::string, int32_t>& saved);

  // -- Vitals simulation ----------------------------------------------------
  /// Advance the vitals by `ticks` sim world ticks at `targetTps`.
  /// Driven from Game::update (game thread) off the sim's world-tick
  /// counter, so vitals freeze when the sim pauses and never run on the
  /// sim thread itself.
  ///   headInLiquid: oxygen drains kOxygenDrainTicks/point; at 0 oxygen,
  ///     health drains kDrownTicks/point (drowning).
  ///   otherwise: oxygen refills kOxygenRefillTicks/point.
  ///   sprinting: stamina drains; otherwise stamina regens. Stamina never
  ///     gates movement yet — it is a live pool, enforcement is future work.
  /// Health does not regenerate here (no regen rule exists yet).
  ///
  /// TPS normalisation (P2b-2, review PLY-1): the rates below are
  /// calibrated in ticks at the reference rate kVitalsReferenceTps
  /// (32 TPS — "~16 s of air" is 512 ticks ONLY there). tickVitals
  /// converts the incoming world ticks to reference-rate ticks through a
  /// fixed sub-accumulator (ticks x 32 / targetTps, remainder carried),
  /// so wall-clock drain/refill is identical at every TPS and the
  /// 32 TPS calibration is bit-exact (the conversion is the identity
  /// there). targetTps <= 0 is treated as the reference rate.
  void tickVitals(uint64_t ticks, bool headInLiquid, bool sprinting, int targetTps);
  /// Reference-rate form (the pre-PLY-1 signature): ticks are already
  /// 32 Hz reference ticks. Kept for callers/tests at the calibrated rate.
  void tickVitals(uint64_t ticks, bool headInLiquid, bool sprinting) {
    tickVitals(ticks, headInLiquid, sprinting, int(kVitalsReferenceTps));
  }
  /// The rate the k*Ticks constants are calibrated at.
  static constexpr uint32_t kVitalsReferenceTps = 32;

 private:
  /// Shared mirror: state fields from the game player, velocity as given.
  void mirrorState(uint8_t dimensionNumber, double vx, double vy, double vz);
  /// True when the position delta since the last sync implies a teleport
  /// (see kTeleportSpeed); also refreshes the delta baseline.
  bool teleportTripped(float dtSeconds);

  Player& game_;
  litholattice::PlayerManager manager_;
  litholattice::StandardVitals vitals_;
  litholattice::StandardStats standardStats_;
  uint32_t kharge_ = 0;
  uint32_t static_ = 0;  ///< "static" stat id (the XP analogue; `static` is a keyword)
  // Velocity derivation: the game Player integrates position directly and
  // tracks no velocity, so syncFromGame differences consecutive positions.
  Vec3 prevPos_{};
  bool hasPrevPos_ = false;
  // Per-mode tick accumulators: rates stay exact over long runs (no
  // fractional-point loss), and switching modes restarts the current mode's
  // pacing instead of carrying a partial point across.
  uint64_t breathAcc_ = 0;   ///< ticks accumulated toward the next drained O2 point
  uint64_t refillAcc_ = 0;   ///< ticks accumulated toward the next refilled O2 point
  uint64_t drownAcc_ = 0;    ///< ticks accumulated toward the next drowning health point
  uint64_t staminaAcc_ = 0;  ///< ticks accumulated toward the next stamina point
  bool staminaDraining_ = false;  ///< last tick's stamina mode (acc resets on flip)
  // PLY-1 rate conversion: remainder of (ticks x kVitalsReferenceTps)
  // not yet emitted as whole reference ticks, in units of 1/targetTps
  // of a reference tick. Carried across calls so no fractional rate is
  // ever lost; the divisor (targetTps) is per-call, and a TPS change
  // simply re-scales future input against the carried remainder.
  uint64_t vitalsRateRem_ = 0;
};

}  // namespace lithokraft
