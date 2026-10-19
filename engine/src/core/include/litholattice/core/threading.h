// Threading primitives: lock-free triple buffer for snapshots, and a small job pool.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <deque>
#include <functional>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>
#include "litholattice/core/log.h"

namespace litholattice {

// Single-producer / single-consumer "latest value" mailbox. The writer never blocks the
// reader and vice versa (double buffering plus a spare slot so neither side ever waits).
//
// READ-REFERENCE LIFETIME (review A-L5): read() returns a reference into one of the
// three slots, and that reference is borrowed, not owned. It stays valid only until
// the writer's NEXT publish+write cycle reaches the same slot: the writer rotates
// through the slots, so after at most two further publish() calls the writer may be
// writing into the very slot a slow reader still holds, and the reader will observe
// the value changing under it (or torn, for non-trivially-copyable T). Callers must
// therefore COPY the value out of read() immediately and must not retain the
// reference across frames/ticks. There is exactly ONE reader thread: read() mutates
// the plain (non-atomic) read_ index, so two concurrent readers race with each
// other even though each individual exchange with the writer is atomic.
template <class T>
class TripleBuffer {
 public:
  T& writeSlot() { return slots_[write_]; }
  void publish() {  // writer: swap back slot into the shared middle, flag it fresh
    unsigned prev = middle_.exchange(write_ | kFresh, std::memory_order_acq_rel);
    write_ = prev & ~kFresh;
  }
  const T& read() {  // reader: grab the freshest slot if one was published
    if (middle_.load(std::memory_order_relaxed) & kFresh) {
      unsigned prev = middle_.exchange(read_, std::memory_order_acq_rel);
      read_ = prev & ~kFresh;
    }
    return slots_[read_];
  }

 private:
  static constexpr unsigned kFresh = 4;
  T slots_[3]{};
  unsigned write_ = 0, read_ = 1;
  std::atomic<unsigned> middle_{2};
};

/// Fixed set of worker threads draining one FIFO of jobs. Exception-safe: a job that throws is
/// logged and counted (failedJobs()) instead of terminating the process, and the in-flight count
/// is always released, so waitIdle() and JobBatch::wait() can never hang on a failed job.
class JobPool {
 public:
  explicit JobPool(unsigned n) {
    workers_.reserve(n);
    for (unsigned i = 0; i < n; ++i) workers_.emplace_back([this] { run(); });
  }
  ~JobPool() {
    { std::lock_guard l(m_); stop_ = true; }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
  }
  JobPool(const JobPool&) = delete;
  JobPool& operator=(const JobPool&) = delete;

  void submit(std::function<void()> job) {
    { std::lock_guard l(m_); ++inFlight_; jobs_.push_back(std::move(job)); }
    cv_.notify_one();
  }
  unsigned size() const { return unsigned(workers_.size()); }
  /// Blocks (no polling) until every job submitted so far has finished.
  void waitIdle() const {
    std::unique_lock l(m_);
    idle_.wait(l, [this] { return inFlight_ == 0; });
  }
  /// Jobs that ended with an exception since the pool was created.
  uint64_t failedJobs() const { return failed_.load(std::memory_order_relaxed); }

 private:
  void run() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock l(m_);
        cv_.wait(l, [this] { return stop_ || !jobs_.empty(); });
        if (stop_ && jobs_.empty()) return;
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      try {
        job();
      } catch (const std::exception& e) {
        failed_.fetch_add(1, std::memory_order_relaxed);
        logError("job pool: job threw: %s", e.what());
      } catch (...) {
        failed_.fetch_add(1, std::memory_order_relaxed);
        logError("job pool: job threw a non-std exception");
      }
      bool idle;
      { std::lock_guard l(m_); idle = --inFlight_ == 0; }
      if (idle) idle_.notify_all();
    }
  }
  std::vector<std::thread> workers_;
  std::deque<std::function<void()>> jobs_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  mutable std::condition_variable idle_;
  bool stop_ = false;
  int inFlight_ = 0;  // guarded by m_
  std::atomic<uint64_t> failed_{0};
};

/// A group of jobs on a JobPool that can be waited on by itself (other pool work keeps running).
/// Each job's exception is caught here; wait() reports how many jobs of this batch failed.
class JobBatch {
 public:
  explicit JobBatch(JobPool& pool) : pool_(pool) {}
  ~JobBatch() { wait(); }  // jobs reference this object: never let it die before they finish
  JobBatch(const JobBatch&) = delete;
  JobBatch& operator=(const JobBatch&) = delete;

  void submit(std::function<void()> job) {
    { std::lock_guard l(m_); ++pending_; }
    pool_.submit([this, job = std::move(job)] {
      bool ok = true;
      try { job(); } catch (const std::exception& e) { ok = false; logError("job batch: job threw: %s", e.what()); }
      catch (...) { ok = false; logError("job batch: job threw a non-std exception"); }
      std::lock_guard l(m_);
      if (!ok) ++failed_;
      if (--pending_ == 0) cv_.notify_all();  // notify under the lock: the waiter may destroy *this right after
    });
  }
  /// Blocks until every job submitted to this batch has finished. Returns the number that threw.
  int wait() {
    std::unique_lock l(m_);
    cv_.wait(l, [this] { return pending_ == 0; });
    return failed_;
  }

 private:
  JobPool& pool_;
  std::mutex m_;
  std::condition_variable cv_;
  int pending_ = 0, failed_ = 0;
};

// MPSC hand-off: workers push, render thread swaps the whole vector out in O(1).
template <class T>
class HandoffQueue {
 public:
  void push(T v) { std::lock_guard l(m_); items_.push_back(std::move(v)); }
  void drainInto(std::vector<T>& out) {
    // Moves straight out of items_ and clears (keeping its buffer): the old
    // swap-into-a-local version freed the queue's heap buffer on every drain,
    // so the next push reallocated - one alloc/free per drain cycle.
    std::lock_guard l(m_);
    out.insert(out.end(), std::make_move_iterator(items_.begin()),
               std::make_move_iterator(items_.end()));
    items_.clear();
  }
 private:
  std::mutex m_;
  std::vector<T> items_;
};

}  // namespace litholattice
