// Occlusion thread implementation.
//
// The thread loop:
//  1. Sleep until next tick (32 Hz = 31.25ms, 16 Hz = 62.5ms).
//  2. For each active query, run isOccluded().
//  3. Write gain to atomic. Audio thread reads it lock-free.
//
// If we consistently miss our deadline, drop from 32 Hz to 16 Hz automatically.
// This is the "graceful degradation" — the game keeps working, just with
// slightly staler occlusion data.

#include "litholattice/audio/occlusion_thread.h"
#include <cassert>
#include <chrono>

namespace litholattice {

OcclusionThread::OcclusionThread() = default;

OcclusionThread::~OcclusionThread() { stop(); }

void OcclusionThread::start(OcclusionRate rate) {
  if (running_.load()) return;
  rate_.store(rate);
  if (rate == OcclusionRate::Inline) return;  // no thread needed in inline mode
  running_.store(true);
  thread_ = std::thread(&OcclusionThread::threadMain, this);
}

void OcclusionThread::stop() {
  running_.store(false);
  if (thread_.joinable()) thread_.join();
}

size_t OcclusionThread::addQuery(float lx, float ly, float lz,
                                  float sx, float sy, float sz) {
  // Structural mutation is mutex-guarded: the occlusion thread may be
  // iterating a snapshot of these same objects right now.
  std::lock_guard<std::mutex> lock(queriesMu_);
  // Find a free slot or append.
  for (size_t i = 0; i < queries_.size(); ++i) {
    bool expected = false;
    if (queries_[i]->active.compare_exchange_strong(expected, true,
                                                    std::memory_order_acq_rel)) {
      // Slot claimed (no other thread can see it as active until we publish
      // below). Write the positions first, then publish with a release
      // store: the occlusion thread's acquire load of `active` pairs with
      // it, so these position stores are visible before the slot reads live.
      queries_[i]->lx.store(lx, std::memory_order_relaxed);
      queries_[i]->ly.store(ly, std::memory_order_relaxed);
      queries_[i]->lz.store(lz, std::memory_order_relaxed);
      queries_[i]->sx.store(sx, std::memory_order_relaxed);
      queries_[i]->sy.store(sy, std::memory_order_relaxed);
      queries_[i]->sz.store(sz, std::memory_order_relaxed);
      queries_[i]->gain.store(1.0f, std::memory_order_relaxed);
      // Publish: release pairs with the thread's acquire load of `active`.
      queries_[i]->active.store(true, std::memory_order_release);
      return i;
    }
  }
  auto q = std::make_unique<OcclusionQuery>();
  q->lx.store(lx, std::memory_order_relaxed);
  q->ly.store(ly, std::memory_order_relaxed);
  q->lz.store(lz, std::memory_order_relaxed);
  q->sx.store(sx, std::memory_order_relaxed);
  q->sy.store(sy, std::memory_order_relaxed);
  q->sz.store(sz, std::memory_order_relaxed);
  q->active.store(true, std::memory_order_release);
  queries_.push_back(std::move(q));
  publishSnapshotLocked();  // structural change: gain() readers see the new slot
  return queries_.size() - 1;
}

void OcclusionThread::publishSnapshotLocked() {
  auto snap = std::make_unique<QuerySnapshot>();
  snap->queries.reserve(queries_.size());
  for (const auto& q : queries_) snap->queries.push_back(q.get());
  const QuerySnapshot* raw = snap.get();
  retiredSnapshots_.push_back(std::move(snap));  // retired, never freed early
  publishedSnapshot_.store(raw, std::memory_order_release);
}

void OcclusionThread::removeQuery(size_t idx) {
  std::lock_guard<std::mutex> lock(queriesMu_);
  if (idx < queries_.size()) queries_[idx]->active.store(false, std::memory_order_release);
}

float OcclusionThread::gain(size_t idx) const {
  // H-AUD-1: genuinely lock-free (the header always claimed it; the old body
  // took queriesMu_). One acquire load of the published snapshot, then the
  // query's own atomic gain. A snapshot loaded just before a publish is
  // stale-but-valid: retired arrays stay alive for the object's lifetime.
  const QuerySnapshot* snap = publishedSnapshot_.load(std::memory_order_acquire);
  if (!snap || idx >= snap->queries.size()) return 1.0f;
  const OcclusionQuery* q = snap->queries[idx];
  if (!q) return 1.0f;
  return q->gain.load(std::memory_order_relaxed);
}

void OcclusionThread::setOcclusionTest(
    std::function<bool(float,float,float,float,float,float)> fn) {
  // Assigning the std::function while threadMain reads it is a data race;
  // the test must be installed before the thread starts.
  assert(!running_.load(std::memory_order_relaxed) && "setOcclusionTest must precede start()");
  isOccluded_ = std::move(fn);
}

void OcclusionThread::setInline(bool inlineMode) {
  if (inlineMode) {
    stop();
    rate_.store(OcclusionRate::Inline);
  } else if (rate_.load(std::memory_order_relaxed) == OcclusionRate::Inline) {
    // H-AUD-3: leaving inline mode restarts the thread at Full rate. The old
    // implementation did nothing at all for false, so a caller that toggled
    // inline off silently kept on-demand occlusion until it happened to call
    // start() itself. (start() refuses to double-start a running thread.)
    start(OcclusionRate::Full);
  }
}

void OcclusionThread::threadMain() {
  using clock = std::chrono::steady_clock;

  while (running_.load(std::memory_order_relaxed)) {
    auto rate = rate_.load(std::memory_order_relaxed);
    if (rate == OcclusionRate::Inline) break;

    int hz = static_cast<int>(rate);  // 32, 16, or 8
    auto tickStart = clock::now();
    auto interval = std::chrono::milliseconds(1000 / hz);

    // Run occlusion checks for all active queries. Snapshot the (heap-stable)
    // raw pointers under the lock, then raycast unlocked: the game thread can
    // addQuery() concurrently without invalidating the snapshot, because query
    // objects are never moved or erased.
    if (isOccluded_) {
      snapshot_.clear();
      {
        std::lock_guard<std::mutex> lock(queriesMu_);
        snapshot_.reserve(queries_.size());
        for (auto& q : queries_) snapshot_.push_back(q.get());
      }
      for (OcclusionQuery* q : snapshot_) {
        if (!q->active.load(std::memory_order_acquire)) continue;
        // Acquire on `active` pairs with the release publish in addQuery(),
        // so the position loads below see the registered coordinates.
        const float lx = q->lx.load(std::memory_order_relaxed);
        const float ly = q->ly.load(std::memory_order_relaxed);
        const float lz = q->lz.load(std::memory_order_relaxed);
        const float sx = q->sx.load(std::memory_order_relaxed);
        const float sy = q->sy.load(std::memory_order_relaxed);
        const float sz = q->sz.load(std::memory_order_relaxed);
        bool blocked = isOccluded_(lx, ly, lz, sx, sy, sz);
        // Smooth transition: lerp toward target (avoids popping)
        float target = blocked ? 0.0f : 1.0f;
        float current = q->gain.load(std::memory_order_relaxed);
        float smoothed = current + (target - current) * 0.5f;
        q->gain.store(smoothed, std::memory_order_relaxed);
      }
    }

    // Deadline check: did we finish in time?
    // Graceful degradation: 32 -> 16 -> 8 -> (manual inline).
    auto elapsed = clock::now() - tickStart;
    if (elapsed > interval) {
      // Missed deadline.
      auto current = rate_.load();
      if (++missCount_ >= kMissThreshold) {
        if (current == OcclusionRate::Full) {
          rate_.store(OcclusionRate::Reduced);  // 32 -> 16 Hz
          missCount_ = 0;
        } else if (current == OcclusionRate::Reduced) {
          rate_.store(OcclusionRate::Low);  // 16 -> 8 Hz
          missCount_ = 0;
        }
        // At Low (8 Hz), we stay there. Inline is manual only.
      }
      continue;  // skip sleep, we're already behind
    }
    missCount_ = 0;
    std::this_thread::sleep_until(tickStart + interval);
  }
}

}  // namespace litholattice
