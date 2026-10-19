// Async file I/O on the JobSystem: litholattice::storage::IoScheduler.
//
// ==== THE COMPLETION CONTRACT (read this before submitting I/O) ====
// Who runs the work: the JobSystem's dedicated I/O threads, via submitIo().
// Blocking file work NEVER runs on a worker thread, and the sim/tick threads
// NEVER block on disk: every submit*() below returns immediately; waiting is
// opt-in through IoFence, which must not be waited on from a job thread.
//
// Who gets notified: the `done` callback you pass, invoked on an I/O thread
// immediately after its op finishes. Callbacks run on I/O threads, NEVER on the
// sim/tick/worker threads. A callback must be short and non-blocking — the
// pattern is push-into-a-lock-free-queue (SpscQueue, the InputPump idiom) and
// let the tick thread drain it once per frame. A callback must never wait on
// sim state, take game locks, or call IoFence::wait()/JobSystem::waitIdle()
// (thread-local assert: that would deadlock the pool). Exceptions thrown by a
// callback are caught and counted as failures.
//
// Ordering guarantees:
//  - Per path: FIFO. Ops submitted to the SAME path form a strand: each op
//    starts only after the previous op to that path finished (callback included),
//    so a write's completion is visible before the next op on that path begins.
//  - Across different paths: no ordering guarantee. Ops on different paths may
//    run concurrently on different I/O threads.
//  - Unpathed submit(): no ordering relative to anything; use IoFence to group.
//
// Failure model: failures never throw. A failed op reports (ok=false, err) on its
// callback and is counted by IoFence::wait(). Crash safety of the writes comes
// from the file contract (file_contract.h): temp + fsync + atomic rename, with
// optional backup rotation.
//
// Lifetime: ops may outlive the caller's stack frame but not the IoScheduler or
// the JobSystem. The scheduler holds no JobSystem ownership — construct it after
// the JobSystem and destroy it before.
#pragma once

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "litholattice/core/job_system.h"
#include "litholattice/storage/file_contract.h"

namespace litholattice::storage {

class IoScheduler;

/// A group of I/O ops on an IoScheduler that can be waited on by itself — the
/// async counterpart of JobFence. The destructor waits, so a fence must not
/// outlive the data its callbacks touch.
class IoFence {
 public:
  explicit IoFence(IoScheduler& io) : io_(io) {}
  ~IoFence();  // defined in async_io.cpp: fail-closed on job-thread misuse
  IoFence(const IoFence&) = delete;
  IoFence& operator=(const IoFence&) = delete;

  /// Read `path` asynchronously; `done(data, err)` runs on an I/O thread.
  /// Empty data + non-empty err = failure. Registered on this fence.
  void submitRead(std::filesystem::path path,
                  std::function<void(Bytes data, std::string err)> done);
  /// Crash-safe write of `data` to `path` (see FileWriteOptions). `done(ok, err)`
  /// runs on an I/O thread. Registered on this fence.
  void submitWrite(std::filesystem::path path, Bytes data, const FileWriteOptions& opts,
                   std::function<void(bool ok, std::string err)> done);
  /// Like submitWrite but CRC32-frames `payload` first (see frameRecord).
  void submitWriteRecord(std::filesystem::path path, Bytes payload, const FileWriteOptions& opts,
                         std::function<void(bool ok, std::string err)> done);

  /// Blocks the caller until every op submitted to this fence has finished.
  /// Returns how many failed (threw or reported failure), or -1 if refused.
  /// Must not be called from a job thread (runtime-checked: logs and returns
  /// -1 instead of deadlocking the pool).
  int wait();

 private:
  friend class IoScheduler;
  void onOpDone(bool ok);

  IoScheduler& io_;
  std::mutex m_;
  std::condition_variable cv_;
  int pending_ = 0, failed_ = 0;
};

/// Routes file I/O through the JobSystem's submitIo(). See the completion
/// contract at the top of this file.
class IoScheduler {
 public:
  explicit IoScheduler(JobSystem& jobs) : jobs_(jobs) {}
  /// Drains: waits for in-flight ops to finish before destroying strands.
  /// The JobSystem must outlive the scheduler (ops run on its I/O threads).
  ~IoScheduler();
  IoScheduler(const IoScheduler&) = delete;
  IoScheduler& operator=(const IoScheduler&) = delete;

  /// Read `path` asynchronously; `done(data, err)` runs on an I/O thread.
  /// Ops to the same path run in submission order (strand); different paths may
  /// run concurrently. When `fence` is non-null the op is counted on it.
  void submitRead(std::filesystem::path path,
                  std::function<void(Bytes data, std::string err)> done,
                  IoFence* fence = nullptr);

  /// Crash-safe write (file contract: temp + fsync + rename). Same strand and
  /// callback rules as submitRead.
  void submitWrite(std::filesystem::path path, Bytes data, const FileWriteOptions& opts,
                   std::function<void(bool ok, std::string err)> done,
                   IoFence* fence = nullptr);

  /// CRC32-framed write (frameRecord, then the contract write).
  void submitWriteRecord(std::filesystem::path path, Bytes payload, const FileWriteOptions& opts,
                         std::function<void(bool ok, std::string err)> done,
                         IoFence* fence = nullptr);

  /// A raw op with no path strand: `work` runs on an I/O thread, then `done()`
  /// runs on the same thread. For file work prefer the pathed submits (ordering).
  void submit(std::function<void()> work, std::function<void()> done, IoFence* fence = nullptr);

  JobSystem& jobs() { return jobs_; }

 private:
  friend class IoFence;
  struct Op {
    std::function<void()> work;  // the blocking part
    std::function<void()> done;  // runs right after work, on the same I/O thread
  };
  struct Strand {
    std::deque<Op> queue;
    bool active = false;  // an op from this strand is currently on the JobSystem
  };
  // RAII: advances the strand even if the op throws, so one bad op can never
  // wedge its path's queue. Nested (not anonymous-namespace) for private access.
  struct StrandGuard {
    IoScheduler* sched;
    std::filesystem::path path;
    ~StrandGuard();
  };
  // `fence` may be null. Wraps op.done so the fence is always notified, even if
  // the user callback throws.
  void submitStrand(std::filesystem::path path, Op op, IoFence* fence);
  void launchHead(const std::filesystem::path& path, Strand& s);  // m_ held
  void finishStrand(const std::filesystem::path& path);

  JobSystem& jobs_;
  std::mutex m_;
  std::condition_variable drainCv_;
  int inflight_ = 0;  ///< ops currently on the JobSystem (m_ held)
  std::map<std::filesystem::path, Strand> strands_;  // one FIFO per path, keyed by
                                                     // the canonical spelling
                                                     // (absolute + lexically_normal):
                                                     // all writers to a path go
                                                     // through one scheduler
};

}  // namespace litholattice::storage
