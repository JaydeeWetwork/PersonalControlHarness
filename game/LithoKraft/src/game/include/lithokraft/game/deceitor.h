// Deceitor: LithoKraft's first mob content (JD design, 2026-10-14).
//
// A Deceitor is an upright biped that lives mixed among the honest
// villagers — the Receiptors — in Wafer settlements called Interchanges.
// It is VISUALLY IDENTICAL to a Receiptor (same biped presentation, same
// happy smile on approach): there is no visual tell. The tell is
// interaction — a Receiptor opens a trading interface; a Deceitor opens
// NOTHING.
//
// Behaviour (JD, 2026-10-14):
//   * Barter (piglin-style): throw/drop items on the ground near a
//     Deceitor and it walks over, takes the offer, and throws item(s)
//     back after a short beat. Offers it LIKES get the enthusiastic
//     tier (better returns, faster acceptance).
//   * Betrayal: while unfed (no successful barter inside the window)
//     and the player's back is turned, it rolls a periodic chance to
//     turn hostile and attack. Feeding it again de-aggros it.
//   * Failed click: interacting with a Deceitor opens no interface —
//     and the Deceitor turns hostile UNLESS the player immediately
//     throws it an item (grace window), which diverts it into barter.
//   * Pack retaliation: attacking one Deceitor turns every Deceitor
//     within the pack radius hostile, together ("gang style").
//
// ARCHITECTURE: the mob lives in the engine AiWorld (cell position,
// pathing, deterministic per-mob AiRng) exactly like the B6 AiHost
// wiring intended — content-free engine, game-owned behaviour. The
// brain is an ai::BtNode; everything game-side it needs (player pose,
// ground item entities, player damage) arrives through
// DeceitorDirector::Hooks, which Game fills from its EntityManager /
// player / adapter. The director also owns the registry the renderer
// reads (expression per mob) and the mirror-entity sync Game runs.
//
// SETTLEMENT CONVERSION (JD design, 2026-10-14 — FUTURE, not built):
// player conduct at an Interchange can convert it either way (feeding
// / freeing liberates toward Receiptor; hostile/exploitative conduct
// corrupts toward Deceitor), and redemption is never closed — liked-
// item barter is the path back even from a fully-Deceitor settlement.
// That layer will need population queries + a barter-event feed from
// this director; per-mob disposition therefore stays DATA on the
// brain (mood/fed), never a type-level constant, so nothing here
// blocks it.
//
// MIXTURE RULE (JD, 2026-10-14): populations run BOTH ways — a
// mostly-Receiptor interchange can hide Deceitors, and a
// mostly-Deceitor interchange can hold genuine Receiptors. No
// settlement is guaranteed pure. Spawning is per-mob here, so any
// mixture in either proportion is representable; the ratio itself is
// data for JD's interchange placement, not code.
//
// GATE: Deceitors exist only when spawned — via AiHost's AiWorld when
// `ai.enabled` is on (console command `deceitor spawn`, cheat), or by
// tests. Nothing spawns them in the default game, so the default
// scenario/determinism surface is untouched. Natural spawning at
// Interchange structures awaits JD's hand-built structures + a
// structure-tagging decision (JD, 2026-10-14).
//
// PERSISTENCE (honest): AiWorld mobs have no save path today (the B6
// wiring scoped lifecycle as JD's call), so Deceitors are session-only:
// no save-format change, nothing to migrate, nothing to corrupt.
//
// EVERY numeric default below is a placeholder flagged JD-REVIEW —
// none of them is a balance lock.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "litholattice/ai/ai_world.h"
#include "litholattice/ai/brain.h"
#include "litholattice/ai/nav.h"

namespace lithokraft {

// ---------------------------------------------------------------- tuning
// All values JD-REVIEW-PENDING placeholders (2026-10-14). AI ticks are
// the AiWorld's own (10 Hz at the engine default), not sim ticks.
struct DeceitorTuning {
  static constexpr double kBarterRadius = 4.0;      ///< scans ground drops within this (blocks)
  static constexpr double kConsumeRadius = 2.0;     ///< close enough to take an offer
  static constexpr uint64_t kUnfedWindowTicks = 600;  ///< 60 s at 10 Hz since last feed (or spawn)
  static constexpr float kBetrayalChancePerTick = 0.02f;  ///< per AI tick while unfed + back turned
  static constexpr double kBetrayalRadius = 12.0;   ///< betrayal only engages this close
  static constexpr double kBackTurnDot = 0.0;       ///< facing·toMob < this = back turned (front hemisphere)
  static constexpr double kSmileRadius = 6.0;       ///< the friendly face shows this close...
  static constexpr double kSmileFacingDot = 0.5;    ///< ...while the player faces it (dot > this)
  static constexpr uint64_t kReturnDelayTicks = 10;     ///< 1 s beat before an ordinary return toss
  static constexpr uint64_t kLikedReturnDelayTicks = 5;  ///< liked offer: half the beat
  static constexpr uint64_t kBarterCooldownTicks = 60;  ///< 6 s after a return toss: drops are
                                                        ///< ignored (it savours the trade — and
                                                        ///< never re-takes its own return)
  static constexpr uint64_t kFailedClickGraceTicks = 30;  ///< 3 s to throw an item after a failed click
  static constexpr double kPackRadius = 16.0;       ///< pack retaliation reach (blocks)
  static constexpr double kAttackRange = 1.8;       ///< melee reach (blocks, XZ)
  static constexpr int kAttackDamage = 4;           ///< of the 64-point health vital
  static constexpr uint64_t kAttackCooldownTicks = 10;  ///< 1 s between hits
};

// ---------------------------------------------------------------- tables
/// The liked-items table is BY STRING ID on purpose: JD's eventual list
/// ("money of some type, gold-type items, and titanium oxide", JD
/// 2026-10-14) is a one-table edit here. As of this build the registry
/// contains NO money/currency item and NO gold item (Static is a player
/// stat, not a throwable item), so the only live entry is titanium
/// oxide — the JD-named item registered with this work. When a gold or
/// currency item is registered, adding its string id below is the whole
/// change; deceitorLikes() resolves against items AND blocks.
bool deceitorLikes(uint32_t itemId);

/// One barter return: an existing-registry item id + count.
struct BarterReturn {
  uint32_t itemId = 0;
  uint16_t count = 0;
};
/// Pick a return from the ordinary pool, or the enthusiastic pool when
/// the offer was liked. Deterministic through the mob's AiRng.
BarterReturn deceitorBarterReturn(bool liked, litholattice::ai::AiRng& rng);

// ---------------------------------------------------------------- bridge
/// A ground drop as the brain sees it (an EntityManager kEntityItem).
struct DeceitorDrop {
  uint64_t entityId = 0;
  uint32_t itemId = 0;
  uint16_t count = 0;
  double x = 0, y = 0, z = 0;
};

/// Game-side services the brain needs. Game fills these from its live
/// state (EntityManager, player transform, player adapter); the
/// selftest fills them from fixtures. All calls happen on the game
/// thread, inside AiWorld::advance (brains tick there).
struct DeceitorHooks {
  /// Player eye/feet position (x,y,z = feet) + XZ facing (unit). False
  /// when no player pose is available (brain then idles safely).
  std::function<bool(double& x, double& y, double& z, double& fx, double& fz)> playerPose;
  /// Nearest ground drop within `radius` of (x, y, z) — nullopt when
  /// none. (x, y, z) is the mob's centre; the radius covers the floor
  /// the drop rests on.
  std::function<std::optional<DeceitorDrop>(double x, double y, double z, double radius)> nearestDrop;
  /// Remove a drop entity from the world (the offer is taken).
  std::function<void(uint64_t entityId)> consumeDrop;
  /// Spawn a return item at (x,y,z) tossed toward (tx,ty,tz).
  std::function<void(double x, double y, double z, uint32_t itemId, uint16_t count,
                     double tx, double ty, double tz)>
      tossReturn;
  /// Apply melee damage to the player (the 64-point health vital).
  std::function<void(int damage)> hurtPlayer;
};

// ---------------------------------------------------------------- brain
enum class DeceitorMood : uint8_t { Neutral, Provoked, Hostile };
enum class DeceitorFace : uint8_t { Neutral, Smile, Scowl };

class DeceitorDirector;

/// Behaviour-tree brain for one Deceitor (one instance per mob; the
/// AiWorld owns it). Registers itself with the director on first tick
/// and unregisters on destruction, so the director's registry is always
/// exactly the live population.
class DeceitorBrain final : public litholattice::ai::BtNode {
 public:
  explicit DeceitorBrain(DeceitorDirector& dir) : dir_(dir) {}
  ~DeceitorBrain() override;
  DeceitorBrain(const DeceitorBrain&) = delete;
  DeceitorBrain& operator=(const DeceitorBrain&) = delete;

  litholattice::ai::BtStatus tick(litholattice::ai::AiContext& ctx) override;

  // Read by the director / presentation / tests.
  DeceitorMood mood() const { return mood_; }
  DeceitorFace face() const { return face_; }
  bool everFed() const { return everFed_; }
  bool hasPendingReturn() const { return hasPendingReturn_; }
  uint32_t mobId() const { return mobId_; }

  /// Failed-click provocation (director entry): the grace window runs
  /// from `aiTick`; a completed barter before it lapses diverts into
  /// barter, otherwise the mob turns hostile at expiry.
  void provoke(uint64_t aiTick);
  /// Pack retaliation / direct enrage: hostile NOW, whatever the fed
  /// state. A pending return is cancelled — it keeps what it took.
  void enrage();

 private:
  void onBarter(litholattice::ai::AiContext& ctx, uint32_t offeredItem);
  bool unfed(uint64_t tick) const;

  DeceitorDirector& dir_;
  uint32_t mobId_ = 0;
  bool registered_ = false;
  DeceitorMood mood_ = DeceitorMood::Neutral;
  DeceitorFace face_ = DeceitorFace::Neutral;
  uint64_t spawnTick_ = 0;
  bool everFed_ = false;
  uint64_t lastFedTick_ = 0;
  uint64_t provokedUntilTick_ = 0;
  bool hasPendingReturn_ = false;
  uint64_t pendingReturnTick_ = 0;
  bool pendingLiked_ = false;
  uint64_t barterReadyTick_ = 0;  ///< drops ignored until this tick (post-return cooldown)
  uint64_t nextAttackTick_ = 0;
};

// ---------------------------------------------------------------- director
/// Owns the Deceitor population's game-side state: the shared walk
/// movement mode, the brain registry, the hooks, and the interaction
/// entry points (failed click / player attack) that the future
/// click-picking and mob-combat paths will call — today the `deceitor`
/// console command (cheat) and the selftest drive them.
class DeceitorDirector {
 public:
  DeceitorDirector();
  ~DeceitorDirector();
  DeceitorDirector(const DeceitorDirector&) = delete;
  DeceitorDirector& operator=(const DeceitorDirector&) = delete;

  void setHooks(DeceitorHooks hooks) { hooks_ = std::move(hooks); }
  const DeceitorHooks& hooks() const { return hooks_; }

  /// Spawn one Deceitor in `world` at cell `at` (feet cell). Nullopt at
  /// the AiWorld mob cap. The brain self-registers on its first tick.
  std::optional<uint32_t> spawn(litholattice::ai::AiWorld& world, litholattice::ai::Cell at);

  // Brain registry (attach/detach are called by DeceitorBrain only).
  void attach(uint32_t mobId, DeceitorBrain* brain);
  void detach(uint32_t mobId, DeceitorBrain* brain);
  DeceitorBrain* brain(uint32_t mobId);
  const DeceitorBrain* brain(uint32_t mobId) const;
  std::vector<uint32_t> mobIds() const;
  size_t population() const { return brains_.size(); }

  /// The interaction tell: a Receiptor would open its trading
  /// interface; a Deceitor NEVER does. This always reports "no
  /// interface" (returns false) and starts the failed-click grace
  /// window on that Deceitor. `aiTick` is the AiWorld's tick counter
  /// (AiWorld::stats().ticks).
  bool playerInteract(uint32_t mobId, uint64_t aiTick);

  /// The player attacked Deceitor `mobId`: it and every Deceitor
  /// within kPackRadius of it (mob positions from `world`) turn
  /// hostile together. (AiWorld mobs carry no health pool today, so
  /// this is an aggro event only — no damage is applied to the mob.)
  void playerAttacked(litholattice::ai::AiWorld& world, uint32_t mobId);

  /// Presentation: the face the renderer should show for `mobId`
  /// (Neutral when unknown). Deceitors present EXACTLY like Receiptors
  /// while non-hostile — the smile is shared; only the Scowl differs,
  /// and it appears only once hostility is open.
  DeceitorFace faceOf(uint32_t mobId) const;

 private:
  std::unique_ptr<litholattice::ai::IMovementMode> walk_;
  std::map<uint32_t, DeceitorBrain*> brains_;
  DeceitorHooks hooks_;
};

/// Self-test (app selftest section): barter ordinary + liked, betrayal
/// gating (unfed/back-turned/fed/facing), failed-click both branches,
/// pack retaliation. Returns failures.
int runDeceitorSelfTest();

}  // namespace lithokraft
