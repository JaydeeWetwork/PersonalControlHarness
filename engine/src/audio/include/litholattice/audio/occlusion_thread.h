// Occlusion simulation thread for spatial audio.
//
// WHAT: Runs wall-occlusion checks off the audio thread.
// WHY: Ray tracing on the audio callback causes dropouts. This thread runs at
//      32 Hz (powers of two), publishes per-voice occlusion gains via atomics,
//      and the audio thread just reads. Clean-room: no DLLs, no Steam Audio,
//      our own voxel raycast.
//
// RATE LADDER (powers of two):
//   32 Hz — full rate (default)
//   16 Hz — fallback for slower systems
//   0 Hz  — inline mode (no thread, occlusion checked on demand, cheapest)
//
// HOW:
// - Game thread registers (listener, sound) pairs each frame.
// - Occlusion thread iterates them at the current rate, runs isOccluded(),
//   writes 0.0 (blocked) or 1.0 (clear) — or partial for distance — to atomics.
// - Audio thread reads atomics. Never traces a ray. Never blocks: gain()
//   reads a lock-free published snapshot of the query-pointer array
//   (H-AUD-1) — it takes no mutex. Structural mutation (addQuery) still
//   publishes under a mutex on the game thread, never on the audio thread.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <functional>

namespace litholattice {

/// Occlusion rate tiers. Powers of two.
enum class OcclusionRate : uint8_t {
  Full = 32,     ///< 32 Hz — default, smooth occlusion transitions
  Reduced = 16,  ///< 16 Hz — fallback for slower CPUs
  Low = 8,       ///< 8 Hz — fallback for low-end tier machines
  Inline = 0,    ///< no thread — check on demand (weakest systems)
};

/// One occlusion query: is the path from listener to sound blocked?
struct OcclusionQuery {
  // Positions are atomics: the game thread writes them in addQuery() (before
  // the active release-store) and the occlusion thread reads them with
  // acquire pairing, so no torn/stale positions cross the race.
  std::atomic<float> lx{0.f}, ly{0.f}, lz{0.f};  ///< listener position
  std::atomic<float> sx{0.f}, sy{0.f}, sz{0.f};  ///< sound position
  std::atomic<float> gain{1.0f};  ///< 1.0 = clear, 0.0 = fully blocked
  std::atomic<bool> active{false};
};

/// Dedicated occlusion simulation thread.
class OcclusionThread {
 public:
  OcclusionThread();
  ~OcclusionThread();

  /// Start at the given rate. Falls back to Reduced if Full can't keep up.
  void start(OcclusionRate rate = OcclusionRate::Full);
  void stop();

  /// Register a query. Returns index. Game thread calls this.
  /// The isOccluded callback does the actual voxel raycast.
  size_t addQuery(float lx, float ly, float lz, float sx, float sy, float sz);

  /// Remove a query (when sound expires).
  void removeQuery(size_t idx);

  /// Read the current gain for a query. Lock-free, audio-thread safe:
  /// one atomic load of the published snapshot, one atomic gain load.
  float gain(size_t idx) const;

  /// Set the voxel occlusion test. Must be called before start(): assigning
  /// the std::function while the thread reads it would be a data race.
  void setOcclusionTest(std::function<bool(float,float,float,float,float,float)> fn);

  /// Current effective rate (may have fallen back).
  OcclusionRate rate() const { return rate_; }

  /// Switch inline mode on/off. setInline(true): stop the thread, rate =
  /// Inline. setInline(false): leave inline mode and restart the thread at
  /// Full rate (H-AUD-3 — the old no-op false branch forced callers to know
  /// they had to call start() themselves). No-op when not in inline mode.
  void setInline(bool inlineMode);

 private:
  void threadMain();

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<OcclusionRate> rate_{OcclusionRate::Full};
  // Structural mutation (addQuery push_back) is mutex-guarded; threadMain
  // snapshots raw pointers under the lock and raycasts unlocked. The
  // OcclusionQuery objects themselves are heap-stable (unique_ptr, never
  // erased), so snapshot pointers stay valid after the lock is released.
  mutable std::mutex queriesMu_;
  std::vector<std::unique_ptr<OcclusionQuery>> queries_;  // unique_ptr: atomics aren't movable
  std::vector<OcclusionQuery*> snapshot_;  // per-tick scratch, reused (no per-tick alloc)
  // H-AUD-1 lock-free read snapshot for gain(): an immutable pointer array
  // published with a release store on every structural change (addQuery
  // append — slot reuse changes nothing structurally). gain() acquire-loads
  // the pointer and reads through it with NO mutex, so the audio thread can
  // never block behind addQuery/removeQuery/threadMain. Retired arrays are
  // kept alive in retiredSnapshots_ for the life of the object: a reader
  // that loaded the old pointer just before a publish keeps reading a valid
  // (stale) array — the standard retire-don't-free snapshot pattern, at the
  // cost of one small array per append.
  struct QuerySnapshot {
    std::vector<const OcclusionQuery*> queries;
  };
  std::atomic<const QuerySnapshot*> publishedSnapshot_{nullptr};
  std::vector<std::unique_ptr<QuerySnapshot>> retiredSnapshots_;  // under queriesMu_
  /// Build + publish a fresh snapshot of queries_. Caller holds queriesMu_.
  void publishSnapshotLocked();
  std::function<bool(float,float,float,float,float,float)> isOccluded_;

  // Performance monitoring: if we miss our deadline too often, drop to 16 Hz.
  static constexpr int kMissThreshold = 64;  // consecutive misses before fallback
  int missCount_ = 0;
};

}  // namespace litholattice
