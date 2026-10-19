#include "litholattice/sim/sim_thread.h"
#include "litholattice/sim/control_stream.h"
#include <cstdio>
#include <string>
#include "litholattice/sim/random_ticks.h"
#include "litholattice/world/raycast.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include "litholattice/core/time.h"
#include "litholattice/core/timer_resolution.h"
#include "litholattice/core/fail_fast.h"
#include "litholattice/core/log.h"

namespace litholattice {
using Ns = std::chrono::nanoseconds;

namespace {
// A bad rate used to die as SIGFPE (integer divide by zero) with no report.
// Fail-fast instead: log the report, then halt. Never silently continue.
bool isPow2Rate(int v) { return v > 0 && (v & (v - 1)) == 0; }

[[noreturn]] void badRate(const char* kind, int value, const char* file, int line,
                          const char* rule = "must be a positive power of two") {
  SimCorruptionReport r;
  r.what = kind;
  r.subsystem = "sim_thread";
  r.file = file;
  r.line = line;
  r.tick = kUnknownTick;
  r.detail = std::string(rule) + "; got " + std::to_string(value);
  simFailFast(r);
}
}  // namespace

void SimThread::validateTickRate(int tps) {
  // Review A-L1: a bare pow2 check accepted rates like 2^30, whose master
  // step Ns(1e9/masterHz) rounds to 0 — the catch-up clamp then divided by
  // that zero step (SIGFPE, the class badRate exists to prevent). The world
  // rate must be one of the supported tick modes (kTickModes ∪ {512}: 512
  // is the handshake/perf harness's single-domain rate, masterHz there is
  // the rate itself). Every accepted rate keeps step >= Ns(1e9/512) > 0
  // because domain rates are capped at kMaxDomainHz in the ctor.
  bool ok = (tps == 512);
  for (int m : kTickModes) ok = ok || (tps == m);
  if (!ok)
    badRate("invalid-tick-rate", tps, "sim_thread.h", 0,
            "must be one of the supported tick modes (32/64/128/256/512)");
}

SimThread::SimThread(ChunkMap& chunks, const BlockRegistry& blocks, std::unique_ptr<IWorldSimulation> simulation,
                     SimConfig config, SubmitMesh submit)
    : chunks_(chunks), blocks_(blocks), sim_(std::move(simulation)), radius_(config.radiusChunks), epoch_(config.epoch),
      belowWorld_(config.belowWorldFill), submit_(std::move(submit)), requestedTps_(config.tickRate) {
  validateTickRate(config.tickRate);
  if (config.domains.size() > size_t(kMaxClockDomains)) config.domains.resize(size_t(kMaxClockDomains));
  domains_ = std::move(config.domains);
  for (size_t i = 0; i < domains_.size(); ++i) {
    if (!isPow2Rate(domains_[i].hz)) badRate("invalid-clock-domain", domains_[i].hz, "sim_thread.cpp", 0);
    // Review A-L1: kMaxDomainHz was defined in clock_domain.h but never
    // enforced anywhere. A domain above it would push masterHz past the
    // tick rate's own bound and shrink the master step toward zero.
    if (domains_[i].hz > kMaxDomainHz)
      badRate("invalid-clock-domain", domains_[i].hz, "sim_thread.cpp", 0,
              "must not exceed kMaxDomainHz (512)");
  }
  for (const ClockDomain& d : domains_) domainInitial_.push_back(d.enabled);
  randomPerSection_ = config.randomTicksPerSection;
  control_ = std::make_unique<ControlStream>();
  randomTickable_ = randomTickableBlocks(blocks_);
  chunks_.setRegistry(&blocks_);  // ChunkMap translates BlockId <-> StableId through this
  sim_->scan(chunks_);
  // ---- Timing Handshake (the standard path since 2026-10-14): constructed
  // when the switch is on (the default); legacy mode constructs nothing and
  // registers nothing.
  handshakeMode_ = config.timingHandshake;
  if (handshakeMode_) {
    int masterHz = config.tickRate;
    for (const ClockDomain& d : domains_) masterHz = std::max(masterHz, d.hz);
    TimingHandshakeAuthority::Config hc;
    hc.phasesPerCycle = config.handshakePhasesPerCycle;
    hc.workerThreads = config.handshakeWorkerThreads;
    hc.pureSpin = config.handshakePureSpin;  // review A-L9: D9's A/B flag was unreachable from SimConfig
    // Nominal phase windows, per group from its own period (review A-M2;
    // the old formula divided the master-step period by phasesPerCycle,
    // double-dividing for Signal and mis-budgeting both groups):
    // - Group 1 (Signal): phasesPerCycle phases run per WORLD tick
    //   (config.tickRate is world TPS), so a phase's window is the
    //   world-tick period divided by the phases per cycle.
    // - Group 2 (clock domains): one phase runs per master step, so its
    //   window is the master-step period itself — no second division.
    // The authority judges repeat offenders against each group's cycle
    // budget (group 1: phase window x phasesPerCycle = the world-tick
    // period; group 2: phase window x1 = the master-step period).
    hc.phaseBudgetGroup1 = Ns(1'000'000'000LL / std::max(1, config.tickRate) /
                              std::max(1, config.handshakePhasesPerCycle));
    hc.phaseBudgetGroup2 = Ns(1'000'000'000LL / std::max(1, masterHz));
    hc.phaseBudget = hc.phaseBudgetGroup2;  // fallback for group 0 / other groups: the master-step window (hs-design §1.6)
    if (config.handshakeWatchdogMs > 0) hc.watchdogCeiling = std::chrono::milliseconds(config.handshakeWatchdogMs);
    handshake_ = std::make_unique<TimingHandshakeAuthority>(std::move(hc));
    handshake_->setFreezeCallback([](const HsFreezeInfo& info) {
      // FREEZE is a recoverable stop-the-world, NOT corruption: report via
      // the fail-fast *formatting* path only — simFailFast must not run
      // (hs-design §4.4; the world is consistent at the last barrier).
      SimCorruptionReport r;
      r.what = "timing-handshake-watchdog";
      r.subsystem = "timing_handshake";
      r.tick = info.cycleTick;
      char buf[256];
      std::snprintf(buf, sizeof buf,
                    "component '%s' (id %u) did not finish phase %llu (cycle %llu): elapsed %llu ns > ceiling %llu ns "
                    "(open stamp %llu, trip stamp %llu); world frozen at the last committed barrier",
                    info.componentName, info.componentId, (unsigned long long)info.phase,
                    (unsigned long long)info.cycleTick, (unsigned long long)info.elapsedNs,
                    (unsigned long long)info.ceilingNs, (unsigned long long)info.openStampNs,
                    (unsigned long long)info.tripStampNs);
      r.detail = buf;
      logError("%s", formatSimCorruptionReport(r).c_str());
    });
    sim_->registerHandshake(*handshake_);
    // Clock domains become cadence items (group 2, affine — engine core
    // timing, hs-design §4.2). The item fires from runPhase calls the sim
    // loop makes once per master step; the cadence test itself uses the
    // loop's current master step / rate, exactly as the legacy divider did.
    hsDomainNames_.reserve(size_t(kMaxClockDomains));
    for (size_t i = 0; i < domains_.size(); ++i) {
      hsDomainNames_.push_back(std::string(domains_[i].name));
      HsComponentDesc d;
      d.name = hsDomainNames_.back().c_str();
      d.cadence = CadenceClass::EveryPhase;
      d.threadAffine = true;
      d.tickCritical = true;
      d.group = 2;
      handshake_->registerComponent(d, [this, i](uint64_t, uint64_t) {
        ClockDomain& dom = domains_[i];
        if (!dom.enabled || hsMasterHz_ <= 0 || dom.hz <= 0) return;
        const uint64_t div = uint64_t(hsMasterHz_ / dom.hz);
        if (div == 0 || hsMasterStep_ % div != 0) return;
        dom.step();
        ++dom.ticks;
        if (hsCountWindow_) ++dom.windowTicks;
      });
    }
  }
}

void SimThread::handleHandshakeFreeze() {
  if (!handshake_ || !handshake_->frozen()) {
    // A-M5: the latch was cleared elsewhere (a routine stop/start clears
    // it in TimingHandshakeAuthority::start()) — the report-once flag
    // must clear with it, or a SECOND, distinct freeze is never reported.
    freezeReported_.store(false);
    return;
  }
  if (freezeReported_.exchange(true)) return;  // once per freeze
  setPaused(true);       // freeze lands on the tick-debugger machinery
  handshake_->notifyFreeze();  // logs the SimCorruptionReport (never simFailFast)
}

SimThread::~SimThread() {
  stop();
  if (handshakeWorkersOutstanding()) {
    // C2: never destroy an authority a live pull worker still
    // dereferences (its WorkFn captures reach this SimThread and the
    // world). Deliberately leak it — process-lifetime, logged, and safe —
    // rather than use-after-free in teardown.
    logError("sim_thread: destroying a SimThread with a handshake worker still outstanding — "
             "deliberately leaking the TimingHandshakeAuthority (poisoned stop)");
    handshake_.release();
  }
}

// ---- control producers (any thread): everything sim-affecting crosses
// the serialized control stream; the wake bump keeps a parked paused loop
// responsive to arrivals, as the old direct edit() did.
void SimThread::setPlayerChunk(int cx, int cz) {
  control_->submit(ControlEvent::chunkEvent(cx, cz));
  notifyWake();
}

void SimThread::setRay(const RayInput& r) {
  ray_.writeSlot() = r;  // presentation slot (HUD look-at): latest wins
  ray_.publish();
  control_->submit(ControlEvent::rayEvent(r));  // stamped copy: edit resolution
  notifyWake();
}

void SimThread::edit(EditCmd c) {
  control_->submit(ControlEvent::editEvent(c));
  notifyWake();
}

void SimThread::runBetweenTicks(std::function<void()> fn) {
  const bool wasPaused = paused();
  setPaused(true);
  // RAII pause guard: if fn() throws, the sim must not stay paused forever.
  struct PauseGuard {
    SimThread* self;
    bool wasPaused;
    ~PauseGuard() { if (!wasPaused) self->setPaused(false); }
  } guard{this, wasPaused};
  // blockMutex_ is held across the sim's whole mutation window (edit drain +
  // every tick of the iteration), so taking it here waits out any in-flight tick.
  { std::lock_guard l(blockMutex_); fn(); }
}

void SimThread::start() {
  if (handshakeWorkersOutstanding()) {
    // C2 poison: a pull worker from a failed stop() is still running
    // against the world; starting again would run two pools against it.
    logError("sim_thread: start() refused — a handshake worker is still outstanding from a failed stop()");
    return;
  }
  if (running_.exchange(true)) return;  // already running: a second std::thread assignment would terminate
  if (handshake_) handshake_->start();  // pull workers first: phases may be published immediately
  // A-M5: a routine stop/start cleared the authority's freeze latch (see
  // TimingHandshakeAuthority::start()); if no freeze is in effect now,
  // the report-once flags clear with it so the NEXT freeze reports too.
  if (handshake_ && !handshake_->frozen()) {
    freezeReported_.store(false);
    stepRefusalLogged_.store(false);
  }
  thread_ = std::thread([this] { run(); });
}

bool SimThread::stop() {
  const bool wasRunning = running_.exchange(false);
  if (wasRunning) notifyWake();  // A-M7: a parked paused loop must wake to see the stop
  if (wasRunning && thread_.joinable()) thread_.join();
  if (handshake_) return handshake_->stop();  // no-detach bounded join (P0a/C2); false = worker outstanding, poisoned
  return true;
}

bool SimThread::waitLoops(uint64_t n, std::chrono::milliseconds timeout) const {
  const uint64_t target = loops() + n;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (loops() < target) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    // A-M7: short sleep backoff instead of a pure yield() busy-spin (this
    // used to spin for up to the full 5 s timeout per call).
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  return true;
}

void SimThread::markDirty(int x, int y, int z) {
  // Own sub-chunk, plus neighbours when the cell sits on a border (face culling / AO).
  const int cx = x >> 4, sy = y >> 4, cz = z >> 4;
  const int lx = x & 15, ly = y & 15, lz = z & 15;
  changedCols_.insert(ChunkKey::make(cx, cz));  // the cell's own column (neighbours only remesh)
  for (int dy = (ly == 0 ? -1 : 0); dy <= (ly == 15 ? 1 : 0); ++dy)
    for (int dz = (lz == 0 ? -1 : 0); dz <= (lz == 15 ? 1 : 0); ++dz)
      for (int dx = (lx == 0 ? -1 : 0); dx <= (lx == 15 ? 1 : 0); ++dx) {
        const int s = sy + dy;
        if (s < 0 || s >= kSubChunks) continue;
        dirtySubs_.insert(packPos(cx + dx, s, cz + dz));
      }
}

void SimThread::flushDirty() {
  for (int64_t k : dirtySubs_) {
    int cx, sy, cz; unpackPos(k, cx, sy, cz);
    if (!chunks_.column(cx, cz)) continue;
    // Recycle MeshInput buffers (~24 KB each; up to ~120 passes/s under signal
    // churn made make_shared-per-snapshot pure churn). The custom deleter returns
    // the buffer to the shared pool; the pool outlives in-flight snapshots.
    std::unique_ptr<MeshInput> buf;
    {
      std::lock_guard l(meshPool_->m);
      if (!meshPool_->free.empty()) {
        buf = std::move(meshPool_->free.back());
        meshPool_->free.pop_back();
      }
    }
    if (!buf) buf = std::make_unique<MeshInput>();
    snapshotForMeshingInto(chunks_, blocks_, cx, sy, cz, epoch_, ++meshSeq_, belowWorld_, *buf);
    MeshInput* raw = buf.release();
    const std::shared_ptr<MeshInputPool> pool = meshPool_;
    submit_(std::shared_ptr<MeshInput>(raw, [pool](MeshInput* p) {
      std::unique_ptr<MeshInput> u(p);
      std::lock_guard l(pool->m);
      if (pool->free.size() < 64) pool->free.push_back(std::move(u));
      // else: pool full, drop it (bounds retained memory)
    }));
  }
  dirtySubs_.clear();
}

void SimThread::castRay(const RayInput& ray) {
  ChunkMap& w = chunks_;
  hit_ = raycast(w, ray.ox, ray.oy, ray.oz, ray.dx, ray.dy, ray.dz, kLookRange, hit_xyz_, prev_xyz_, &blocks_);
  const float dx = hit_xyz_[0] + 0.5f - ray.ox, dy = hit_xyz_[1] + 0.5f - ray.oy, dz = hit_xyz_[2] + 0.5f - ray.oz;
  hitDist_ = hit_ ? std::sqrt(dx * dx + dy * dy + dz * dz) : 0.f;
}

// ---- POST phase (control serialization): apply the stream events whose
// stamp has matured, in (tick, seq) order. Ray events move the stamped
// aim, PlayerChunk events move the tick area, Edit events run the edit
// application against the stamped ray. This replaces the pre-serialization
// applyEdits: the stamp discipline is identical (stamped at drain with
// the current tick, applied at that tick's step head), extended from
// edits-only to every sim-affecting control kind, with sequence numbers.
bool SimThread::applyControlEvents(uint64_t tick) {
  // Fast path: nothing due — no extraction, no raycast (the caller keeps
  // the HUD highlight live from the raw ray slot).
  if (!control_->hasDue(tick)) return false;
  controlDue_.clear();
  control_->extractDue(tick, controlDue_);
  const IWorldSimulation::DirtyFn dirty = [this](int x, int y, int z) { simChanged(x, y, z); };
  bool anyEdit = false;
  bool castValid = false;  // hit_ currently reflects stampedRay_ + the world as it stands
  for (ControlEvent& e : controlDue_) {
    switch (e.kind) {
      case ControlKind::Ray:
        stampedRay_ = e.ray;
        castValid = false;
        break;
      case ControlKind::PlayerChunk:
        areaChunkKey_ = ChunkKey::make(e.chunk.cx, e.chunk.cz);
        break;
      case ControlKind::Edit: {
        anyEdit = true;
        if (!castValid) { castRay(stampedRay_); castValid = true; }  // fresh: the world/aim moved
        if (applyOneEdit(e.edit, dirty)) castValid = false;  // world changed: re-cast before the next edit
        break;
      }
      case ControlKind::Locomotion:
      case ControlKind::ControlFrame:
        // No sim-thread consumer today: locomotion steps on the game
        // thread and pilot frames merge into the game's InputSystem
        // upstream of this boundary. The events are in the stream (and
        // the record) so IDemoRecord sees every control source; they are
        // consumed here — extracted in order, accounted in the stream
        // stats — never dropped silently.
        break;
    }
  }
  return anyEdit;
}

// One edit against the current stamped-ray cast. Returns true when the
// world changed (the caller re-casts before the next edit, exactly as
// the old loop re-cast after each mutating edit).
bool SimThread::applyOneEdit(EditCmd& e, const IWorldSimulation::DirtyFn& dirty) {
  ChunkMap& w = chunks_;
  const BlockRegistry& reg = blocks_;
  if (e.kind == EditCmd::Kind::Use) {
    sim_->use(w, e.x, e.y, e.z, dirty);
    return false;
  }
  if (e.kind == EditCmd::Kind::Command) { sim_->command(w, e.command, dirty); return false; }
  if (!hit_ || hitDist_ > kReach + 0.5f) return false;
  int x, y, z;
  if (e.kind == EditCmd::Kind::Place) {
    x = prev_xyz_[0]; y = prev_xyz_[1]; z = prev_xyz_[2];
    if (w.get(x, y, z) != kAir) return false;
    if (!sim_->canPlace(w, x, y, z, e.block)) return false;  // game placement rules
    int face = 0;
    if (reg.has(e.block, kAttachable)) {  // mounts on the clicked face of the hit block
      face = attachFaceFor(hit_xyz_[0] - x, hit_xyz_[1] - y, hit_xyz_[2] - z);
      if (!face || !sim_->canAttach(w, x, y, z, e.block, face)) return false;
    }
    if (!w.set(x, y, z, e.block)) return false;
    if (reg.has(e.block, kDirectional)) w.setState(x, y, z, makeState(0, e.facing));
    if (face) w.setState(x, y, z, withAttach(w.getState(x, y, z), face));
    sim_->onBlockChanged(w, x, y, z, e.block);
  } else {
    x = hit_xyz_[0]; y = hit_xyz_[1]; z = hit_xyz_[2];
    w.set(x, y, z, kAir);
    sim_->onBlockChanged(w, x, y, z, kAir);
    popAttached(x, y, z);
  }
  markDirty(x, y, z);
  return true;
}

// A cell changed on the sim thread (player edit, game sim, random tick, script command): mark it
// dirty, and when it no longer holds a solid block, pop off attachables mounted on it.
void SimThread::simChanged(int x, int y, int z) {
  markDirty(x, y, z);
  if (!blocks_.has(chunks_.get(x, y, z), kSolid)) popAttached(x, y, z);
}

// The support at (x, y, z) is gone: attachables mounted on it pop off.
void SimThread::popAttached(int x, int y, int z) {
  static constexpr int n6[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  for (const auto& d : n6) {
    const int nx = x + d[0], ny = y + d[1], nz = z + d[2];
    const BlockId b = chunks_.get(nx, ny, nz);
    if (!blocks_.has(b, kAttachable)) {  // game rule (e.g. wire needs a floor or a ceiling)
      if (b == kAir || (d[1] == 0) || sim_->stillSupported(chunks_, nx, ny, nz)) continue;
      chunks_.set(nx, ny, nz, kAir);
      sim_->onBlockChanged(chunks_, nx, ny, nz, kAir);
      sim_->onBlockPopped(chunks_, nx, ny, nz, b);
      simChanged(nx, ny, nz);
      continue;
    }
    int ox, oy, oz;
    attachOffset(stAttach(chunks_.getState(nx, ny, nz)), ox, oy, oz);
    if (nx + ox != x || ny + oy != y || nz + oz != z) continue;  // mounted elsewhere (or legacy)
    chunks_.set(nx, ny, nz, kAir);
    sim_->onBlockChanged(chunks_, nx, ny, nz, kAir);
    sim_->onBlockPopped(chunks_, nx, ny, nz, b);
    simChanged(nx, ny, nz);  // cascades to anything mounted on the popped block
  }
}

void SimThread::publishEvents() {
  sim_->takeEvents(eventBuf_);
  for (const SimEvent& e : eventBuf_) events_.push(e);
  eventBuf_.clear();
}

// Sleep most of the way, then yield-spin: sleep_until alone is too coarse for 512 Hz.
static void waitUntil(Clock::time_point t) {
  for (;;) {
    const auto left = t - Clock::now();
    if (left <= Ns(0)) return;
    if (left > std::chrono::microseconds(1500)) std::this_thread::sleep_for(left - std::chrono::milliseconds(1));
    else std::this_thread::yield();
  }
}

void SimThread::run() {
  const HighResTimerScope timerResolution;  // 1 ms sleeps for the high-rate domains
  // CH-H8: masterStep_/worldTickN_/dropped_/acc_/window state are MEMBERS,
  // initialised once at construction — this function deliberately does NOT
  // reset them. Every stop()/start() (every save, every stream pump)
  // resumes the tick exactly where it stopped: keyed random streams never
  // replay, edit stamps never regress, SimStats.worldTick never goes
  // backwards. Only the wall-clock pacing timestamp is re-based per run.
  auto last = Clock::now();
  if (windowStart_.time_since_epoch().count() == 0) windowStart_ = last;
  // Fail-fast boundary (fail_fast.h, contract section 2; review CH-H6): the
  // WHOLE loop body runs under this guard — not just the per-step pipeline.
  // applyControlEvents (paused branch), publishEvents/takeEvents, the stats calls
  // (activeCount/inspect) and the castRay fallback all reach game hooks on
  // this thread; an exception escaping any of them is converted to a
  // corruption report and the process halts via simFailFastFromException.
  // Log-and-continue is a violation — the world may be half-updated.
  auto tickGuard = [&](const char* subsystem, auto&& stepFn) {
    try {
      stepFn();
    } catch (const std::exception& e) {
      simFailFastFromException(e, subsystem, worldTickN_);
    } catch (...) {
      simFailFastFromException(std::runtime_error("sim_thread: unknown non-std::exception escaped tick"),
                               subsystem, worldTickN_);
    }
  };
  const auto dirtyFn = [this](int x, int y, int z) { simChanged(x, y, z); };
  constexpr auto kRemeshInterval = std::chrono::microseconds(8333);
  auto lastFlush = Clock::now();

  auto reconfigure = [&] {
    cfgWorldTps_ = requestedTps_.load();
    cfgMasterHz_ = cfgWorldTps_;
    for (auto& d : domains_) if (d.enabled) cfgMasterHz_ = std::max(cfgMasterHz_, d.hz);
    cfgWorldDiv_ = cfgMasterHz_ / cfgWorldTps_;  // integer: all rates are powers of two
    stepDur_ = Ns(1'000'000'000LL / cfgMasterHz_);
  };

  // ---- ONE shared master-step body (review CH-H5; hs-design §4.2(5)/D11)
  // The live path and the paused tick-stepper both run THIS body, so the
  // stepped pipeline can never diverge from the live one again: clock
  // domains (or their handshake group-2 phase), tick-quantized edits,
  // sim_->tick, sim_->randomTick AND runRandomTicks, and the window/stat
  // accounting all live here exactly once. `countWindow` selects the
  // per-second domain window counting (both paths pass true; the stepped
  // path previously skipped it — now documented as included, so domain
  // achieved-Hz stats stay honest while stepping).
  // Returns false iff the group-2 handshake phase FROZE this step
  // (review A-M5): the step is then NOT consumed — masterStep_ is not
  // advanced — and the same master step re-runs on resume, when the
  // domain cadence (masterStep % divider) will select exactly the work
  // that was frozen out. (Signal-side group-1 state self-heals via its
  // forced full pass; a mid-tick freeze AFTER the domain phase committed
  // still advances the master step, because the domain work for it did
  // run to completion — only the world tick is left incomplete, exactly
  // as the live path always behaved.)
  auto stepMasterOnce = [&](bool countWindow) -> bool {
    const auto t0 = Clock::now();
    bool hsOk = true;
    if (handshake_) {
      // Clock domains fire as cadence items in a standalone phase for
      // this master step (group 2), before the world tick — the legacy
      // ordering (hs-design §4.2).
      hsMasterStep_ = masterStep_; hsMasterHz_ = cfgMasterHz_; hsCountWindow_ = countWindow;
      hsOk = handshake_->runPhase(masterStep_, masterStep_, 2);
      if (!hsOk) handleHandshakeFreeze();
    } else {
      for (auto& d : domains_)
        if (d.enabled && masterStep_ % uint64_t(cfgMasterHz_ / d.hz) == 0) {
          d.step(); ++d.ticks;
          if (countWindow) ++d.windowTicks;
        }
    }
    if (!hsOk) {
      windowAllNs_ += double((Clock::now() - t0).count());
      return false;  // frozen step: not consumed, re-runs on resume (A-M5)
    }
    if (masterStep_ % uint64_t(cfgWorldDiv_) == 0) {
      const auto t1 = Clock::now();
      const bool tickEdits = applyControlEvents(worldTickN_);  // POST: tick-quantized control application
      stepEditsApplied_ |= tickEdits;
      stepRaycastDone_ |= tickEdits;  // applyControlEvents casts iff it had due edits
      const uint64_t pk = areaChunkKey_;  // stamped PlayerChunk state, folded at the POST phase
      const TickArea area{int(int32_t(pk >> 32)), int(int32_t(pk & 0xffffffffu)), radius_};
      sim_->tick(chunks_, area, dirtyFn);
      if (handshake_ && handshake_->frozen()) {
        // Watchdog froze this tick mid-cycle: the tick does not complete
        // (no random ticks, no world-tick increment) and the sim pauses
        // on the debugger path. The master step still advances: its
        // domain phase committed (see the A-M5 note above).
        handleHandshakeFreeze();
      } else {
        sim_->randomTick(chunks_, area, worldTickN_, dirtyFn);
        runRandomTicks(chunks_, blocks_, randomTickable_, area.pcx, area.pcz, radius_, worldTickN_, randomPerSection_,
                       [&](int x, int y, int z, BlockId b, uint32_t rnd) {
                         ++randomTicks_;
                         sim_->onRandomTick(chunks_, x, y, z, b, rnd, dirtyFn);
                       },
                       &randomTickCache_);
        ++worldTickN_; ++windowWorldTicks_;
      }
      const double tickNs = double((Clock::now() - t1).count());
      windowWorldNs_ += tickNs; windowWorldMaxNs_ = std::max(windowWorldMaxNs_, tickNs);
    }
    windowAllNs_ += double((Clock::now() - t0).count());
    ++masterStep_;
    return true;
  };

  // The whole loop body as one guarded unit (CH-H6). Local per-iteration
  // results are reported back through members (stepEditsApplied_ etc.).
  auto loopBody = [&] {
    if (requestedTps_.load(std::memory_order_relaxed) != cfgWorldTps_ ||
        domainToggles_.load(std::memory_order_relaxed) != togglesSeen_ || !configured_) {
      togglesSeen_ = domainToggles_.load();
      for (size_t i = 0; i < domains_.size(); ++i) domains_[i].enabled = domainInitial_[i] != (((togglesSeen_ >> i) & 1u) != 0);
      configured_ = true;
      reconfigure();
    }

    // ---- sim-thread mutation window: the edit drain plus every tick of this
    // iteration run under blockMutex_. getBlock() (game/MCP thread) takes the same
    // mutex, so cross-thread block reads are race-free (the old lock was theater:
    // no writer ever took it). Uncontended for the sim thread.
    bool editsApplied = false;
    bool raycastDone = false;
    {
      std::lock_guard l(blockMutex_);

      // ---- control PRE phase (control serialization): drain every control
      // source's submissions once per iteration and stamp each event with
      // (seq, the tick it lands on). Application at the POST phase is then
      // a pure function of the stamped stream (the pre-serialization code
      // drained only edits, at this same point, with this same stamp rule;
      // identical edit streams could still diverge across frame rates
      // because the ray and the tick area were read from raw slots).
      control_->prePhase(worldTickN_);

      const RayInput ray = ray_.read();
      stepEditsApplied_ = false;
      stepRaycastDone_ = false;
      const bool paused = paused_.load(std::memory_order_relaxed);

      // ---- fixed-timestep accumulator with catch-up cap
      const auto now = Clock::now();
      acc_ += now - last; last = now;
      const Ns maxLag = std::chrono::milliseconds(100);
      if (acc_ > maxLag) { dropped_ += uint64_t((acc_ - maxLag) / stepDur_); acc_ = maxLag; }

      if (paused) {
        acc_ = Ns(0);
        // Stamps assigned at the PRE phase are the current tick, so paused
        // application by stamp == the old apply-at-once behavior for live
        // input — and a replayed event stamped in the future now correctly
        // waits for its tick instead of landing early.
        editsApplied = applyControlEvents(worldTickN_);
        raycastDone = editsApplied;  // applyControlEvents casts iff it had due edits
        // Tick debugger: run pending single-step WORLD ticks while paused
        // (CH-H5): keep advancing master steps through the shared body
        // until N world ticks have completed, however many master steps
        // the domain divider takes.
        uint64_t steps = stepRequested_.exchange(0);
        if (handshake_ && handshake_->frozen()) {
          // A frozen phase must NOT be re-run by a step request (hs-design
          // §4.4): refuse the steps, log once per freeze.
          if (!stepRefusalLogged_.exchange(true))
            logWarn("timing-handshake: step request refused — the sim is frozen by the watchdog (tick_unfreeze to recover)");
          steps = 0;
        }
        uint64_t steppedWorld = 0;
        while (steppedWorld < steps) {
          const uint64_t before = worldTickN_;
          if (!stepMasterOnce(true)) break;  // froze mid-stepping: remaining steps are dropped
                                             // (the freeze pauses the sim; unfreeze + re-step to continue)
          if (worldTickN_ != before) { ++steppedWorld; ticksStepped_.fetch_add(1, std::memory_order_relaxed); }
        }
        editsApplied |= stepEditsApplied_;
        raycastDone |= stepRaycastDone_;
      } else {
        int steps = 0;
        while (acc_ >= stepDur_ && steps < 16) {
          if (!stepMasterOnce(true)) {
            // Frozen step (A-M5): not consumed — masterStep_ unchanged and
            // the step re-runs on resume. Charge the pacing slot anyway
            // and stop this burst; the freeze has paused the sim.
            acc_ -= stepDur_;
            break;
          }
          ++steps;
          acc_ -= stepDur_;
        }
        editsApplied |= stepEditsApplied_;
        raycastDone |= stepRaycastDone_;
      }
      if (!raycastDone) castRay(ray);  // no tick ran: keep the HUD highlight live
    }  // release blockMutex_ before the sleep below

    bool flushNow = editsApplied;  // player edits remesh at once (no added latency)
    if (remeshAll_.exchange(false)) {
      for (const auto& [key, col] : chunks_.columns())
        for (int sy = 0; sy < kSubChunks; ++sy)
          if (col->subs[sy]) dirtySubs_.insert(packPos(col->cx, sy, col->cz));
      flushNow = true;
    }
    // Signal-driven changes are coalesced to at most ~120 remesh passes per second: at 512 Hz a
    // busy circuit would otherwise snapshot and mesh the same sub-chunks hundreds of times a second.
    const auto tFlush = Clock::now();
    if (!dirtySubs_.empty() && (flushNow || tFlush - lastFlush >= kRemeshInterval)) {
      flushDirty();
      lastFlush = tFlush;
    }
    publishEvents();

    const auto tw = Clock::now();
    const double win = std::chrono::duration<double>(tw - windowStart_).count();
    if (win >= 1.0) {
      statTps_ = float(windowWorldTicks_ / win);
      statTickMs_ = windowWorldTicks_ ? float(windowWorldNs_ / windowWorldTicks_ / 1e6) : 0.f;
      statTickMaxMs_ = float(windowWorldMaxNs_ / 1e6); windowWorldMaxNs_ = 0;
      statBudget_ = float(windowAllNs_ / (win * 1e9) * 100.0);
      for (auto& d : domains_) { d.achievedHz = float(d.windowTicks / win); d.windowTicks = 0; }
      windowStart_ = tw; windowWorldTicks_ = 0; windowWorldNs_ = windowAllNs_ = 0;
    }

    SimStats& s = stats_.writeSlot();
    s.worldTick = worldTickN_; s.targetTps = cfgWorldTps_; s.masterHz = cfgMasterHz_;
    s.tps = statTps_; s.tickMs = statTickMs_; s.tickMaxMs = statTickMaxMs_; s.budgetPct = statBudget_; s.droppedTicks = dropped_;
    s.active = uint32_t(sim_->activeCount());
    s.randomTicks = randomTicks_;
    if (handshake_) {
      // Authority thread reading its own authority's stats: the stats()
      // live-reference contract (A-M6) is satisfied on this thread.
      const HsStats& hs = handshake_->stats();
      s.hsOverrunPhases = hs.overrunPhases;
      uint32_t off = 0;
      for (const HsComponentStats& c : hs.perComponent) off += c.repeatOffender ? 1u : 0u;
      s.hsOffenders = off;
      s.hsFrozen = handshake_->frozen();
    }
    s.domainCount = int(domains_.size());
    for (size_t i = 0; i < domains_.size(); ++i)
      s.domains[i] = {domains_[i].hz, domains_[i].enabled ? domains_[i].achievedHz : 0.f, domains_[i].enabled};
    s.hit = hit_; s.hitDist = hitDist_; s.inReach = hit_ && hitDist_ <= kReach + 0.5f;
    if (hit_) {
      s.hx = hit_xyz_[0]; s.hy = hit_xyz_[1]; s.hz = hit_xyz_[2];
      s.hitBlock = chunks_.get(s.hx, s.hy, s.hz); s.hitState = chunks_.getState(s.hx, s.hy, s.hz);
      // The inspector string is rebuilt only when the looked-at cell changes
      // (it was formatted every loop iteration, even with zero ticks). Note: the
      // line can go briefly stale if the block's state changes under a stationary
      // cursor; moving the cursor refreshes it.
      if (!hitInfoValid_ || hit_xyz_[0] != hitInfoXyz_[0] || hit_xyz_[1] != hitInfoXyz_[1] ||
          hit_xyz_[2] != hitInfoXyz_[2]) {
        hitInfoCache_ = sim_->inspect(chunks_, s.hx, s.hy, s.hz);
        hitInfoXyz_[0] = hit_xyz_[0]; hitInfoXyz_[1] = hit_xyz_[1]; hitInfoXyz_[2] = hit_xyz_[2];
        hitInfoValid_ = true;
      }
      std::snprintf(s.hitInfo, sizeof s.hitInfo, "%s", hitInfoCache_.c_str());
    } else {
      s.hitInfo[0] = 0;
      hitInfoValid_ = false;
    }
    stats_.publish();

    // ---- tail wait (review A-M7) ----
    // Live: pace to the next master step as before. Paused with nothing
    // pending: PARK on wakeCv_ instead of waking at masterHz (512/s) to
    // re-drain/re-cast/re-publish. Every wake source bumps the wake epoch
    // via notifyWake(): unpause, step request, edit arrival, ray update,
    // rate/domain change, remesh request, stop. A step request that
    // arrived during this body was already exchanged above; one landing
    // after the exchange bumps stepRequested_ and the epoch, and the
    // predicate below sees it — the park can never swallow a step or an
    // edit. Promptness: the wake is a CV notify, so edit application and
    // step response are faster than the old one-master-step poll.
    if (paused_.load(std::memory_order_relaxed) && stepRequested_.load(std::memory_order_relaxed) == 0) {
      std::unique_lock lk(wakeM_);
      const uint64_t e = wakeEpoch_.load(std::memory_order_relaxed);
      wakeCv_.wait(lk, [&] {
        return !running_.load(std::memory_order_relaxed) || !paused_.load(std::memory_order_relaxed) ||
               stepRequested_.load(std::memory_order_relaxed) != 0 ||
               wakeEpoch_.load(std::memory_order_relaxed) != e;
      });
    } else {
      waitUntil(last + (stepDur_ - acc_));
    }
  };

  while (running_.load(std::memory_order_relaxed)) {
    loops_.fetch_add(1, std::memory_order_acq_rel);
    tickGuard("sim_thread", [&] { loopBody(); });
  }
}

}  // namespace litholattice
