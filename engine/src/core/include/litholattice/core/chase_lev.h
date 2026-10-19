// Lock-free work-stealing deque (Chase-Lev, SPAA 2005): the owner pushes and pops
// from the bottom (LIFO); any number of thief threads steal from the top (FIFO).
// This is the per-thread queue behind JobSystem.
//
// OWNERSHIP (read this — it bit us once): the Chase-Lev algorithm requires a
// single owner thread for pushBottom/popBottom. JobSystem submits come from ANY
// thread, so a per-deque mutex (`ownerM_`) serializes the owner operations and
// restores the invariant. steal() never takes it and stays fully lock-free, so
// cross-thread load balancing never contends with the owner. The mutex is a leaf
// (L1 in the job_system.h lock order): it is never held while acquiring another
// lock or while running a job.
//
// Correctness notes (the subtle ones — read before touching):
//  - top_/bottom_ are SIGNED (int64_t). An empty popBottom() transiently stores -1
//    into bottom_; with unsigned arithmetic that would wrap to SIZE_MAX and a
//    concurrent steal() could then observe top < bottom on an empty deque and
//    "steal" a garbage slot. Signed keeps the empty case ordered (top=0 > bottom=-1).
//  - pushBottom: item write, release fence, then bottom_ store. A thief that
//    observes the new bottom_ via its acquire load synchronizes with that fence,
//    so the item write is visible before the slot is read. (Standard release/acquire
//    pairing; TSan models it.)
//  - popBottom / steal race on the last item is settled by a single seq_cst CAS on
//    top_, exactly as in the paper; the loser backs off (popBottom restores
//    bottom_, steal returns false without touching the caller's slot).
//  - Both steal() and popBottom() COPY the slot before the last-item CAS and only
//    move the copy into `out` on success, so a lost race never corrupts (or, for
//    move-aware T like std::function, empties) the slot the winner takes.
//  - Slots are never cleared after a pop (the paper doesn't either): a thief that
//    lost the CAS may still be copying the slot, and clearing would turn that
//    safe read/read overlap into a read/write data race.
//  - Fixed capacity on purpose: a growable array would let a thief that loaded the
//    old array pointer steal a moved-from slot after the owner grew (phantom job).
//    When full, pushBottom returns false and the JobSystem spills to a shared
//    overflow queue (cold path, mutex-protected). T must therefore be cheaply
//    copyable (std::function is).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

#include "litholattice/core/thread_debug.h"

namespace litholattice {

template <typename T, size_t kCapacity = 1024>
class ChaseLevDeque {
  static_assert((kCapacity & (kCapacity - 1)) == 0, "capacity must be a power of two");
  static_assert(kCapacity >= 2, "capacity must hold at least two items");

 public:
  ChaseLevDeque() {
    buffer_ = std::make_unique<Buffer>();
    array_.store(buffer_.get(), std::memory_order_relaxed);
  }
  ~ChaseLevDeque() = default;  // threads must be joined before this runs (JobSystem does)
  ChaseLevDeque(const ChaseLevDeque&) = delete;
  ChaseLevDeque& operator=(const ChaseLevDeque&) = delete;

  // ---- owner operations (serialized by ownerM_) ----------------------------------
  // Single logical owner: JobSystem submits arrive from any thread, so the mutex
  // restores the one-owner-at-a-time invariant the algorithm requires. Uncontended
  // in the common case (the worker owns its deque); a submitting thread only
  // contends briefly with that worker's pops.
  /// Push onto the bottom. Returns false when full WITHOUT touching `item` (the
  /// caller keeps ownership and spills elsewhere); never blocks, never allocates.
  bool pushBottom(T&& item) {
    std::lock_guard<std::mutex> l(ownerM_);
    const int64_t b = bottom_.load(std::memory_order_relaxed);
    const int64_t t = top_.load(std::memory_order_acquire);
    if (b - t >= static_cast<int64_t>(kCapacity) - 1) return false;  // full: `item` untouched
    Buffer* a = array_.load(std::memory_order_relaxed);
    a->items[size_t(b) & (kCapacity - 1)] = std::move(item);
    std::atomic_thread_fence(std::memory_order_release);
    bottom_.store(b + 1, std::memory_order_relaxed);
    return true;
  }

  /// Pop from the bottom (LIFO). Returns false when empty.
  /// COPY-BEFORE-COMMIT (review A-M4): the candidate slot is copied into a
  /// local BEFORE bottom_ is decremented — the same order steal() uses. The
  /// old code decremented bottom_ first, so a throwing copy (std::function
  /// can throw on copy, e.g. bad_alloc) left bottom_ already rewound: the
  /// job was neither returned nor restored and silently vanished, while the
  /// caller's in-flight accounting had already counted it. Now a throwing
  /// copy propagates with bottom_/top_ untouched and the job still queued;
  /// the caller (JobSystem::workerLoop) catches, counts, and retries.
  /// Owner operations are serialized by ownerM_, so no other owner can move
  /// bottom_ between the copy and the commit; thieves only take from the
  /// top, and the last-item race is still settled by the seq_cst CAS below.
  bool popBottom(T& out) {
    std::lock_guard<std::mutex> l(ownerM_);
    const int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
    const int64_t t0 = top_.load(std::memory_order_acquire);
    if (t0 > b) return false;  // empty: nothing to copy, nothing to commit
    // COPY, never move, until the race below is resolved: a concurrent thief
    // copies the same slot, and exactly one of us wins the CAS. (Moving here
    // would let the thief copy a moved-from shell -> empty job -> crash.)
    // This copy happens BEFORE the commit store to bottom_: if it throws,
    // the deque is exactly as it was on entry.
    Buffer* a = array_.load(std::memory_order_relaxed);
    const size_t idx = size_t(b) & (kCapacity - 1);
    T item = a->items[idx];
    // Commit: claim slot b, then re-read top_ (a thief may have taken the
    // top item — possibly this very slot, if it was also the last — while
    // we copied; the CAS decides that case exactly as in the paper).
    bottom_.store(b, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const int64_t t = top_.load(std::memory_order_relaxed);
    if (t <= b) {
      if (t == b) {
        // Last item: race the thieves; exactly one winner via CAS on top_.
        int64_t expected = t;
        if (!top_.compare_exchange_strong(expected, t + 1, std::memory_order_seq_cst,
                                         std::memory_order_relaxed)) {
          bottom_.store(b + 1, std::memory_order_relaxed);
          return false;  // lost: the thief owns the item; our copy is discarded
        }
        bottom_.store(b + 1, std::memory_order_relaxed);
      }
      // (t < b: bottom_ is already b from the commit store.)
      // NOTE: the slot is deliberately NOT cleared. A thief that lost the CAS above
      // may still be copying this slot (its copy is discarded, but the copy itself
      // must not race a write). Clearing would turn that safe read/read overlap
      // into a read/write data race on T. Stale slot contents are harmless:
      // indices are consumed monotonically and every slot is overwritten by
      // pushBottom before it is ever read again.
      out = std::move(item);
      return true;
    }
    bottom_.store(b + 1, std::memory_order_relaxed);  // a thief took the last item first: restore
    return false;
  }

  // ---- any thread -------------------------------------------------------------
  /// Steal from the top (FIFO). Returns false when empty or the race was lost.
  bool steal(T& out) {
    int64_t t = top_.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const int64_t b = bottom_.load(std::memory_order_acquire);
    if (t < b) {
      Buffer* a = array_.load(std::memory_order_acquire);
      // Copy first: if the CAS below loses, the slot still belongs to the owner.
      T item = a->items[size_t(t) & (kCapacity - 1)];
      if (top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                      std::memory_order_relaxed)) {
        out = std::move(item);
        return true;
      }
    }
    return false;
  }

  /// Racy estimate for debug/metrics only. Never use for control flow.
  size_t approxSize() const {
    const int64_t s =
        bottom_.load(std::memory_order_relaxed) - top_.load(std::memory_order_relaxed);
    return s < 0 ? 0 : static_cast<size_t>(s);
  }

 private:
  struct Buffer {
    std::unique_ptr<T[]> items = std::make_unique<T[]>(kCapacity);
  };
  std::unique_ptr<Buffer> buffer_;  // fixed for the deque's lifetime (see note above)
  std::atomic<Buffer*> array_{nullptr};
  // Serializes the owner operations (pushBottom/popBottom) across submitting
  // threads. Leaf mutex (L1): never held while acquiring another lock.
  std::mutex ownerM_;
  alignas(64) std::atomic<int64_t> top_{0};     // thieves' end (CAS)
  alignas(64) std::atomic<int64_t> bottom_{0};  // owner's end
};

}  // namespace litholattice
