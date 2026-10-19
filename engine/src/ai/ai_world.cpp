#include "litholattice/ai/ai_world.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>

namespace litholattice::ai {

AiWorld::AiWorld(const WalkCache& nav, AiConfig cfg, JobPool* pool) : nav_(nav), cfg_(cfg), hpa_(nav), pool_(pool) {
  cfg_.hz = std::clamp(cfg_.hz, 8, 16);
  cfg_.mobCap = std::clamp(cfg_.mobCap, 1, 128);
  cfg_.planRange = std::clamp(cfg_.planRange, 32, 64);
  cfg_.pathsPerTick = std::max(1, cfg_.pathsPerTick);
}

std::optional<uint32_t> AiWorld::spawn(Cell at, const IMovementMode* mode, BtPtr brain) {
  if (int(mobs_.size()) >= cfg_.mobCap) return std::nullopt;
  const uint32_t id = nextId_++;
  Mob m;
  m.id = id; m.pos = at; m.mode = mode; m.brain = std::move(brain);
  mobs_.emplace(id, std::move(m));
  return id;
}
bool AiWorld::despawn(uint32_t id) {
  if (ticking_) {
    // Deferred: tick() is iterating mobs_ and may hold a Mob& to this entry.
    if (mobs_.count(id)) { pendingDespawn_.push_back(id); return true; }
    return false;
  }
  return mobs_.erase(id) > 0;
}
Mob* AiWorld::mob(uint32_t id) { auto it = mobs_.find(id); return it == mobs_.end() ? nullptr : &it->second; }
void AiWorld::setGoal(uint32_t id, Cell goal) {
  if (Mob* m = mob(id)) { m->goal = goal; m->wantPath = true; m->path.clear(); m->pathSteps.clear(); m->pathAt = 0; }
}
std::optional<Cell> AiWorld::waypoint(const std::string& name) const {
  auto it = waypoints_.find(name);
  return it == waypoints_.end() ? std::nullopt : std::optional<Cell>(it->second);
}
bool AiWorld::requestDrop(uint32_t id, uint32_t item) const {
  auto it = mobs_.find(id);
  return it != mobs_.end() && boss().allowDrop(it->second, item);
}

void AiWorld::advance(int simTicks, int worldTps) {
  if (worldTps <= 0) return;
  acc_ += double(simTicks) * double(cfg_.hz) / double(worldTps);
  while (acc_ >= 1.0) { acc_ -= 1.0; tick(); }
}

void AiWorld::tick() {
  const auto t0 = std::chrono::steady_clock::now();
  ticking_ = true;
  ++tick_;
  AiStats st;
  st.ticks = tick_;
  st.mobs = int(mobs_.size());
  const float surge = boss().surge(tick_);
  // 1) Think (id order, LOD by distance to the nearest observer).
  for (auto& [id, m] : mobs_) {
    int dist = observers_.empty() ? 0 : 1 << 30;
    for (const Cell& o : observers_) dist = std::min(dist, std::max(std::abs(o.x - m.pos.x), std::abs(o.z - m.pos.z)));
    m.lodEvery = dist <= cfg_.lodNear ? 1 : dist <= cfg_.lodFar ? 2 : 4;
    if (!m.brain || (tick_ + id) % uint64_t(m.lodEvery)) continue;
    AiContext ctx{*this, m, AiRng{cfg_.seed * 0x9e3779b97f4a7c15ull ^ (uint64_t(id) << 32) ^ tick_}, tick_, surge, {}};
    const std::optional<Cell> before = m.goal;
    m.brain->tick(ctx);
    if (m.goal && (!before || !(*m.goal == *before))) { m.wantPath = true; m.path.clear(); m.pathSteps.clear(); m.pathAt = 0; }
    ++m.thoughts;
    ++st.thought;
  }
  // 2) Plan: up to the budget, in id order; searches run in parallel, results applied in order.
  // H-AI-2 GUARD: `want` holds Mob* across the parallel plan phase and the
  // apply loop below. That is safe ONLY because (a) mobs_ is a std::map
  // (node-stable: inserts never invalidate other elements' addresses) and
  // (b) despawns during tick() are deferred to the flush at the end of this
  // function (see despawn()), so nothing erases a Mob mid-phase. Do NOT add
  // a mid-tick erase, or change the mobs_ container (unordered_map/vector),
  // without reworking this -- every pointer in `want` would dangle.
  std::vector<Mob*> want;
  for (auto& [id, m] : mobs_)
    if (m.wantPath && m.goal && m.mode) want.push_back(&m);
  const size_t n = std::min(want.size(), size_t(cfg_.pathsPerTick));
  st.pathsRun = int(n);
  st.pathsDeferred = int(want.size() - n);
  std::vector<Path> results(n);
  std::vector<char> served(n, 0);  // P4: indices pre-filled below (flags off -> all zero)
  if (cfg_.useFlowField || cfg_.useHpa) {
    for (size_t i = 0; i < n; ++i) {
      Mob& m = *want[i];
      if (cfg_.useFlowField) {
        const Cell g = *m.goal;
        if (!flow_ || !(flowGoal_ == g) || flowGen_ != nav_.generation()) {
          flow_.emplace();
          flowOk_ = flow_->build(nav_, g, cfg_.flowRange);
          flowGoal_ = g;
          flowGen_ = nav_.generation();
        }
        if (flowOk_ && flow_->dist(m.pos) != FlowField::kUnreached) {
          Path p;
          p.cells.push_back(m.pos);
          p.steps.push_back(StepTag::kStepStart);
          Cell cur = m.pos;
          while (int(p.cells.size()) <= cfg_.planRange && !(cur == g)) {
            const std::optional<Cell> nx = flow_->next(cur);
            if (!nx) break;
            const int dy = nx->y - cur.y;
            p.steps.push_back(dy > 0 ? StepTag::kStep : dy < 0 ? StepTag::kDrop : StepTag::kWalk);
            p.cells.push_back(*nx);
            cur = *nx;
          }
          if (p.cells.size() > 1) {
            p.complete = (cur == g);
            results[i] = std::move(p);
            served[i] = 1;
            continue;
          }
        }
      }
      if (cfg_.useHpa) {
        const int dx = std::abs(m.goal->x - m.pos.x), dy = std::abs(m.goal->y - m.pos.y),
                  dz = std::abs(m.goal->z - m.pos.z);
        if (std::max({dx, dy, dz}) > cfg_.planRange) {
          if (std::optional<Path> hp = hpa_.findPath(m.pos, *m.goal, m.mode)) {
            results[i] = std::move(*hp);
            served[i] = 1;
          }
        }
      }
    }
  }
  // B-H3 (2026-10-15): prewarm, HERE on the tick thread, every column a
  // pooled search can touch BEFORE any job is submitted -- the invariant
  // is that a JobPool worker never triggers a first-compute of a column
  // (a first-compute reads the live ChunkMap, which the mutating owner
  // thread may be writing concurrently: a data race, UB -- see the
  // WalkCache thread contract in nav.h). The search box: findPath keeps
  // every candidate cell within planRange of `from` on x/z (y is free --
  // a column spans the full height), and movement modes probe a few
  // cells beyond a candidate (widest: Leap gap jumps, 4 cells), so the
  // box is widened by one whole column (16 cells) of margin on every
  // side. Columns are unioned across the whole batch and prewarmed once
  // each, in sorted (deterministic) order; an already-cached column is
  // a cheap lookup, so the steady-state cost is only genuinely new
  // columns at the frontier. Searches already served above (flow/HPA)
  // ran synchronously on this thread and need no prewarm; with no pool
  // (or a single search) the job below runs on this thread too, so the
  // prewarm is skipped entirely. Hop-chained replans are covered: they
  // are new searches in a later tick(), which prewarms its own box.
  if (pool_ && n > 1) {
    std::vector<uint64_t> cols;
    for (size_t i = 0; i < n; ++i) {
      if (served[i]) continue;
      const Mob& m = *want[i];
      const int r = cfg_.planRange + 16;  // plan box + one-column margin
      const int cx0 = (m.pos.x - r) >> 4, cx1 = (m.pos.x + r) >> 4;
      const int cz0 = (m.pos.z - r) >> 4, cz1 = (m.pos.z + r) >> 4;
      for (int cz = cz0; cz <= cz1; ++cz)
        for (int cx = cx0; cx <= cx1; ++cx)
          cols.push_back((uint64_t(uint32_t(cx)) << 32) | uint32_t(cz));
    }
    std::sort(cols.begin(), cols.end());
    cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
    for (const uint64_t k : cols)
      nav_.prewarm(int(int32_t(uint32_t(k >> 32))), int(int32_t(uint32_t(k))));
  }
  auto job = [&](size_t i) {
    if (served[i]) return;
    const Mob& m = *want[i];
    results[i] = findPath(nav_, {m.pos, hopToward(m.pos, *m.goal, cfg_.planRange - 8), m.mode, cfg_.planRange, cfg_.nodesPerPath});
  };
  if (pool_ && n > 1) {
    std::mutex mx;
    std::condition_variable cv;
    size_t left = n;
    for (size_t i = 0; i < n; ++i)
      pool_->submit([&, i] {
        // Decrement even if job() throws (JobPool catches): otherwise a single
        // throwing path job hangs cv.wait forever.
        struct DoneGuard {
          std::mutex& mx;
          size_t& left;
          std::condition_variable& cv;
          ~DoneGuard() {
            std::lock_guard l(mx);
            if (--left == 0) cv.notify_one();
          }
        } done{mx, left, cv};
        job(i);
      });
    std::unique_lock l(mx);
    cv.wait(l, [&] { return left == 0; });
  } else {
    for (size_t i = 0; i < n; ++i) job(i);
  }
  for (size_t i = 0; i < n; ++i) {
    Mob& m = *want[i];
    m.path = std::move(results[i].cells);
    m.pathSteps = std::move(results[i].steps);
    m.pathAt = 0;
    m.wantPath = false;  // replanned (next hop) when this path runs out short of the goal
  }
  // 3) Move: one step per tick (two during a surge).
  const int steps = surge > 1.f ? 2 : 1;
  for (auto& [id, m] : mobs_) {
    for (int s = 0; s < steps && m.pathAt + 1 < m.path.size(); ++s) m.pos = m.path[++m.pathAt];
    if (m.goal && m.pathAt + 1 >= m.path.size() && !m.wantPath) {
      if (m.pos == *m.goal) m.goal.reset();
      else m.wantPath = true;  // next hop
    }
  }
  st.tickMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  stats_ = st;
  // Flush deferred despawns now that no iteration holds Mob references.
  ticking_ = false;
  for (uint32_t id : pendingDespawn_) mobs_.erase(id);
  pendingDespawn_.clear();
}

}  // namespace litholattice::ai
