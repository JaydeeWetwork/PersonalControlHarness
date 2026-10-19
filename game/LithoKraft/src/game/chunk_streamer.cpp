#include "lithokraft/game/chunk_streamer.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>
#include "lithokraft/game/world_save.h"
#include "litholattice/core/log.h"
#include "litholattice/world/chunk.h"

namespace lithokraft {

namespace {
/// SW-H3: RAII in-flight accounting for one streamer job. The counter is
/// decremented on EVERY exit from the job lambda — normal return, early
/// return, and exception unwinding alike (JobPool catches the exception
/// outside the lambda, after this destructor has run). Unless the job marks
/// success, the key is also reported on the failure queue so update() can
/// un-request it and schedule a retry (B-L7); a throwing job can therefore
/// neither leak the in-flight count (which used to stall all streaming at
/// the kMaxInFlight gate) nor wedge the key in requested_ forever.
struct JobGuard {
  std::shared_ptr<std::atomic<int>> inFlight;
  std::shared_ptr<litholattice::HandoffQueue<uint64_t>> failed;
  uint64_t key = 0;
  bool success = false;
  JobGuard(std::shared_ptr<std::atomic<int>> n, std::shared_ptr<litholattice::HandoffQueue<uint64_t>> f, uint64_t k)
      : inFlight(std::move(n)), failed(std::move(f)), key(k) {}
  JobGuard(const JobGuard&) = delete;
  JobGuard& operator=(const JobGuard&) = delete;
  ~JobGuard() {
    if (!success) failed->push(key);
    inFlight->fetch_sub(1, std::memory_order_relaxed);
  }
};
}  // namespace

ChunkStreamer::ChunkStreamer(litholattice::JobPool& pool, std::shared_ptr<const litholattice::ColumnGenerator> gen, WorldSave* save,
                             std::string dimensionId, int radiusChunks, const std::unordered_set<uint64_t>& initial)
    : pool_(pool),
      gen_(std::move(gen)),
      save_(save),
      dimensionId_(std::move(dimensionId)),
      radius_(std::max(0, radiusChunks)),
      requested_(initial) {
  initialCount_ = initial.size();
  // B-L2 (P3-2) setup enforcement: the pre-baked columns this streamer
  // inherits must sit inside the packPos range; an out-of-range column's
  // cells would alias onto other cells' signal/light keys. Loud at setup
  // (dimension registration is the other enforcement point — a dimension
  // must declare bounds inside [-2^20, 2^20-1] blocks; see chunk.h).
  for (uint64_t key : initial) {
    const int cx = int(int32_t(key >> 32)), cz = int(int32_t(key & 0xffffffffu));
    if (!centreInRange(cx, cz))
      logError("stream: dimension '%s' pre-baked column %d,%d is OUTSIDE the packPos range "
               "(blocks must stay within [%d, %d]); its cells would alias in packPos-keyed sets",
               dimensionId_.c_str(), cx, cz, litholattice::kPackPosCoordMin, litholattice::kPackPosCoordMax);
  }
}

bool ChunkStreamer::centreInRange(int cx, int cz) {
  const int64_t x0 = int64_t(cx) * 16, z0 = int64_t(cz) * 16;
  return x0 >= litholattice::kPackPosCoordMin && x0 + 15 <= litholattice::kPackPosCoordMax &&
         z0 >= litholattice::kPackPosCoordMin && z0 + 15 <= litholattice::kPackPosCoordMax;
}

ChunkStreamer::~ChunkStreamer() {
  // Jobs hold shared_ptr copies of the queues/counters, so they can always
  // finish safely; wait them out so no job touches a dead streamer — but
  // BOUNDED (SW-H3): a stuck job must not hang exit / dimension switch.
  // Mirroring the timing-handshake stop policy's spirit (bounded join, loud
  // log, no silent detach): after kDrainTimeout we log loudly and return;
  // the orphaned job still only touches shared_ptr-owned state and the
  // generator (shared_ptr), and in every Game teardown path the WorldSave
  // and the JobPool outlive the streamer, so it finishes harmlessly.
  const auto deadline = std::chrono::steady_clock::now() + kDrainTimeout;
  while (inFlight_->load(std::memory_order_relaxed) > 0) {
    if (std::chrono::steady_clock::now() >= deadline) {
      logError("stream: streamer destroyed with %d job(s) still in flight after %lld ms — NOT waiting longer; "
               "the job(s) finish into orphaned queues (see chunk_streamer.h)",
               inFlight_->load(std::memory_order_relaxed),
               static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(kDrainTimeout).count()));
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

bool ChunkStreamer::requestable(uint64_t key, std::chrono::steady_clock::time_point now) {
  if (requested_.count(key)) return false;
  if (const auto it = retries_.find(key); it != retries_.end() && now < it->second.nextTry) return false;
  return true;
}

void ChunkStreamer::pumpRequests(std::vector<std::pair<int, uint64_t>> want) {
  // B-L7: collect job failures first — un-request those keys and arm their
  // retry backoff, so a transient fault heals on a later update instead of
  // leaving a permanent hole for the session.
  {
    std::vector<uint64_t> failedKeys;
    failed_->drainInto(failedKeys);
    const auto now = std::chrono::steady_clock::now();
    for (uint64_t key : failedKeys) {
      requested_.erase(key);
      Retry& r = retries_[key];
      ++r.attempts;
      const int shift = std::min(r.attempts, 6);  // 1s, 2s, 4s ... capped below
      const auto delay = std::min(std::chrono::milliseconds(500 << shift), std::chrono::milliseconds(30000));
      r.nextTry = now + delay;
    }
  }
  // Nearest-first: when something crosses into ungenerated territory the
  // ground underfoot streams in before the horizon does.
  constexpr int kMaxInFlight = 32;  // leave pool capacity for meshing
  std::sort(want.begin(), want.end());
  for (const auto& [dist2, key] : want) {
    (void)dist2;
    if (inFlight_->load(std::memory_order_relaxed) >= kMaxInFlight) break;
    const int cx = int(int32_t(key >> 32)), cz = int(int32_t(key & 0xffffffffu));
    if (!centreInRange(cx, cz)) continue;  // B-L2: per-column edge guard (a wide radius near the bound)
    if (!requested_.insert(key).second) continue;
    inFlight_->fetch_add(1, std::memory_order_relaxed);
    WorldSave* save = save_;
    const std::string dim = dimensionId_;
    pool_.submit([save, dim, cx, cz, key, done = done_, failed = failed_, n = inFlight_, ready = readyCount_, gen = gen_] {
      JobGuard guard{n, failed, key};  // SW-H3: decrements (and reports failure) on ALL exits
      try {
        // Disk first: a column the player edited in an earlier session must come
        // back as saved, not as fresh terrain (SW-H1: its SGNL payload rides
        // along in the result and is imported by the Game on insertion).
        if (save) {
          WorldSave::LoadedColumn loaded = save->loadColumn(dim, cx, cz);
          if (loaded.status == WorldSave::ColumnLoad::Loaded) {
            done->push(StreamedColumn{std::move(loaded.column), std::move(loaded.signal), true});
            ready->fetch_add(1, std::memory_order_relaxed);
            guard.success = true;
            return;
          }
          if (loaded.status == WorldSave::ColumnLoad::Unreadable) {
            // B-M3 fail closed: a saved copy exists but cannot be read. Leave
            // a hole and retry later — NEVER generate over the saved column.
            logError("stream: column %d,%d: saved copy unreadable — left empty, will retry (never generated over)", cx, cz);
            return;
          }
          // NotSaved: fall through to generation, the common case.
        }
        std::unique_ptr<litholattice::ChunkColumn> col = gen->generate(cx, cz);
        if (col) {
          done->push(StreamedColumn{std::move(col), {}, false});
          ready->fetch_add(1, std::memory_order_relaxed);
          guard.success = true;
        } else {
          logError("stream: column %d,%d failed to generate (left empty, will retry)", cx, cz);
        }
      } catch (const std::exception& e) {
        logError("stream: column %d,%d job threw: %s (left empty, will retry)", cx, cz, e.what());
      } catch (...) {
        logError("stream: column %d,%d job threw a non-std exception (left empty, will retry)", cx, cz);
      }
    });
  }
}

void ChunkStreamer::update(int playerCx, int playerCz) {
  if (!gen_) return;
  if (!centreInRange(playerCx, playerCz)) {  // B-L2: refuse out-of-packPos-range centres
    if (!boundWarned_) {
      boundWarned_ = true;
      logError("stream: request centre %d,%d is outside the packPos range (blocks [%d, %d]); "
               "refusing to stream there (cells would alias in packPos-keyed sets)",
               playerCx, playerCz, litholattice::kPackPosCoordMin, litholattice::kPackPosCoordMax);
    }
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  std::vector<std::pair<int, uint64_t>> want;  // (distance^2 in chunks, key)
  for (int dz = -radius_; dz <= radius_; ++dz)
    for (int dx = -radius_; dx <= radius_; ++dx) {
      const uint64_t key = litholattice::ChunkKey::make(playerCx + dx, playerCz + dz);
      if (requestable(key, now)) want.emplace_back(dx * dx + dz * dz, key);
    }
  pumpRequests(std::move(want));
}

void ChunkStreamer::updateAround(int cx, int cz, int radiusChunks) {
  if (!gen_) return;
  if (!centreInRange(cx, cz)) return;  // B-L2: same refusal as update() (logged there / at setup)
  const int r = std::clamp(radiusChunks, 0, radius_);
  const auto now = std::chrono::steady_clock::now();
  std::vector<std::pair<int, uint64_t>> want;
  for (int dz = -r; dz <= r; ++dz)
    for (int dx = -r; dx <= r; ++dx) {
      const uint64_t key = litholattice::ChunkKey::make(cx + dx, cz + dz);
      if (requestable(key, now)) want.emplace_back(dx * dx + dz * dz, key);
    }
  pumpRequests(std::move(want));
}

void ChunkStreamer::requestColumn(int cx, int cz) {
  if (!gen_) return;
  if (!centreInRange(cx, cz)) return;  // B-L2: same refusal as update()
  const uint64_t key = litholattice::ChunkKey::make(cx, cz);
  if (!requestable(key, std::chrono::steady_clock::now())) return;
  pumpRequests({{0, key}});
}

void ChunkStreamer::forgetColumn(int cx, int cz) {
  const uint64_t key = litholattice::ChunkKey::make(cx, cz);
  requested_.erase(key);
  retries_.erase(key);
  std::erase_if(staged_, [&](const StreamedColumn& sc) {
    return sc.column && sc.column->cx == cx && sc.column->cz == cz;
  });
}

std::vector<uint64_t> selectEvictionCandidates(const std::vector<uint64_t>& residentKeys, int centerCx, int centerCz,
                                               int streamRadius, int hysteresisChunks, size_t budget) {
  const int limit = streamRadius + hysteresisChunks;
  std::vector<std::pair<int, uint64_t>> far;  // (distance, key)
  for (uint64_t key : residentKeys) {
    const int cx = int(int32_t(key >> 32)), cz = int(int32_t(key & 0xffffffffu));
    const int d = std::max(std::abs(cx - centerCx), std::abs(cz - centerCz));
    if (d > limit) far.emplace_back(d, key);
  }
  std::sort(far.begin(), far.end(), [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first > b.first : a.second < b.second;  // farthest first, key tiebreak
  });
  std::vector<uint64_t> out;
  for (const auto& [d, key] : far) {
    if (out.size() >= budget) break;
    out.push_back(key);
  }
  return out;
}

std::vector<StreamedColumn> ChunkStreamer::takeReady() { return takeReadyUpTo(SIZE_MAX); }

std::vector<StreamedColumn> ChunkStreamer::takeReadyUpTo(size_t max) {
  // Move everything the jobs finished into the main-thread staging buffer
  // first (this is where retry backoff is cleared, exactly as the old
  // unbounded takeReady did), then hand out at most `max`. The remainder
  // stays staged: hasReady() keeps reporting it, so the caller's next pump
  // inserts the next bounded batch — never one giant sim-stopped window.
  std::vector<StreamedColumn> freshCols;
  done_->drainInto(freshCols);
  readyCount_->fetch_sub(int(freshCols.size()), std::memory_order_relaxed);
  for (const StreamedColumn& sc : freshCols)
    if (sc.column) retries_.erase(litholattice::ChunkKey::make(sc.column->cx, sc.column->cz));  // success clears the backoff
  for (auto& sc : freshCols) staged_.push_back(std::move(sc));
  const size_t n = std::min(max, staged_.size());
  std::vector<StreamedColumn> out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) out.push_back(std::move(staged_[i]));
  staged_.erase(staged_.begin(), staged_.begin() + std::ptrdiff_t(n));
  return out;
}

}  // namespace lithokraft
