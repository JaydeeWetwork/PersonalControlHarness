// AiHost implementation. See ai_host.h for the wiring contract.
#include "lithokraft/game/ai_host.h"

#include <algorithm>

#include "lithokraft/content/blocks.h"  // SimRole, kGameClimbable, kGameFluid
#include "litholattice/core/log.h"

namespace lithokraft {

using namespace litholattice;

ai::NavRules gameNavRules(const BlockRegistry& blocks) {
  // P0 (2026-10-14): real classification from the game's own content data,
  // replacing the B6 defaults (solidity only). The predicates capture the
  // registry by reference; the registry outlives every WalkCache built
  // from these rules (game registry is process-lifetime; fixture
  // registries in tests outlive their hosts by construction).
  ai::NavRules rules;
  rules.solid = [&blocks](BlockId b) { return blocks.has(b, kSolid); };
  rules.track = [&blocks](BlockId b) { return roleOf(blocks.def(b)) == SimRole::Wire; };
  rules.climbable = [&blocks](BlockId b) { return (blocks.def(b).flags & kGameClimbable) != 0; };
  rules.fluid = [&blocks](BlockId b) { return (blocks.def(b).flags & kGameFluid) != 0; };
  return rules;
}

void AiHost::bindWorld(ChunkMap& chunks, const BlockRegistry& blocks, JobPool* pool,
                       ai::Cell centre) {
  unbindWorld();
  chunks_ = &chunks;
  blocks_ = &blocks;
  pool_ = pool;
  centre_ = centre;
  // P0 invalidation feed: every structural edit to the bound map posts its
  // column here (the listener runs on the mutating thread -- sim thread in
  // play, game thread at load). The listener is installed for the whole
  // bind lifetime, but while ai.enabled is off the sink drops every post
  // (B-H1): a fresh WalkCache computes from the live map at construct(),
  // so no pre-enable edit can ever be "missed". The queue is drained by
  // advance(), and cleared once more at construct() (below).
  chunks_->setEditListener([this](int cx, int cz) { postEditedColumn(cx, cz); });
  if (enabled_) construct();
}

void AiHost::unbindWorld() {
  if (chunks_) chunks_->setEditListener(nullptr);  // stop the feed before anything dies
  world_.reset();
  nav_.reset();
  chunks_ = nullptr;
  blocks_ = nullptr;
  pool_ = nullptr;
  observerSet_ = false;
  std::lock_guard l(invMutex_);
  pendingCols_.clear();
}

void AiHost::setEnabled(bool on) {
  if (on == enabled_.load()) return;
  enabled_.store(on);
  if (on) {
    construct();
  } else {
    world_.reset();
    nav_.reset();
    // Belt-and-braces (B-H1): anything posted in the enable/disable race
    // window is worthless now -- there is no cache to invalidate, and the
    // next construct() builds fresh from the live map regardless.
    std::lock_guard l(invMutex_);
    pendingCols_.clear();
  }
}

void AiHost::postEditedColumn(int cx, int cz) {
  // B-H1 primary fix: while the gate is off there is no WalkCache and
  // never will be one that could consume this post (construct() builds
  // fresh from the live map and discards the queue), so drop it here --
  // one atomic load on the mutating thread, no mutex, no growth.
  if (!enabled_.load()) return;
  const uint64_t key = (uint64_t(uint32_t(cx)) << 32) | uint32_t(cz);
  std::lock_guard l(invMutex_);
  pendingCols_.push_back(key);
}

void AiHost::drainInvalidations() {
  // B-H1 belt-and-braces: with no cache there is nothing to invalidate,
  // but the queue must still be EMPTIED, never left to accumulate behind
  // the early return (the pre-fix bug: this return skipped the clear).
  if (!nav_) {
    std::lock_guard l(invMutex_);
    pendingCols_.clear();
    return;
  }
  std::vector<uint64_t> cols;
  {
    std::lock_guard l(invMutex_);
    cols.swap(pendingCols_);
  }
  if (cols.empty()) return;
  std::sort(cols.begin(), cols.end());
  cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
  for (const uint64_t key : cols) {
    const int cx = int(int32_t(uint32_t(key >> 32))), cz = int(int32_t(uint32_t(key)));
    nav_->invalidate(cx << 4, cz << 4);  // invalidate() takes block coords
  }
}

void AiHost::construct() {
  if (world_ || !chunks_ || !blocks_) return;
  // Live-world feed: the WalkCache reads the ACTIVE dimension's actual
  // ChunkMap, classified by the game's real NavRules (gameNavRules: wire
  // cells are track, content flags mark climbable/fluid).
  nav_ = std::make_unique<ai::WalkCache>(*chunks_, *blocks_, gameNavRules(*blocks_));
  // Columns edited between bind and construct: while disabled those
  // posts were dropped at the sink (B-H1); any that raced the enable
  // store are dropped here (the fresh cache computes from the live map
  // anyway -- this keeps the queue from replaying ancient history on
  // first advance).
  {
    std::lock_guard l(invMutex_);
    pendingCols_.clear();
  }
  // Prewarm the spawn area on this (owner) thread per the WalkCache
  // contract: the 3x3 columns around the bind centre.
  const int ccx = centre_.x >> 4, ccz = centre_.z >> 4;
  for (int dz = -1; dz <= 1; ++dz)
    for (int dx = -1; dx <= 1; ++dx) nav_->prewarm(ccx + dx, ccz + dz);
  ai::AiConfig cfg;  // engine defaults: hz 10 (clamped 8..16), cap 64
  world_ = std::make_unique<ai::AiWorld>(*nav_, cfg, pool_);
  logInfo("ai: AiWorld constructed over the live world (ai.enabled; hz %d, mob cap %d)",
          world_->config().hz, world_->config().mobCap);
}

void AiHost::advance(int simTicks, int worldTps) {
  if (!world_ || simTicks <= 0 || worldTps <= 0) return;
  drainInvalidations();  // AI-tick boundary: nav reflects every edit so far
  if (observerSet_) world_->setObservers({observer_});
  world_->advance(simTicks, worldTps);
}

}  // namespace lithokraft
