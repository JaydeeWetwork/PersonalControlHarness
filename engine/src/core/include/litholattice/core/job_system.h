// Work-stealing job system: the threading backbone for LithoLattice.
//
// What it is: N named worker threads, each owning kJobPriorityCount lock-free
// Chase-Lev deques (one per JobPriority). Idle workers steal from each other
// (high priority first); a shared overflow queue absorbs the rare case of a full
// deque. Dedicated I/O threads run blocking jobs so worker threads NEVER block.
// A JobFence is the per-stage barrier the deterministic scheduler uses.
//
// Relationship to the existing JobPool (threading.h): JobPool stays exactly as it
// is, for simple uses (a correct, obviously-right mutex FIFO; the procgen self-test
// and several fixtures use it). JobSystem is the high-performance substrate for the
// sim, render, audio-adjacent and I/O work. Deliberately NOT a wrapper: the two
// have different fairness semantics (FIFO vs. work-stealing) and different costs.
//
// ---- lock order (strict; read before adding any lock) --------------------------
// L0  lock-free: Chase-Lev deques, SpscQueue, atomics (stop_, workGen_,
//     inFlight_, ioInFlight_, submitCursor_). No lock, no order.
// L1  overflowM_ / ioM_ / fence m_: leaf mutexes. Held only to push/drain a short
//     queue or to flip a completion counter; NEVER held while running a job and
//     never held while acquiring L2.
// L2  JobSystem control mutex m_: only for the sleep/wake condition-variable
//     handshake and the shutdown sequence. Never held while running a job.
// L3  debug-log mutex (thread_debug.cpp): absolute leaf. Taken to emit one log
//     line, released immediately; no other lock may be acquired while holding it.
// RULES: no lock is ever held across a job boundary (jobs run with zero locks
// held); a job must never call waitIdle()/JobFence::wait() (thread-local assert —
// that would deadlock the pool); blocking work goes to submitIo(), never to a
// worker job (debug builds log a warning for any job running longer than
// kSlowJobWarnUs).
//
// ---- shutdown -------------------------------------------------------------------
// Disciplined, no detached threads, no races: the destructor sets stop_, wakes
// everyone, drains already-submitted work (workers AND I/O threads finish what is
// queued), then joins every thread. Submitting during/after shutdown is a bug
// (assert in debug builds). Because the destructor drains, jobs must not reference
// objects that die before the JobSystem does.
//
// ---- determinism ----------------------------------------------------------------
// The system provides WHERE work runs; the DeterministicScheduler (sim/det_sched.h)
// declares the ORDER. A stage's systems are submitted behind a JobFence; the fence
// is the stage barrier, so stages still run strictly in order and command buffers
// still drain between stages exactly as on the JobPool path. Stealing only moves a
// job to another thread — within one stage the systems provably write disjoint
// resources, so placement cannot change the result. Threaded runs hash
// bit-identical to single-threaded runs (see the det-sched self-tests, 1/2/4/8
// workers, JobPool vs JobSystem backends).
//
// Naming: litholattice:: namespace; I-prefix only for real interfaces (none here —
// the -OR suffix is for game-style components, not engine infrastructure, and is
// deliberately not used).
#pragma once

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "litholattice/core/chase_lev.h"
#include "litholattice/core/log.h"
#include "litholattice/core/thread_debug.h"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <cstdio>
#include <cstring>
#endif

namespace litholattice {

// ---- tunables -----------------------------------------------------------------
inline constexpr size_t kJobDequeCapacity = 1024;  ///< per-thread, per-priority Chase-Lev slots
inline constexpr unsigned kMaxJobThreads = 64;     ///< sanity clamp on worker/io thread counts
/// Debug tripwire: a worker job running longer than this is logged as a bug
/// ("worker jobs must never block"). Active only with LITHOLATTICE_THREAD_DEBUG.
inline constexpr uint64_t kSlowJobWarnUs = 50000;

enum class JobPriority : uint8_t { Low = 0, Normal = 1, High = 2 };
inline constexpr unsigned kJobPriorityCount = 3;

using Job = std::function<void()>;

// splitmix32: cheap, high-quality mixer for seeding per-worker PRNG states.
inline uint32_t splitmix32(uint32_t x) {
  x += 0x9E3779B9u;
  x = (x ^ (x >> 15)) * 0x2C1B3C6Du;
  x = (x ^ (x >> 12)) * 0x297A2D39u;
  return x ^ (x >> 15);
}

#ifdef LITHOLATTICE_THREAD_DEBUG
inline constexpr bool kThreadDebug = true;
#else
inline constexpr bool kThreadDebug = false;
#endif

// ---- thread utilities -----------------------------------------------------------
/// Platform-guarded thread naming and affinity. Linux: pthread_setname_np /
/// pthread_setaffinity_np. Elsewhere: setName is a no-op, pinToCores returns false
/// (and debug-logs the skip). Names are truncated to 15 chars (Linux limit).
struct ThreadUtil {
  static void setName(const char* name) {
#if defined(__linux__)
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%s", name);
    pthread_setname_np(pthread_self(), buf);
#else
    (void)name;
#endif
    LITHO_THREAD_LOG("thread", "named '%s'", name);
  }

  /// Pin the calling thread to `mask` (bit c = core c, up to 64 cores).
  /// Returns false on non-Linux platforms (no-op).
  static bool pinToCores(uint64_t mask) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    for (unsigned c = 0; c < 64; ++c)
      if (mask & (1ULL << c)) CPU_SET(c, &set);
    const bool ok = pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
    LITHO_THREAD_LOG("thread", "affinity mask=0x%llx %s", (unsigned long long)mask,
                     ok ? "applied" : "FAILED");
    return ok;
#else
    LITHO_THREAD_LOG("thread", "affinity mask=0x%llx ignored (not Linux)",
                     (unsigned long long)mask);
    (void)mask;
    return false;
#endif
  }

  static unsigned coreCount() {
    const unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : n;
  }
};

/// RAII named thread with optional affinity: names itself, pins itself, joins on
/// destruction (never detached). Used for the input thread; available to audio and
/// render for their dedicated threads.
class NamedThread {
 public:
  template <typename Fn>
  NamedThread(const char* name, uint64_t affinityMask, Fn&& fn) {
    thread_ = std::thread([n = std::string(name), affinityMask, f = std::forward<Fn>(fn)]() mutable {
      ThreadUtil::setName(n.c_str());
      if (affinityMask != 0) ThreadUtil::pinToCores(affinityMask);
      f();
    });
  }
  ~NamedThread() {
    if (thread_.joinable()) thread_.join();  // disciplined: never detached
  }
  NamedThread(const NamedThread&) = delete;
  NamedThread& operator=(const NamedThread&) = delete;

 private:
  std::thread thread_;
};

// ---- the job system ---------------------------------------------------------------
class JobSystem {
 public:
  struct Config {
    unsigned workerThreads = 0;  ///< 0 = one per core (at least 1, at most kMaxJobThreads)
    unsigned ioThreads = 1;      ///< dedicated blocking-job threads (clamped to >= 1)
    /// Affinity masks, bit c = core c. 0 = default policy:
    /// on Linux with >= 4 cores, workers take cores 1..N-1 and I/O takes core 0,
    /// keeping realtime-adjacent work (audio/input) off the sim workers' cores.
    /// On smaller machines / non-Linux: no pinning (best effort, OS schedules).
    uint64_t workerAffinityMask = 0;
    uint64_t ioAffinityMask = 0;
    const char* namePrefix = "litho";  ///< worker names: "<prefix>-w0" ... (<=15 chars total)
  };

  // Note: no `= Config{}` default argument here — GCC rejects a nested struct with
  // default member initializers as a default argument inside the enclosing class.
  JobSystem() { init(Config{}); }
  explicit JobSystem(const Config& cfg) { init(cfg); }
  ~JobSystem() { shutdown(); }

  JobSystem(const JobSystem&) = delete;
  JobSystem& operator=(const JobSystem&) = delete;

  /// Submit a non-blocking job to the worker pool (stealable, priority-ordered).
  /// Worker jobs must NEVER block: no I/O, no waiting on game locks, no waitIdle().
  void submit(Job job, JobPriority prio = JobPriority::Normal) {
    assert(!stop_.load(std::memory_order_relaxed) && "submit during/after shutdown");
    inFlight_.fetch_add(1, std::memory_order_relaxed);
    const unsigned target =
        submitCursor_.fetch_add(1, std::memory_order_relaxed) % (unsigned)workers_.size();
    const unsigned pi = (unsigned)prio;
    if (!workers_[target]->deques[pi].pushBottom(std::move(job))) {
      // Cold path: this deque is full (1024 pending on one worker). Spill to the
      // shared per-priority overflow; a worker picks it up when its deques run
      // dry. L1 lock, O(1) push.
      std::lock_guard<std::mutex> l(overflowM_);
      overflow_[pi].push_back(std::move(job));
      LITHO_THREAD_LOG("job", "submit prio=%u worker=%u OVERFLOW depth=%zu", pi, target,
                       overflow_[pi].size());
    } else if constexpr (kThreadDebug) {
      LITHO_THREAD_LOG("job", "submit prio=%u worker=%u depth~%zu", pi, target,
                       workers_[target]->deques[pi].approxSize());
    }
    workGen_.fetch_add(1, std::memory_order_release);  // wake a sleeper; pairs with the wait predicate
    // B8 lost-wakeup issue, fixed 2026-10-23 (see the LithoKraft review doc docs/reviews/CODE_REVIEW_2026-10-23_Cortana.md, B8).
    // The empty lock on m_ before the notify is REQUIRED: a worker tests its wait predicate under m_
    // and then blocks. A worker between that check and its wait would miss a notify sent without
    // m_, sleep forever with the job queued, and a fence/waitIdle()/join() then hangs forever.
    // Proven with all-thread stacks; the tick timing handshake was investigated and ruled out.
    { std::lock_guard<std::mutex> l(m_); }
    cv_.notify_one();
  }

  /// Submit a job that may block (file/network I/O). Runs on a dedicated I/O
  /// thread, never on a worker — a blocked worker is a lost sim core.
  void submitIo(Job job) {
    assert(!stop_.load(std::memory_order_relaxed) && "submitIo during/after shutdown");
    ioInFlight_.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> l(ioM_);  // L1 leaf
      ioQueue_.push_back(std::move(job));
    }
    ioCv_.notify_one();
  }

  /// Block until every submitted job (workers + I/O) has finished. Must NOT be
  /// called from a worker/I/O job (thread-local assert: that would deadlock).
  void waitIdle() {
    assert(!t_onJobThread && "waitIdle() from a job thread would deadlock the pool");
    std::unique_lock<std::mutex> l(m_);  // L2
    idleCv_.wait(l, [this] {
      return inFlight_.load(std::memory_order_acquire) == 0 &&
             ioInFlight_.load(std::memory_order_acquire) == 0;
    });
  }

  uint64_t failedJobs() const { return failed_.load(std::memory_order_relaxed); }
  unsigned workerCount() const { return (unsigned)workers_.size(); }
  unsigned ioThreadCount() const { return (unsigned)ioThreads_.size(); }

  /// True when called on a worker or I/O thread of any JobSystem.
  static bool onJobThread() { return t_onJobThread; }

  /// Affinity mask the engine's realtime-adjacent threads (audio callback helper,
  /// input thread) should use: core 0 on Linux with >= 4 cores, else 0 (no pin).
  /// Workers are pinned away from it by the default config policy.
  static uint64_t realtimeAffinityMask() {
#if defined(__linux__)
    return ThreadUtil::coreCount() >= 4 ? 1ULL : 0ULL;
#else
    return 0ULL;
#endif
  }

 private:
  struct Worker {
    ChaseLevDeque<Job, kJobDequeCapacity> deques[kJobPriorityCount];
  };

  static thread_local bool t_onJobThread;

  void init(const Config& cfg) {
    unsigned workers = cfg.workerThreads == 0 ? ThreadUtil::coreCount() : cfg.workerThreads;
    if (workers < 1) workers = 1;
    if (workers > kMaxJobThreads) workers = kMaxJobThreads;
    unsigned ios = cfg.ioThreads < 1 ? 1 : cfg.ioThreads;
    if (ios > kMaxJobThreads) ios = kMaxJobThreads;

    const uint64_t workerMask = cfg.workerAffinityMask != 0
                                    ? cfg.workerAffinityMask
                                    : defaultWorkerMask(workers);
    const uint64_t ioMask =
        cfg.ioAffinityMask != 0 ? cfg.ioAffinityMask : defaultIoMask();

    workers_.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) workers_.push_back(std::make_unique<Worker>());
    // Per-worker steal-PRNG seeds: identical seeds made every worker's first
    // steal-victim pick collide (thundering herd on first contention).
    rngState_.resize(workers);
    for (unsigned i = 0; i < workers; ++i) rngState_[i] = splitmix32(0x9E3779B9u + i);
    workerThreads_.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) {
      std::string name = std::string(cfg.namePrefix) + "-w" + std::to_string(i);
      workerThreads_.emplace_back([this, i, name, workerMask] {
        t_onJobThread = true;
        ThreadUtil::setName(name.c_str());
        if (workerMask != 0) ThreadUtil::pinToCores(workerMask);
        LITHO_THREAD_LOG("thread", "worker %u online (%u workers)", i, workerCount());
        workerLoop(i);
        LITHO_THREAD_LOG("thread", "worker %u offline", i);
      });
    }
    ioThreads_.reserve(ios);
    for (unsigned i = 0; i < ios; ++i) {
      std::string name = std::string(cfg.namePrefix) + "-io" + std::to_string(i);
      ioThreads_.emplace_back([this, i, name, ioMask] {
        t_onJobThread = true;
        ThreadUtil::setName(name.c_str());
        if (ioMask != 0) ThreadUtil::pinToCores(ioMask);
        LITHO_THREAD_LOG("thread", "io thread %u online", i);
        ioLoop(i);
        LITHO_THREAD_LOG("thread", "io thread %u offline", i);
      });
    }
    LITHO_THREAD_LOG("thread", "JobSystem up: %u workers + %u io threads", workers, ios);
  }

  static uint64_t defaultWorkerMask(unsigned workers) {
    // Pin sim workers away from core 0 (reserved for realtime: audio/input/io)
    // when there are enough cores for it to matter.
#if defined(__linux__)
    const unsigned cores = ThreadUtil::coreCount();
    if (cores >= 4) {
      uint64_t mask = 0;
      for (unsigned c = 1; c < cores && c <= workers + 1; ++c) mask |= 1ULL << c;
      if (mask == 0) mask = ~1ULL;  // paranoia: never an empty mask
      return mask;
    }
#endif
    (void)workers;
    return 0ULL;  // no pinning: best effort, the OS schedules
  }

  static uint64_t defaultIoMask() {
#if defined(__linux__)
    if (ThreadUtil::coreCount() >= 4) return 1ULL;  // core 0, with the realtime threads
#endif
    return 0ULL;
  }

  void shutdown() {
    LITHO_THREAD_LOG("thread", "shutdown: draining %d worker + %d io jobs in flight",
                     inFlight_.load(std::memory_order_relaxed),
                     ioInFlight_.load(std::memory_order_relaxed));
    stop_.store(true, std::memory_order_release);
    workGen_.fetch_add(1, std::memory_order_release);
    // B8 lost-wakeup issue, fixed 2026-10-23 (see the LithoKraft review doc docs/reviews/CODE_REVIEW_2026-10-23_Cortana.md, B8).
    // The empty locks before the notifies are REQUIRED: a worker (or io thread) between its stop_
    // predicate check and its wait would miss a notify sent without its mutex and never wake, so
    // the join below hangs forever (the captured engine_sim hang stacks: main in join, worker in
    // cv_.wait). The tick timing handshake was investigated and ruled out.
    { std::lock_guard<std::mutex> l(m_); }    // B8 fix: same missed-wakeup window as submit()
    { std::lock_guard<std::mutex> l(ioM_); }  // io threads test stop_ under ioM_
    cv_.notify_all();
    ioCv_.notify_all();
    for (auto& t : workerThreads_) t.join();
    for (auto& t : ioThreads_) t.join();
    LITHO_THREAD_LOG("thread", "shutdown: all threads joined, failed=%llu",
                     (unsigned long long)failed_.load(std::memory_order_relaxed));
  }

  // Find work for worker `self`: own deques high->low, then steal victims in
  // pseudo-random order high->low, then the shared overflow. No locks except the
  // cold overflow path (L1).
  bool findJob(unsigned self, Job& out) {
    Worker& w = *workers_[self];
    for (int p = int(JobPriority::High); p >= int(JobPriority::Low); --p) {
      if (w.deques[p].popBottom(out)) return true;
    }
    const unsigned n = (unsigned)workers_.size();
    unsigned r = rngState_[self] = rngState_[self] * 1664525u + 1013904223u;  // xorshift-ish LCG
    const unsigned start = n == 0 ? 0 : (r >> 8) % n;
    for (unsigned i = 0; i < n; ++i) {
      const unsigned v = (start + i) % n;
      if (v == self) continue;
      Worker& vw = *workers_[v];
      for (int p = int(JobPriority::High); p >= int(JobPriority::Low); --p) {
        if (vw.deques[p].steal(out)) {
          LITHO_THREAD_LOG("job", "steal worker=%u victim=%u prio=%d", self, v, p);
          return true;
        }
      }
    }
    {
      // Cold path only: per-priority overflow, highest priority first, O(1).
      std::lock_guard<std::mutex> l(overflowM_);  // L1
      for (int p = int(JobPriority::High); p >= int(JobPriority::Low); --p) {
        if (!overflow_[p].empty()) {
          out = std::move(overflow_[p].back());
          overflow_[p].pop_back();
          LITHO_THREAD_LOG("job", "overflow drain worker=%u prio=%d remaining=%zu", self, p,
                           overflow_[p].size());
          return true;
        }
      }
    }
    return false;
  }

  void workerLoop(unsigned self) {
    uint64_t gen = workGen_.load(std::memory_order_acquire);
    for (;;) {
      Job job;
      bool found = false;
      try {
        found = findJob(self, job);
      } catch (const std::exception& e) {
        // A throwing Job copy in steal()/popBottom (e.g. OOM during the
        // std::function copy) must not kill the worker thread (review
        // A-M4: catch ALL exceptions here — a non-std throw escaped this
        // std-only catch and terminated the process).
        failed_.fetch_add(1, std::memory_order_relaxed);
        logError("job system: findJob threw: %s", e.what());
        // inFlight_ is deliberately NOT decremented, and the scan retries:
        // popBottom()/steal() copy BEFORE committing their indices, so a
        // throwing copy consumes NO job — the job is still queued and
        // still counted in inFlight_. Decrementing here (the review's
        // literal wording, written against the old copy-after-decrement
        // deque where the job WAS lost) would double-release the count
        // when the preserved job later runs through executeJob, letting
        // waitIdle() return early with work outstanding. Retrying after a
        // short backoff is what guarantees waitIdle() cannot hang on this
        // path: the preserved job is picked up by this same scan.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      } catch (...) {
        failed_.fetch_add(1, std::memory_order_relaxed);
        logError("job system: findJob threw a non-std exception");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;  // same copy-before-commit reasoning as the std branch
      }
      if (found) {
        executeJob(std::move(job), inFlight_, "worker", self);
        continue;
      }
      if (stop_.load(std::memory_order_acquire)) {
        // Shutdown drain: no sleeping here (that would busy-loop on the cv
        // predicate); a brief yield while the last jobs land, then exit.
        if (inFlight_.load(std::memory_order_acquire) == 0) return;
        std::this_thread::yield();
        continue;
      }
      // No work anywhere: sleep until a submit or shutdown. The predicate
      // compares workGen_ against the generation seen before the scan, so a
      // submit that lands between the scan and the wait cannot be missed.
      LITHO_THREAD_LOG("thread", "worker %u sleep", self);
      std::unique_lock<std::mutex> l(m_);  // L2
      cv_.wait(l, [this, &gen] {
        return stop_.load(std::memory_order_acquire) ||
               workGen_.load(std::memory_order_acquire) != gen;
      });
      gen = workGen_.load(std::memory_order_acquire);
      LITHO_THREAD_LOG("thread", "worker %u wake", self);
    }
  }

  void ioLoop(unsigned self) {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> l(ioM_);  // L1
        ioCv_.wait(l, [this] {
          return stop_.load(std::memory_order_acquire) || !ioQueue_.empty();
        });
        if (ioQueue_.empty()) return;  // stop_ is set (see predicate)
        job = std::move(ioQueue_.front());
        ioQueue_.pop_front();
      }
      executeJob(std::move(job), ioInFlight_, "io", self);
    }
  }

  // Runs one job with ZERO locks held (the lock-order rules). Exceptions are
  // caught and counted; the in-flight counter is always released so waitIdle()
  // and JobFence::wait() can never hang on a failed job.
  void executeJob(Job&& job, std::atomic<int>& counter, const char* kind, unsigned idx) {
    (void)kind;  // only used by the debug logging below
    (void)idx;
    // Declared (not defined) when LITHOLATTICE_THREAD_DEBUG is off; the
    // if-constexpr bodies are discarded there, so [[maybe_unused]] keeps -Wall quiet.
    [[maybe_unused]] uint64_t id = 0, t0 = 0;
    if constexpr (kThreadDebug) {
      id = detail::threadDebugNextJobId();
      t0 = detail::threadDebugNowUs();
      LITHO_THREAD_LOG("job", "start id=%llu %s=%u", (unsigned long long)id, kind, idx);
    }
    try {
      job();
    } catch (const std::exception& e) {
      failed_.fetch_add(1, std::memory_order_relaxed);
      logError("job system: job threw: %s", e.what());
    } catch (...) {
      failed_.fetch_add(1, std::memory_order_relaxed);
      logError("job system: job threw a non-std exception");
    }
    if constexpr (kThreadDebug) {
      const uint64_t dt = detail::threadDebugNowUs() - t0;
      LITHO_THREAD_LOG("job", "finish id=%llu %llu us", (unsigned long long)id,
                       (unsigned long long)dt);
      if (dt > kSlowJobWarnUs)
        LITHO_THREAD_LOG("job", "SLOW JOB id=%llu ran %llu us: worker jobs must never block",
                         (unsigned long long)id, (unsigned long long)dt);
    }
    if (counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      std::lock_guard<std::mutex> l(m_);  // L2, only to pair with idleCv_
      idleCv_.notify_all();
    }
  }

  std::vector<std::unique_ptr<Worker>> workers_;
  std::vector<unsigned> rngState_;  // per-worker steal PRNG state (worker-local use)
  std::vector<std::thread> workerThreads_;
  std::vector<std::thread> ioThreads_;

  // Shared per-priority overflow for full-deque spills (cold path): O(1) push/pop,
  // priority order preserved. Guarded by overflowM_ (L1 leaf).
  std::vector<Job> overflow_[kJobPriorityCount];
  std::mutex overflowM_;  // L1

  std::deque<Job> ioQueue_;
  std::mutex ioM_;                 // L1
  std::condition_variable ioCv_;

  std::mutex m_;                   // L2: sleep/wake handshake + idleCv_ pairing
  std::condition_variable cv_;      // workers sleep here
  std::condition_variable idleCv_;  // waitIdle() sleeps here
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> workGen_{0};
  std::atomic<int> inFlight_{0};
  std::atomic<int> ioInFlight_{0};
  std::atomic<unsigned> submitCursor_{0};
  std::atomic<uint64_t> failed_{0};
};

inline thread_local bool JobSystem::t_onJobThread = false;

/// A group of jobs on a JobSystem that can be waited on by itself (other pool work
/// keeps running) — the stage barrier for the deterministic scheduler. Each job's
/// exception is caught here; wait() reports how many jobs of this fence failed.
/// The destructor waits, so a fence must not outlive work it references... rather:
/// jobs may reference the fence's owner, so never let the owner die first.
class JobFence {
 public:
  explicit JobFence(JobSystem& sys) : sys_(sys) {}
  ~JobFence() { wait(); }  // jobs reference the caller: never let it die before they finish
  JobFence(const JobFence&) = delete;
  JobFence& operator=(const JobFence&) = delete;

  void submit(Job job, JobPriority prio = JobPriority::Normal) {
    {
      std::lock_guard<std::mutex> l(m_);  // L1 leaf
      ++pending_;
    }
    sys_.submit(
        [this, j = std::move(job)] {
          bool ok = true;
          try {
            j();
          } catch (const std::exception& e) {
            ok = false;
            logError("job fence: job threw: %s", e.what());
          } catch (...) {
            ok = false;
            logError("job fence: job threw a non-std exception");
          }
          std::lock_guard<std::mutex> l(m_);  // L1 leaf; never nested with system locks
          if (!ok) ++failed_;
          if (--pending_ == 0) cv_.notify_all();  // notify under the lock: the waiter may destroy *this right after
        },
        prio);
  }

  /// Blocks until every job submitted to this fence has finished. Returns the
  /// number that threw. Must not be called from a job thread (assert).
  int wait() {
    assert(!JobSystem::onJobThread() && "JobFence::wait() from a job thread would deadlock");
    std::unique_lock<std::mutex> l(m_);  // L1 leaf
    cv_.wait(l, [this] { return pending_ == 0; });
    return failed_;
  }

 private:
  JobSystem& sys_;
  std::mutex m_;
  std::condition_variable cv_;
  int pending_ = 0, failed_ = 0;
};

}  // namespace litholattice
