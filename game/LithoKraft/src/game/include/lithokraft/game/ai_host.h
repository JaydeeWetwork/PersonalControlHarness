// AiHost: LithoKraft's production owner for the engine AiWorld
// (litholattice::ai) — brief B6 wiring, 2026-10-13.
//
// WHAT: Game owns one AiHost. loadActive() binds it to the ACTIVE
//   dimension's live ChunkMap + the game block registry + the game JobPool
//   (before the SimThread starts, while the game thread still owns the
//   map). When the `ai.enabled` cvar is on, the host constructs a
//   WalkCache over that live map and an AiWorld on top of it; Game::update
//   then drives it from elapsed sim world ticks via
//   AiWorld::advance(simTicks, worldTps) — never tick() per sim tick — so
//   AI thinks at its own clamped 8..16 Hz, freezes while the sim is
//   paused, and never rides the circuit clock (code-review ENT-2 fix
//   direction). The player cell is fed as the AiWorld observer each
//   advance, so LOD/senses distance is computed against the live player.
//
// P0 WIRING (2026-10-14): two feeds that B6 left as defaults are now real.
//   (a) NavRules: the WalkCache is built with gameNavRules() below --
//   solidity from BlockFlags, track from SimRole::Wire, climbable/fluid
//   from the kGameClimbable / kGameFluid content flags (no block carries
//   them yet; see blocks.h). (b) Invalidations: bindWorld installs a
//   ChunkMap edit listener; every structural edit (set / erase / insert on
//   the sim or game thread) posts its column to a mutex-guarded queue, and
//   advance() drains it into WalkCache::invalidate() at the AI-tick
//   boundary, before the AiWorld plans -- so mobs path on current geometry,
//   and the drain point (not wall-clock edit order across threads) is what
//   keeps the AI history reproducible from the sim-tick stream.
//   B-H1 (2026-10-15): posts made while the gate is OFF are dropped at
//   the sink and never queued -- construct() discards the queue anyway
//   (a fresh cache computes from the live map), so a pre-enable post can
//   never be a "missed edit" that matters. The listener itself stays
//   installed for the whole bind lifetime (install/remove is tied to
//   bindWorld/unbindWorld, and ChunkMap holds a single listener slot);
//   while disabled its entire cost is one atomic load per edit on the
//   mutating thread: no mutex, no allocation, no queue growth.
//
// GATE: `ai.enabled` (Bool, default 0 = OFF, archived; registered by
//   registerAiCvars in settings_cvars.cpp, the registerMcpSensesCvars
//   pattern for host knobs outside GameSettings). Disabled = no AiWorld
//   and no WalkCache are constructed, advance() is a no-op, and the edit
//   listener drops every post (see B-H1 above): zero effect on the
//   game. Default behaviour is therefore unchanged.
//
// SCOPE (honest): this is the construction + tick-driving + live-world
//   feed path. No mob content/definitions and no spawn/lifecycle design
//   are invented here — the production AiWorld starts EMPTY; what spawns
//   mobs in the live game is JD's scoping call. The smoke selftest
//   (ai_host_selftest.cpp) spawns through the host's AiWorld directly to
//   prove the production path end to end.
//
// THREADING: game thread only (bound, enabled and advanced inside
//   Game::loadActive / Game::update). The WalkCache is constructed over
//   the live map at enable time and the spawn area (3x3 columns around
//   the bind centre) is prewarmed there, per the WalkCache contract.
//   Beyond that, first-compute is prewarm-only (B-H3, 2026-10-15):
//   AiWorld::tick prewarms every column inside each pending search's
//   plan box on the tick (game) thread BEFORE submitting the search to
//   the JobPool, so a pool worker never computes a column -- workers
//   only read already-cached, immutable columns. Dimension switch
//   unbinds the host BEFORE the old dimension unloads (the cache holds
//   a map reference).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "litholattice/ai/ai_world.h"
#include "litholattice/ai/nav.h"
#include "litholattice/core/threading.h"  // JobPool
#include "litholattice/world/block.h"    // BlockRegistry
#include "litholattice/world/chunk_map.h"

namespace lithokraft {

class AiHost {
 public:
  AiHost() = default;
  AiHost(const AiHost&) = delete;
  AiHost& operator=(const AiHost&) = delete;

  /// Bind to a live dimension's world (game thread, before the sim
  /// starts). Rebinding destroys any previous AiWorld/WalkCache first.
  /// `centre` is the spawn/player cell: its 3x3 columns are prewarmed
  /// when (and only when) the AiWorld is constructed.
  void bindWorld(litholattice::ChunkMap& chunks, const litholattice::BlockRegistry& blocks,
                 litholattice::JobPool* pool, litholattice::ai::Cell centre);
  /// Destroy the AiWorld + WalkCache and forget the world. Safe anytime.
  void unbindWorld();

  /// The ai.enabled gate. Enabling constructs (when bound); disabling
  /// destroys. Repeated calls with the same value are no-ops.
  void setEnabled(bool on);
  bool enabled() const { return enabled_.load(); }
  /// B-H1 test/diagnostic seam: columns currently queued for the next
  /// drain. Provably 0 at all times while the gate is off (posts are
  /// dropped at the sink), and 0 again after every advance() drain.
  size_t pendingInvalidations() const {
    std::lock_guard l(invMutex_);
    return pendingCols_.size();
  }
  /// True when an AiWorld actually exists (bound + enabled).
  bool constructed() const { return world_ != nullptr; }

  /// The LOD/senses observer feed: the live player cell, set by Game
  /// before each advance.
  void setObserver(litholattice::ai::Cell c) {
    observer_ = c;
    observerSet_ = true;
  }

  /// Drive the AiWorld by elapsed sim world ticks (AiWorld::advance
  /// semantics: AI ticks = simTicks * hz / worldTps, hz clamped 8..16).
  /// First drains the pending column invalidations posted by the ChunkMap
  /// edit listener into the WalkCache. No-op unless constructed.
  void advance(int simTicks, int worldTps);

  litholattice::ai::AiWorld* world() { return world_.get(); }
  const litholattice::ai::AiWorld* world() const { return world_.get(); }
  const litholattice::ai::WalkCache* nav() const { return nav_.get(); }

 private:
  void construct();  // requires bound pointers; no-op when already built
  void postEditedColumn(int cx, int cz);  // ChunkMap listener sink (any thread)
  void drainInvalidations();              // game thread, from advance()

  litholattice::ChunkMap* chunks_ = nullptr;
  const litholattice::BlockRegistry* blocks_ = nullptr;
  litholattice::JobPool* pool_ = nullptr;
  litholattice::ai::Cell centre_{};
  std::unique_ptr<litholattice::ai::WalkCache> nav_;
  std::unique_ptr<litholattice::ai::AiWorld> world_;
  // Atomic: the edit-listener sink reads the gate on the MUTATING thread
  // (sim thread in play) while setEnabled writes it on the game thread
  // (B-H1). Relaxed would suffice (the flag gates a queue, not data);
  // the default seq_cst load costs nothing measurable per edit.
  std::atomic<bool> enabled_ = false;  // ai.enabled gate (default OFF)
  bool observerSet_ = false;
  litholattice::ai::Cell observer_{};
  mutable std::mutex invMutex_;
  std::vector<uint64_t> pendingCols_;  // packed column keys from the edit listener
};

/// The game's NavRules for the engine WalkCache (P0): solidity from
/// BlockFlags; track = SimRole::Wire cells (flat trace and vias alike --
/// the cache only marks passable cells, so solid vias never become track);
/// climbable / fluid from the kGameClimbable / kGameFluid content flags.
/// The predicates read the PASSED registry (no globals), so tests can build
/// rules over fixture registries.
litholattice::ai::NavRules gameNavRules(const litholattice::BlockRegistry& blocks);

/// Smoke self-test (app selftest section): disabled = no construction /
/// no effect; enabled = AiWorld constructed over a fixture live map
/// through this host, advance() drives it at the clamped Hz, a spawned
/// mob paths and moves; disable/unbind destroy it. Returns failures.
int runAiHostSelfTest();

}  // namespace lithokraft
