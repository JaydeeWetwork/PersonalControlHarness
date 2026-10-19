// Deceitor implementation. See deceitor.h for the design contract.
#include "lithokraft/game/deceitor.h"

#include <cmath>

#include "lithokraft/content/blocks.h"
#include "lithokraft/content/items.h"
#include "lithokraft/content/silica.h"  // kItemSilicaSeed

namespace lithokraft {

using namespace litholattice;

// ------------------------------------------------------------------ tables
namespace {

/// Liked items, BY STRING ID (JD, 2026-10-14: "money of some type,
/// gold-type items, and titanium oxide"). One-table edit when JD's
/// list lands: append the string id here. Live entries today:
///   * titanium oxide — registered with this work at JD's naming.
/// Placeholder slots (no such item exists in the registry yet — the
/// day one is registered, its row here is the whole change):
///   * money / currency — none exists (Static is a player STAT, not a
///     throwable item; nothing item-like to love yet).
///   * gold-type items/blocks — none registered yet.
constexpr std::string_view kDeceitorLikedStringIds[] = {
    "lithokraft:titanium_oxide",
    // "lithokraft:gold_ingot",   // JD: gold-type items go here when registered
    // "lithokraft:coin",         // JD: money/currency items go here when registered
};

std::string_view stringIdOf(uint32_t itemId) {
  if (const ItemDef* d = findItem(itemId)) return d->stringId;
  if (itemId > 0 && itemId < blockRegistry().size()) return blockRegistry().idOf(BlockId(itemId));
  return {};
}

}  // namespace

bool deceitorLikes(uint32_t itemId) {
  const std::string_view sid = stringIdOf(itemId);
  if (sid.empty()) return false;
  for (const std::string_view liked : kDeceitorLikedStringIds)
    if (sid == liked) return true;
  return false;
}

BarterReturn deceitorBarterReturn(bool liked, ai::AiRng& rng) {
  // Placeholder pools (JD-REVIEW): every entry is an EXISTING registry
  // item/block — nothing was invented for these tables. Ordinary offers
  // get plain Wafer materials; liked offers get the enthusiastic tier
  // (manufactured / rarer goods, larger counts). BlockIds are runtime
  // values (registry-assigned), so the pools resolve them per call.
  const size_t idx = size_t(rng.next() % 5);
  if (liked) {
    static const uint32_t kIds[] = {uint32_t(kChipOR), uint32_t(kLamp), uint32_t(kResonatorCrystal),
                                    uint32_t(kCapacitorCan), uint32_t(kGlass)};
    static constexpr uint16_t kCounts[] = {1, 2, 2, 2, 4};
    return {kIds[idx], kCounts[idx]};
  }
  static const uint32_t kIds[] = {uint32_t(kWaferSilicon), uint32_t(kDirt), uint32_t(kStone),
                                  uint32_t(kGlass), kItemSilicaSeed};
  static constexpr uint16_t kCounts[] = {2, 2, 1, 1, 3};
  return {kIds[idx], kCounts[idx]};
}

// ------------------------------------------------------------------ brain
DeceitorBrain::~DeceitorBrain() {
  if (registered_) dir_.detach(mobId_, this);
}

void DeceitorBrain::provoke(uint64_t aiTick) {
  if (mood_ == DeceitorMood::Hostile) return;
  mood_ = DeceitorMood::Provoked;
  provokedUntilTick_ = aiTick + DeceitorTuning::kFailedClickGraceTicks;
}

void DeceitorBrain::enrage() {
  mood_ = DeceitorMood::Hostile;
  hasPendingReturn_ = false;  // it keeps what it took
}

bool DeceitorBrain::unfed(uint64_t tick) const {
  const uint64_t base = everFed_ ? lastFedTick_ : spawnTick_;
  return tick >= base && tick - base > DeceitorTuning::kUnfedWindowTicks;
}

void DeceitorBrain::onBarter(ai::AiContext& ctx, uint32_t offeredItem) {
  everFed_ = true;
  lastFedTick_ = ctx.tick;
  mood_ = DeceitorMood::Neutral;  // diverts a provocation; de-aggros a betrayal
  pendingLiked_ = deceitorLikes(offeredItem);
  hasPendingReturn_ = true;
  pendingReturnTick_ = ctx.tick + (pendingLiked_ ? DeceitorTuning::kLikedReturnDelayTicks
                                                  : DeceitorTuning::kReturnDelayTicks);
}

ai::BtStatus DeceitorBrain::tick(ai::AiContext& ctx) {
  if (!registered_) {
    mobId_ = ctx.mob.id;
    spawnTick_ = ctx.tick;
    dir_.attach(mobId_, this);
    registered_ = true;
  }
  const DeceitorHooks& h = dir_.hooks();
  const double mx = ctx.mob.pos.x + 0.5, mz = ctx.mob.pos.z + 0.5;

  double px = 0, py = 0, pz = 0, fx = 0, fz = 0;
  const bool havePlayer = h.playerPose && h.playerPose(px, py, pz, fx, fz);
  double distPlayer = 1e30, dotFacing = 1.0;  // dotFacing = facing · dirToMob
  if (havePlayer) {
    const double dx = mx - px, dz = mz - pz;
    distPlayer = std::sqrt(dx * dx + dz * dz);
    if (distPlayer > 1e-6) dotFacing = (fx * dx + fz * dz) / distPlayer;
  }

  // 1) A due barter return: toss it toward the player.
  if (hasPendingReturn_ && ctx.tick >= pendingReturnTick_) {
    if (h.tossReturn) {
      const BarterReturn r = deceitorBarterReturn(pendingLiked_, ctx.rng);
      h.tossReturn(mx, ctx.mob.pos.y + 1.5, mz, r.itemId, r.count,
                   havePlayer ? px : mx, havePlayer ? py + 1.0 : ctx.mob.pos.y + 1.0,
                   havePlayer ? pz : mz);
    }
    hasPendingReturn_ = false;
    barterReadyTick_ = ctx.tick + DeceitorTuning::kBarterCooldownTicks;
  }

  // 2) Barter scan: walk to the nearest ground offer; take it in reach.
  //    Runs in every mood — a thrown item is also how a hostile
  //    Deceitor is fed back down (step 4 overwrites the goal with the
  //    chase only while still hostile). Suppressed during the
  //    post-return cooldown so it never re-takes its own return.
  if (h.nearestDrop && ctx.tick >= barterReadyTick_) {
    if (const auto drop = h.nearestDrop(mx, ctx.mob.pos.y + 0.5, mz, DeceitorTuning::kBarterRadius)) {
      const double dx = drop->x - mx, dz = drop->z - mz;
      const double d = std::sqrt(dx * dx + dz * dz);
      if (d <= DeceitorTuning::kConsumeRadius) {
        if (h.consumeDrop) h.consumeDrop(drop->entityId);
        onBarter(ctx, drop->itemId);
      } else {
        ctx.mob.goal = ai::Cell{int(std::floor(drop->x)), int(std::floor(drop->y)),
                                int(std::floor(drop->z))};
      }
    }
  }

  // 3) Failed-click grace expiry: no throw came — the mask drops.
  if (mood_ == DeceitorMood::Provoked && ctx.tick > provokedUntilTick_)
    mood_ = DeceitorMood::Hostile;

  // 4) Hostile: chase and hit. Neutral: the betrayal roll (unfed +
  //    close + back turned), through the mob's own seeded AiRng.
  if (mood_ == DeceitorMood::Hostile) {
    if (havePlayer) {
      // P2 (pathfinding crew, 2026-10-14): the chase goal is the player's
      // REAL cell -- previously the mob's own Y was substituted, so a
      // Deceitor could never follow a player up or down. Behaviour is
      // otherwise unchanged; with Leap (below) it can now act on the
      // height difference.
      ctx.mob.goal = ai::Cell{int(std::floor(px)), int(std::floor(py)), int(std::floor(pz))};
      if (distPlayer <= DeceitorTuning::kAttackRange && ctx.tick >= nextAttackTick_) {
        if (h.hurtPlayer) h.hurtPlayer(DeceitorTuning::kAttackDamage);
        nextAttackTick_ = ctx.tick + DeceitorTuning::kAttackCooldownTicks;
      }
    }
  } else if (mood_ == DeceitorMood::Neutral && havePlayer) {
    if (unfed(ctx.tick) && distPlayer <= DeceitorTuning::kBetrayalRadius &&
        dotFacing < DeceitorTuning::kBackTurnDot &&
        ctx.rng.unit() < DeceitorTuning::kBetrayalChancePerTick)
      mood_ = DeceitorMood::Hostile;
  }

  // 5) The face: identical to a Receiptor's while non-hostile — the
  //    smile is the shared, cordial mask; only open hostility scowls.
  if (mood_ == DeceitorMood::Hostile) {
    face_ = DeceitorFace::Scowl;
  } else if (havePlayer && distPlayer <= DeceitorTuning::kSmileRadius &&
             dotFacing > DeceitorTuning::kSmileFacingDot) {
    face_ = DeceitorFace::Smile;
  } else {
    face_ = DeceitorFace::Neutral;
  }
  ctx.lastAction = face_ == DeceitorFace::Scowl    ? "deceitor:scowl"
                   : face_ == DeceitorFace::Smile ? "deceitor:smile"
                                                  : "deceitor:idle";
  return ai::BtStatus::Success;
}

// ------------------------------------------------------------------ director
// P2 (pathfinding crew, 2026-10-14): Deceitors LEAP -- the director's
// locomotion mode is the engine's Leap mode (diagonals, jump-up 2, gap
// jumps, long drops; a strict superset of Walk's edges at the same base
// costs), per JD's 2026-10-14 direction that mobs be "a little bit more
// tricky to deal with" than Minecraft's. The member keeps its name: it is
// still "the walking mode" slot, now with the fuller vocabulary.
DeceitorDirector::DeceitorDirector() : walk_(ai::makeLeap()) {}
DeceitorDirector::~DeceitorDirector() = default;

std::optional<uint32_t> DeceitorDirector::spawn(ai::AiWorld& world, ai::Cell at) {
  return world.spawn(at, walk_.get(), std::make_unique<DeceitorBrain>(*this));
}

void DeceitorDirector::attach(uint32_t mobId, DeceitorBrain* brain) { brains_[mobId] = brain; }
void DeceitorDirector::detach(uint32_t mobId, DeceitorBrain* brain) {
  const auto it = brains_.find(mobId);
  if (it != brains_.end() && it->second == brain) brains_.erase(it);
}
DeceitorBrain* DeceitorDirector::brain(uint32_t mobId) {
  const auto it = brains_.find(mobId);
  return it == brains_.end() ? nullptr : it->second;
}
const DeceitorBrain* DeceitorDirector::brain(uint32_t mobId) const {
  const auto it = brains_.find(mobId);
  return it == brains_.end() ? nullptr : it->second;
}
std::vector<uint32_t> DeceitorDirector::mobIds() const {
  std::vector<uint32_t> ids;
  ids.reserve(brains_.size());
  for (const auto& [id, b] : brains_) ids.push_back(id);
  return ids;
}

bool DeceitorDirector::playerInteract(uint32_t mobId, uint64_t aiTick) {
  if (DeceitorBrain* b = brain(mobId)) b->provoke(aiTick);
  return false;  // a Deceitor has NO trading interface — that is the tell
}

void DeceitorDirector::playerAttacked(ai::AiWorld& world, uint32_t mobId) {
  const ai::Mob* victim = world.mob(mobId);
  if (!victim) return;
  const ai::Cell v = victim->pos;
  for (const auto id : mobIds()) {
    DeceitorBrain* b = brain(id);
    const ai::Mob* m = world.mob(id);
    if (!b || !m) continue;
    const double dx = double(m->pos.x - v.x), dz = double(m->pos.z - v.z);
    if (std::sqrt(dx * dx + dz * dz) <= DeceitorTuning::kPackRadius) b->enrage();
  }
}

DeceitorFace DeceitorDirector::faceOf(uint32_t mobId) const {
  const DeceitorBrain* b = brain(mobId);
  return b ? b->face() : DeceitorFace::Neutral;
}

}  // namespace lithokraft
