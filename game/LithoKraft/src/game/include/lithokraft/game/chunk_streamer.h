// Player-centered chunk streaming for the real game path.
//
// Root cause this fixes: Dimension::load() pre-bakes exactly one fixed square of
// columns, [0, worldChunks) x [0, worldChunks), anchored at the world origin, and
// nothing in the game path ever generated or loaded another column afterwards —
// walking away from spawn meant walking into void (ChunkMap::get on a missing
// column is air, surfaceY is -1, set() fails).
//
// ChunkStreamer closes that hole by reusing the existing machinery, not a
// parallel system: the dimension's ColumnGenerator (same worldgen the pre-bake
// uses), the game's JobPool, and WorldSave (per-column disk loads through the
// existing WorldStore / region format — the save format is unchanged). The Game
// calls update() once per frame with the player position; columns inside the
// load radius (GameConfig::tickRadius, the same radius the sim ticks) that are
// neither loaded nor in flight are requested nearest-first. Finished columns are
// handed back through takeReady() and inserted by the Game while the sim thread
// is stopped — the same stop/insert/start pattern Game::saveWorld already uses —
// because the sim thread owns the ChunkMap while it runs.
//
// Scope (deliberate, first pass): columns are never unloaded, so memory grows
// with explored area. Failures ARE retried (B-L7): a job that throws, fails to
// generate, or finds its saved copy unreadable (B-M3) reports its key back,
// update() un-requests it, and it is re-requested after an exponential
// backoff (500 ms doubling, capped at 30 s, no permanent give-up — a hole
// can always heal, and an unreadable saved column is never generated over).
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "litholattice/core/threading.h"
#include "litholattice/world/chunk_map.h"
#include "litholattice/world/column_generator.h"

namespace lithokraft {

class WorldSave;

/// One finished streamed column, as handed to the Game by takeReady().
struct StreamedColumn {
  std::unique_ptr<litholattice::ChunkColumn> column;
  /// The column's SGNL payload bytes when it came from disk (SW-H1): the
  /// Game scanColumn()s the inserted column and importState()s these bytes
  /// while the sim is stopped. Empty for generated columns (and for saved
  /// columns that hold no components).
  std::vector<uint8_t> signal;
  bool fromDisk = false;  ///< true = loaded from the save; false = freshly generated
};

class ChunkStreamer {
 public:
  /// `gen`: the active dimension's generator (shared with worldgen jobs).
  /// `save` / `dimensionId`: when save is non-null, a saved column on disk wins
  /// over fresh generation (player edits persist across sessions). Both must
  /// outlive the streamer. `initial`: keys of the columns the pre-bake already
  /// loaded, so they are never re-requested.
  ChunkStreamer(litholattice::JobPool& pool, std::shared_ptr<const litholattice::ColumnGenerator> gen, WorldSave* save,
                std::string dimensionId, int radiusChunks, const std::unordered_set<uint64_t>& initial);
  /// Bounded wait for in-flight jobs (SW-H3): at most kDrainTimeout, then a
  /// loud log and return — never an unbounded busy-wait. Jobs hold shared_ptr
  /// copies of the queues/counters and of the generator, so a job that
  /// outlives the streamer finishes into orphaned queues safely.
  ~ChunkStreamer();

  /// Main thread, once per frame: request missing columns within the radius of
  /// the player's chunk, nearest first, bounded by the in-flight cap.
  void update(int playerCx, int playerCz);
  /// Secondary centers (P2c 2026-10-13, CAM-2 + ENT-1): request missing
  /// columns within `radiusChunks` of another center (a non-player
  /// camera's chunk-load center, a projectile's path column). Same
  /// machinery, same dedup set, same in-flight cap — callers issue the
  /// player's update() FIRST each frame, so secondary centers only ever
  /// spend the in-flight capacity the player did not use (player
  /// priority). Radius is clamped to [0, radius_] (a secondary center
  /// never loads wider than the player's own radius).
  void updateAround(int cx, int cz, int radiusChunks);
  /// Request one specific column if it is neither loaded nor in flight
  /// (the projectile manager's chunk seam lands here). Same dedup,
  /// backoff and in-flight cap as update().
  void requestColumn(int cx, int cz);
  /// Drain columns that finished loading/generating since the last call. The
  /// caller owns inserting them into the ChunkMap (sim stopped) and meshing.
  std::vector<StreamedColumn> takeReady();
  /// Bounded drain (P3-2 B-M2): like takeReady(), but returns at most `max`
  /// columns; the rest stay staged (main-thread side) for the next call, so
  /// a burst of finished columns cannot land in ONE sim-stopped insertion
  /// window. Game::pumpStreaming inserts through this with a small
  /// per-frame budget; the snapshot-then-insert discipline is unchanged
  /// (columns are fully generated/loaded before they are ever handed out).
  std::vector<StreamedColumn> takeReadyUpTo(size_t max);
  bool hasReady() const { return !staged_.empty() || readyCount_->load(std::memory_order_relaxed) > 0; }
  /// Column eviction (A2 / B-M1): forgets a RESIDENT column the Game has
  /// evicted — removes its requested_ entry (so a later update() may
  /// re-stream it), drops any staged (finished-but-not-yet-inserted) copy,
  /// and clears retry backoff. Main thread only, like update(). A column
  /// still in flight is NOT forgotten (its job will deliver normally and
  /// the Game's insert path dedups against the resident map).
  void forgetColumn(int cx, int cz);
  /// Columns requested so far that did not come from the pre-bake (diagnostics/tests).
  size_t streamedCount() const { return requested_.size() - initialCount_; }
  size_t requestedCount() const { return requested_.size(); }

 private:
  /// Shared request pump behind update()/updateAround()/requestColumn():
  /// drains failures (arming retry backoff), then submits generation /
  /// load jobs for the wanted columns nearest-first until the in-flight
  /// cap. `want` is (distance^2, key) pairs, already dedup-filtered.
  void pumpRequests(std::vector<std::pair<int, uint64_t>> want);
  /// True when `key` may be requested right now (not already requested,
  /// not inside a retry backoff window at `now`).
  bool requestable(uint64_t key, std::chrono::steady_clock::time_point now);
  /// B-L2 (P3-2): true when chunk (cx, cz)'s block extent fits the packPos
  /// range (chunk.h: x,z in [-2^20, 2^20-1]). Out-of-range centres are
  /// refused (their cells' packPos keys would alias onto other cells);
  /// the refusal is logged loudly once per streamer, not per frame.
  bool centreInRange(int cx, int cz);
  bool boundWarned_ = false;  ///< B-L2: the out-of-range refusal was logged
  static constexpr auto kDrainTimeout = std::chrono::seconds(5);  // SW-H3 bounded destructor wait
  struct Retry { int attempts = 0; std::chrono::steady_clock::time_point nextTry{}; };
  litholattice::JobPool& pool_;
  std::shared_ptr<const litholattice::ColumnGenerator> gen_;
  WorldSave* save_;  // null = generate-only (tests, --no-save)
  std::string dimensionId_;
  int radius_;
  size_t initialCount_ = 0;
  std::unordered_set<uint64_t> requested_;  // loaded or in flight (failures are erased by update(), B-L7)
  std::unordered_map<uint64_t, Retry> retries_;  // failed keys: attempt count + backoff deadline (main thread)
  std::shared_ptr<litholattice::HandoffQueue<StreamedColumn>> done_ =
      std::make_shared<litholattice::HandoffQueue<StreamedColumn>>();
  std::shared_ptr<litholattice::HandoffQueue<uint64_t>> failed_ =
      std::make_shared<litholattice::HandoffQueue<uint64_t>>();
  std::shared_ptr<std::atomic<int>> inFlight_ = std::make_shared<std::atomic<int>>(0);
  std::shared_ptr<std::atomic<int>> readyCount_ = std::make_shared<std::atomic<int>>(0);
  /// B-M2 staging for bounded drains: finished columns moved out of the
  /// lock-free done_ queue but not yet handed to the Game. Main thread only.
  std::vector<StreamedColumn> staged_;
};

/// Column eviction candidate selection (A2 / B-M1): keys of resident
/// columns whose Chebyshev chunk distance from (centerCx, centerCz)
/// exceeds streamRadius + hysteresisChunks, farthest first (ties broken
/// by key, so the choice is deterministic), truncated to `budget`.
/// Pure function of the resident key set — the Game feeds it the active
/// dimension's column keys each pump; hysteresis/budget values are the
/// caller's policy (JD-DECISION parameters, see game.cpp).
std::vector<uint64_t> selectEvictionCandidates(const std::vector<uint64_t>& residentKeys, int centerCx, int centerCz,
                                               int streamRadius, int hysteresisChunks, size_t budget);

}  // namespace lithokraft
