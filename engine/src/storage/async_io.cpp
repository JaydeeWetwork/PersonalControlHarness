// IoScheduler: async file I/O on the JobSystem's submitIo().
// See async_io.h for the completion contract.
#include "litholattice/storage/async_io.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>

#include "litholattice/core/log.h"

namespace litholattice::storage {

IoScheduler::StrandGuard::~StrandGuard() { sched->finishStrand(path); }

void IoScheduler::submitRead(std::filesystem::path path,
                             std::function<void(Bytes data, std::string err)> done,
                             IoFence* fence) {
  struct Result {
    Bytes data;
    std::string err;
  };
  auto res = std::make_shared<Result>();
  Op op;
  op.work = [path, res] {
    if (!readFileAll(path, &res->data, &res->err)) res->data.clear();
  };
  op.done = [res, done = std::move(done)]() mutable {
    done(std::move(res->data), std::move(res->err));
  };
  submitStrand(std::move(path), std::move(op), fence);
}

void IoScheduler::submitWrite(std::filesystem::path path, Bytes data, const FileWriteOptions& opts,
                              std::function<void(bool ok, std::string err)> done,
                              IoFence* fence) {
  struct Result {
    bool ok = false;
    std::string err;
  };
  auto res = std::make_shared<Result>();
  Op op;
  op.work = [path, data = std::move(data), opts, res] {
    res->ok = writeFileAtomic(path, data, &res->err, opts);
  };
  op.done = [res, done = std::move(done)]() mutable { done(res->ok, std::move(res->err)); };
  submitStrand(std::move(path), std::move(op), fence);
}

void IoScheduler::submitWriteRecord(std::filesystem::path path, Bytes payload,
                                    const FileWriteOptions& opts,
                                    std::function<void(bool ok, std::string err)> done,
                                    IoFence* fence) {
  submitWrite(std::move(path), frameRecord(payload), opts, std::move(done), fence);
}

void IoScheduler::submit(std::function<void()> work, std::function<void()> done,
                         IoFence* fence) {
  if (fence) {
    std::lock_guard<std::mutex> l(fence->m_);
    ++fence->pending_;
  }
  jobs_.submitIo([work = std::move(work), done = std::move(done), fence]() mutable {
    bool ok = true;
    try {
      work();
    } catch (...) {
      ok = false;
    }
    try {
      done();
    } catch (...) {
      ok = false;
    }
    if (fence) fence->onOpDone(ok);
    // A throwing raw op is a bug in the caller: count it on the JobSystem too.
    if (!ok) throw std::runtime_error("io scheduler: raw op threw");
  });
}

void IoScheduler::submitStrand(std::filesystem::path path, Op op, IoFence* fence) {
  // Canonical strand identity: absolute + lexically_normal, so "world/r.0.0.llr",
  // "./world/r.0.0.llr" and "world//r.0.0.llr" hash to ONE strand (and one
  // .tmp) instead of getting concurrent writers on the same file. Lexical
  // only: symlinks are not resolved (see the header contract).
  {
    std::error_code ec;
    const auto abs = std::filesystem::absolute(path, ec);
    path = (ec ? path : abs).lexically_normal();
  }
  if (fence) {
    {
      std::lock_guard<std::mutex> l(fence->m_);
      ++fence->pending_;
    }
    auto user = std::move(op.done);
    op.done = [fence, user = std::move(user)]() mutable {
      bool ok = true;
      try {
        user();
      } catch (...) {
        ok = false;  // a throwing callback counts as a failed op, like a job throw
      }
      fence->onOpDone(ok);
    };
  }
  std::lock_guard<std::mutex> l(m_);
  Strand& s = strands_[path];
  s.queue.push_back(std::move(op));
  if (!s.active) {
    s.active = true;
    launchHead(path, s);
  }
}

IoScheduler::~IoScheduler() {
  // Drain: an op's StrandGuard touches `this` on completion, so the scheduler
  // must not die mid-flight. Ops run on the JobSystem's I/O threads (which must
  // outlive us), so waiting here cannot deadlock the pool.
  std::unique_lock<std::mutex> l(m_);
  drainCv_.wait(l, [this] { return inflight_ == 0; });
}

void IoScheduler::launchHead(const std::filesystem::path& path, Strand& s) {
  Op op = std::move(s.queue.front());
  s.queue.pop_front();
  ++inflight_;
  jobs_.submitIo([this, path, op = std::move(op)]() mutable {
    StrandGuard g{this, path};
    bool ok = true;
    try {
      op.work();
    } catch (...) {
      ok = false;
    }
    try {
      op.done();
    } catch (...) {
      ok = false;  // unreachable for fence-tracked dones (they catch), belt and braces
    }
    if (!ok) throw std::runtime_error("io scheduler: strand op threw");
  });
}

void IoScheduler::finishStrand(const std::filesystem::path& path) {
  std::lock_guard<std::mutex> l(m_);
  if (--inflight_ == 0) drainCv_.notify_all();
  auto it = strands_.find(path);
  if (it == strands_.end()) return;  // cannot happen; defensive
  Strand& s = it->second;
  if (s.queue.empty()) {
    strands_.erase(it);  // idle strands cost nothing to rebuild
  } else {
    launchHead(path, s);
  }
}

// ---- IoFence ----------------------------------------------------------------------

IoFence::~IoFence() {
  // Fix 2026-10-11 (regression): wait() refuses on a job thread (returns -1
  // instead of deadlocking). But the in-flight op lambdas hold a raw `this`
  // and call onOpDone() on completion — if the destructor completes while ops
  // are pending, that's a use-after-free. Destroying an armed fence on a job
  // thread is a programming error: fail loud (abort), not silent. With zero
  // pending ops the refusal is harmless and destruction proceeds normally.
  if (wait() < 0) {
    std::lock_guard<std::mutex> l(m_);
    if (pending_ > 0) {
      logError("storage: IoFence destroyed on a job thread with %d ops pending; aborting (would UAF)",
               pending_);
      std::abort();
    }
  }
}

void IoFence::submitRead(std::filesystem::path path,
                         std::function<void(Bytes data, std::string err)> done) {
  io_.submitRead(std::move(path), std::move(done), this);
}

void IoFence::submitWrite(std::filesystem::path path, Bytes data, const FileWriteOptions& opts,
                          std::function<void(bool ok, std::string err)> done) {
  io_.submitWrite(std::move(path), std::move(data), opts, std::move(done), this);
}

void IoFence::submitWriteRecord(std::filesystem::path path, Bytes payload,
                                const FileWriteOptions& opts,
                                std::function<void(bool ok, std::string err)> done) {
  io_.submitWriteRecord(std::move(path), std::move(payload), opts, std::move(done), this);
}

void IoFence::onOpDone(bool ok) {
  std::lock_guard<std::mutex> l(m_);
  if (!ok) ++failed_;
  if (--pending_ == 0) cv_.notify_all();
}

int IoFence::wait() {
  // Runtime-checked, not just assert(): an NDEBUG build must not silently
  // deadlock the pool. Returns -1 when refused (never 0-vs-failed ambiguity:
  // a real wait returns the failed-op count, >= 0).
  if (JobSystem::onJobThread()) {
    logError("storage: IoFence::wait() called from a job thread; refusing (would deadlock)");
    return -1;
  }
  std::unique_lock<std::mutex> l(m_);
  cv_.wait(l, [this] { return pending_ == 0; });
  return failed_;
}

}  // namespace litholattice::storage
