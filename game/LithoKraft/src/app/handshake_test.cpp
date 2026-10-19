// Timing Handshake policy + determinism harness (hs-design §6).
// Modes (from main.cpp): --handshake-test (all proofs),
// --handshake-scenario TICKS legacy|handshake WORKERS PHASES (hash only).
#include "handshake_test.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "litholattice/core/log.h"
#include "litholattice/core/threading.h"
#include "litholattice/sim/det_sched.h"
#include "litholattice/sim/sim_thread.h"
#include "litholattice/sim/timing_handshake.h"
#include "litholattice/world/dimension.h"
#include "lithokraft/content/blocks.h"
#include "lithokraft/content/circuit_scene.h"
#include "lithokraft/content/component.h"
#include "lithokraft/content/signal.h"
#include "lithokraft/content/signal_world.h"
#include "lithokraft/game/dimensions.h"
#include "lithokraft/game/world_save.h"

namespace lithokraft {
namespace {

using litholattice::DeterministicScheduler;
using Clock = std::chrono::steady_clock;

int gFails = 0;
void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++gFails;
}

// ---- outcome hash (hs-design §6.4) ----
// FNV-1a over every cell (block StableId + cell state) of every column in
// ascending column-key order, then the signal exportState map in order, then
// component count and tick count. Missing sub-chunks hash as air cells, so
// the hash never depends on which sub-chunks happen to be allocated.
// A missing sub-chunk hashes as 4096 air cells (kAirStableId, state 0).
// The state half is 8 zero bytes: (h ^ 0) * prime per byte, i.e. one
// multiplication by prime^8 — exact. The StableId half has no such shortcut
// (xor does not distribute over multiplication), so it loops per cell.
uint64_t hashAirSubChunk(uint64_t h) {
  uint64_t primePow8 = 1;
  for (int i = 0; i < 8; ++i) primePow8 *= DeterministicScheduler::kFnvPrime;
  for (int i = 0; i < kSubChunkVolume; ++i) {
    h = DeterministicScheduler::hashU64(kAirStableId, h);
    h *= primePow8;  // hashU64(0, h), closed form
  }
  return h;
}

uint64_t hashWorld(const ChunkMap& world, const SignalSim& sig, uint64_t ticks) {
  uint64_t h = DeterministicScheduler::kFnvOffset;
  std::vector<uint64_t> keys;
  keys.reserve(world.columns().size());
  for (const auto& [k, col] : world.columns()) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  for (uint64_t key : keys) {
    const ChunkColumn* col = world.columns().at(key).get();
    for (int sy = 0; sy < kSubChunks; ++sy) {
      const SubChunk* sc = col->subs[size_t(sy)].get();
      if (!sc) { h = hashAirSubChunk(h); continue; }
      for (int i = 0; i < kSubChunkVolume; ++i) {
        h = DeterministicScheduler::hashU64(sc->blocks.at(size_t(i)), h);
        h = DeterministicScheduler::hashU64(uint64_t(sc->state[size_t(i)]), h);
      }
    }
  }
  for (const auto& [k, bytes] : sig.exportState()) {
    h = DeterministicScheduler::hashU64(k, h);
    h = DeterministicScheduler::hashBytes(bytes.data(), bytes.size(), h);
  }
  h = DeterministicScheduler::hashU64(uint64_t(sig.componentCount()), h);
  h = DeterministicScheduler::hashU64(ticks, h);
  return h;
}

// ---- determinism scenario (hs-design §6.4, driven in-process) ----
// buildCircuitScene(world, 4, y, 4, 120) on the wafer dimension (perf §3's
// scene), player chunk (4,4), radius 8. Scripted inputs are applied between
// ticks at exact tick indices — identical in both modes by construction:
//   tick 100: break the first red trace (packPos order) + onBlockChanged
//   tick 120: use() the first repeater (cycles its delay)
//   tick 150: replace the trace + onBlockChanged
// (The §6.4 text routes inputs through SimThread::edit; edits there are
// stamped at drain time by wall clock, which would make the *input itself*
// differ between runs. Tick-indexed direct inputs are the deterministic
// equivalent; the SimThread path is exercised by the freeze proof below
// and by --perf-headless.)
struct Scenario {
  DimensionRegistry reg;
  Dimension* wafer = nullptr;
  std::unique_ptr<SignalWorldSimulation> ws;
  int y = 0, wireX = -1, wireZ = -1, repX = -1, repZ = -1;
};

Scenario buildScenario() {
  Scenario sc;
  sc.reg = createDimensions();
  sc.wafer = sc.reg.find("wafer");
  JobPool pool(2);
  sc.wafer->load(pool, 8);
  ChunkMap& w = sc.wafer->chunks;
  sc.y = w.surfaceY(8, 8) + 1;
  buildCircuitScene(w, 4, sc.y, 4, 120);
  sc.ws = std::make_unique<SignalWorldSimulation>();
  sc.ws->scan(w);
  // First red trace / repeater at scene height, in packPos order (x, then z).
  for (int x = 4; x < 124 && (sc.wireX < 0 || sc.repX < 0); ++x)
    for (int z = 4; z < 124 && (sc.wireX < 0 || sc.repX < 0); ++z) {
      const BlockId b = w.get(x, sc.y, z);
      if (b == kRedTrace && sc.wireX < 0) { sc.wireX = x; sc.wireZ = z; }
      if (b == kRepeater && sc.repX < 0) { sc.repX = x; sc.repZ = z; }
    }
  return sc;
}

uint64_t runScenario(int ticks, bool handshake, int workers, int phases) {
  Scenario sc = buildScenario();
  ChunkMap& w = sc.wafer->chunks;
  const litholattice::IWorldSimulation::DirtyFn noDirty = [](int, int, int) {};
  const litholattice::TickArea area{4, 4, 8};
  std::unique_ptr<litholattice::TimingHandshakeAuthority> auth;
  if (handshake) {
    litholattice::TimingHandshakeAuthority::Config hc;
    hc.phasesPerCycle = phases;
    hc.workerThreads = workers;
    auth = std::make_unique<litholattice::TimingHandshakeAuthority>(hc);
    sc.ws->registerHandshake(*auth);
    auth->start();
  }
  auto scripted = [&](int t) {
    if (t == 100 && sc.wireX >= 0) {
      w.set(sc.wireX, sc.y, sc.wireZ, kAir);
      sc.ws->onBlockChanged(w, sc.wireX, sc.y, sc.wireZ, kAir);
    } else if (t == 120 && sc.repX >= 0) {
      sc.ws->use(w, sc.repX, sc.y, sc.repZ, noDirty);
    } else if (t == 150 && sc.wireX >= 0) {
      w.set(sc.wireX, sc.y, sc.wireZ, kRedTrace);
      sc.ws->onBlockChanged(w, sc.wireX, sc.y, sc.wireZ, kRedTrace);
    }
  };
  for (int t = 0; t < ticks; ++t) {
    scripted(t);
    sc.ws->tick(w, area, noDirty);
  }
  if (auth) auth->stop();
  return hashWorld(w, sc.ws->signal(), uint64_t(ticks));
}

// ---- §6.2 WAIT proof ----
struct Latch {
  std::mutex m;
  std::condition_variable cv;
  bool released = false;
  void wait() { std::unique_lock lk(m); cv.wait(lk, [&] { return released; }); }
  void release() { { std::lock_guard lk(m); released = true; } cv.notify_all(); }
};

uint64_t mixState(uint64_t state, uint64_t id, uint64_t phase) {
  return litholattice::detMix64(state ^ (id * 0x9E3779B97F4A7C15ULL) ^ (phase + 0x9E3779B97F4A7C15ULL));
}

// Calibrate a splitmix spin on this machine: iterations ≈ targetNs of work.
// The per-iteration cost is the MINIMUM over several samples: CPU load can
// only ever inflate a sample (preemption stretches it), never deflate it, so
// the minimum is the load-proof estimate of true cost — a burn calibrated
// from it takes AT LEAST targetNs under any load. (A single-sample
// calibration taken during a load spike under-burns when the spike passes:
// the burner's firing then lands under the cycle budget and earns no
// overrun — observed in practice with parallel builds on this 2-core box.)
uint64_t calibrateBurnIters(double targetNs) {
  double minNsPerIter = 1e30;
  uint64_t sink = 0;
  for (int sample = 0; sample < 5; ++sample) {
    const auto c0 = Clock::now();
    for (uint64_t i = 0; i < 200'000; ++i) sink = litholattice::detMix64(sink + i);
    const double nsPerIter = double(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - c0).count()) / 200'000.0;
    minNsPerIter = std::min(minNsPerIter, nsPerIter);
  }
  if (sink == 42) std::printf(" ");
  std::printf("  calibration: %.2f ns/iter (min of 5)\n", minNsPerIter);
  return uint64_t(targetNs / std::max(1.0, minNsPerIter));
}

void spinBurn(uint64_t iters, uint64_t seed) {
  uint64_t s = seed;
  for (uint64_t i = 0; i < iters; ++i) s = litholattice::detMix64(s + i);
  if (s == 42) std::printf(" ");
}

void testWait() {
  std::printf("== handshake WAIT policy (burn component overruns; outcome unchanged)\n");
  // Burn ≈40 ms per firing: 8x the 5 ms phase budget and 2x the 20 ms CYCLE
  // budget (5 ms x 4 phases) that offender accounting is judged against
  // (sweep C2) — the margin is what makes the attribution checks below
  // meaningful under CPU saturation, not merely on an idle box.
  const uint64_t burnIters = calibrateBurnIters(40'000'000.0);
  std::printf("  burn = %llu iters (~40 ms vs 5 ms phase budget / 20 ms cycle budget)\n", (unsigned long long)burnIters);

  auto runArm = [&](bool heavy, litholattice::HsStats& statsOut, std::vector<uint64_t>& statesOut) {
    litholattice::TimingHandshakeAuthority::Config hc;
    hc.phasesPerCycle = 4;
    hc.workerThreads = 2;
    hc.phaseBudget = std::chrono::milliseconds(5);
    hc.watchdogCeiling = std::chrono::seconds(10);
    hc.offenderThreshold = 3;
    litholattice::TimingHandshakeAuthority auth(hc);
    std::vector<uint64_t> states(4, 0);
    auto filler = [&](int id) {
      return [&states, id](uint64_t phase, uint64_t) { states[size_t(id)] = mixState(states[size_t(id)], uint64_t(id), phase); };
    };
    using litholattice::CadenceClass;
    litholattice::HsComponentDesc d;
    d.name = "test.filler0"; auth.registerComponent(d, filler(0));
    litholattice::HsComponentDesc bd;
    bd.name = "test.burn"; bd.phaseDivider = 8; bd.phaseOffset = 0;  // fires on phases % 8 == 0
    auth.registerComponent(bd, [&states, heavy, burnIters](uint64_t phase, uint64_t) {
      states[1] = mixState(states[1], 1, phase);
      if (heavy) spinBurn(burnIters, phase);
    });
    d.name = "test.filler2"; auth.registerComponent(d, filler(2));
    d.name = "test.filler3"; auth.registerComponent(d, filler(3));
    auth.start();
    bool allOk = true;
    for (uint64_t ph = 0; ph < 64; ++ph) allOk = auth.runPhase(ph, ph / 4, 0) && allOk;
    auth.stop();
    statsOut = auth.stats();
    statesOut = states;
    return allOk;
  };

  litholattice::HsStats heavyStats, lightStats;
  std::vector<uint64_t> heavyStates, lightStates;
  const bool okHeavy = runArm(true, heavyStats, heavyStates);
  const bool okLight = runArm(false, lightStats, lightStates);
  uint64_t hHeavy = DeterministicScheduler::kFnvOffset, hLight = DeterministicScheduler::kFnvOffset;
  for (uint64_t v : heavyStates) hHeavy = DeterministicScheduler::hashU64(v, hHeavy);
  for (uint64_t v : lightStates) hLight = DeterministicScheduler::hashU64(v, hLight);
  check(okHeavy && okLight, "WAIT: all 64 phases commit in both arms (nothing skipped)");
  check(hHeavy == hLight, "WAIT: outcome hash identical despite overruns (schedule preserved)");
  check(heavyStats.overrunPhases >= 1, "WAIT: overrun phases counted");
  check(heavyStats.perComponent.size() > 1 && heavyStats.perComponent[1].overruns >= 1, "WAIT: burner overruns attributed");
  check(heavyStats.perComponent[1].repeatOffender, "WAIT: burner flagged repeat offender at threshold 3");
  bool fillerOffender = false;
  for (size_t i = 0; i < heavyStats.perComponent.size(); ++i)
    if (i != 1 && heavyStats.perComponent[i].repeatOffender) fillerOffender = true;
  check(!fillerOffender, "WAIT: no filler component flagged as offender");
  check(heavyStats.qpcViolations == 0, "WAIT: zero QPC-agreement violations");
  // P1-WAIT (CH-H4/C2): attribution is own-work-per-cycle, so fillers must
  // not even accrue overruns in the heavy arm, however loaded the machine.
  bool fillerOverruns = false;
  for (size_t i = 0; i < heavyStats.perComponent.size(); ++i)
    if (i != 1 && heavyStats.perComponent[i].overruns != 0) fillerOverruns = true;
  check(!fillerOverruns, "WAIT: no filler component accrued any overrun (own-work attribution)");
  // P1-WAIT (sweep §5): the flagged offender's recorded name is the
  // registered name — non-empty and correct.
  check(heavyStats.perComponent.size() > 1 && heavyStats.perComponent[1].name == "test.burn",
        "WAIT: flagged offender's recorded name is non-empty and correct ('test.burn')");
  // P1-WAIT (A-M2): phase diagnostics are split per group; this test runs
  // entirely in group 0, so groups 1/2 must show no phases at all.
  check(heavyStats.perGroup[0].phases == 64 && heavyStats.perGroup[0].overrunPhases >= 1 &&
            heavyStats.perGroup[1].phases == 0 && heavyStats.perGroup[2].phases == 0,
        "WAIT: per-group phase stats split (group 0 only, with its overruns)");
}

// ---- P1-WAIT offender name propagation (sweep §5 empty-name defect) ----
// Registration names in production come from SSO strings stored in a
// reallocating vector (Signal's hsNames_): 'signal.mark' / 'signal.prep'
// were flagged with name ''. The authority now owns a copy taken at
// registration, so neither caller-storage death nor an empty registration
// can produce an empty offender record.
void testOffenderNamePropagation() {
  std::printf("== handshake offender name propagation (flagged offenders are always named)\n");
  const uint64_t burnIters = calibrateBurnIters(10'000'000.0);  // ~10 ms >> 4 ms cycle budget (1 ms x 4)
  litholattice::TimingHandshakeAuthority::Config hc;
  hc.phasesPerCycle = 4;
  hc.workerThreads = 1;
  hc.phaseBudget = std::chrono::milliseconds(1);
  hc.watchdogCeiling = std::chrono::seconds(10);
  hc.offenderThreshold = 1;
  litholattice::TimingHandshakeAuthority auth(hc);
  uint32_t ssoId = UINT32_MAX;
  {
    std::string shortName = "test.sso";  // SSO: c_str() points inside this object
    litholattice::HsComponentDesc d;
    d.name = shortName.c_str();
    ssoId = auth.registerComponent(d, [burnIters](uint64_t phase, uint64_t) { spinBurn(burnIters, phase + 7); });
    shortName.assign("ZZZZZZZZZZZZZZZZZZZZ overwritten after registration");
  }  // shortName destroyed here: the registered pointer dangles from now on
  litholattice::HsComponentDesc ed;
  ed.name = "";  // empty registration
  const uint32_t emptyId = auth.registerComponent(
      ed, [burnIters](uint64_t phase, uint64_t) { spinBurn(burnIters, phase + 13); });
  auth.start();
  bool allOk = true;
  for (uint64_t ph = 0; ph < 4; ++ph) allOk = auth.runPhase(ph, ph / 4, 0) && allOk;
  auth.stop();
  const litholattice::HsStats& st = auth.stats();
  check(allOk, "NAME: all phases commit");
  check(ssoId < st.perComponent.size() && st.perComponent[ssoId].repeatOffender,
        "NAME: component registered via a short-lived SSO name is flagged on its own work");
  check(ssoId < st.perComponent.size() && st.perComponent[ssoId].name == "test.sso",
        "NAME: flagged offender's recorded name survives caller storage ('test.sso')");
  check(emptyId < st.perComponent.size() && st.perComponent[emptyId].repeatOffender &&
            !st.perComponent[emptyId].name.empty(),
        "NAME: empty registration still records a non-empty offender name");
  check(emptyId < st.perComponent.size() &&
            st.perComponent[emptyId].name == "unnamed-" + std::to_string(emptyId),
        "NAME: empty registration records the synthesized 'unnamed-<id>' name");
}

// ---- §6.3 FREEZE proof (bare authority) ----
void testFreezeBare() {
  std::printf("== handshake FREEZE policy (hang component; watchdog names it)\n");
  auto runArm = [&](bool withHang, Latch* latch, uint64_t& hashOut, bool& frozeOut, litholattice::HsFreezeInfo& infoOut,
                    double& wallMsOut) {
    litholattice::TimingHandshakeAuthority::Config hc;
    hc.phasesPerCycle = 4;
    hc.workerThreads = 2;
    hc.phaseBudget = std::chrono::milliseconds(0);
    hc.watchdogCeiling = std::chrono::milliseconds(50);
    litholattice::TimingHandshakeAuthority auth(hc);
    std::vector<uint64_t> states(2, 0);
    using litholattice::CadenceClass;
    litholattice::HsComponentDesc d;
    d.name = "test.filler";
    auth.registerComponent(d, [&states](uint64_t ph, uint64_t) { states[0] = mixState(states[0], 0, ph); });
    litholattice::HsComponentDesc hd;
    hd.name = "test.hang"; hd.phaseDivider = 1'000'003; hd.phaseOffset = 3;  // due in phase 3 only
    auth.registerComponent(hd, [&states, withHang, latch](uint64_t ph, uint64_t) {
      states[1] = mixState(states[1], 1, ph);
      if (withHang && latch) latch->wait();
    });
    auth.start();
    bool ok = true;
    for (uint64_t ph = 0; ph < 3; ++ph) ok = auth.runPhase(ph, 0, 0) && ok;
    uint64_t h = DeterministicScheduler::kFnvOffset;
    for (uint64_t v : states) h = DeterministicScheduler::hashU64(v, h);
    hashOut = h;
    const auto t0 = Clock::now();
    frozeOut = !auth.runPhase(3, 0, 0);
    wallMsOut = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    infoOut = auth.lastFreeze();
    if (withHang) {
      const auto t1 = Clock::now();
      const bool second = auth.runPhase(4, 1, 0);
      const double secondMs = std::chrono::duration<double, std::milli>(Clock::now() - t1).count();
      check(!second && secondMs < 10.0, "FREEZE: latched — a second runPhase returns false immediately (never force-advances)");
      latch->release();
    }
    auth.stop();
    return ok;
  };
  Latch latch;
  uint64_t hHang = 0, hClean = 0;
  bool froze = false, frozeClean = true;
  litholattice::HsFreezeInfo info, infoClean;
  double wallMs = 0, wallClean = 0;
  const bool okHang = runArm(true, &latch, hHang, froze, info, wallMs);
  const bool okClean = runArm(false, nullptr, hClean, frozeClean, infoClean, wallClean);
  check(okHang && okClean, "FREEZE: phases 0-2 commit in both arms");
  check(froze, "FREEZE: runPhase(3) returns false when the watchdog trips");
  check(wallMs >= 40.0 && wallMs < 2000.0, "FREEZE: trip happens within ceiling + slack");
  check(std::string(info.componentName) == "test.hang", "FREEZE: HsFreezeInfo names the hang component");
  check(info.elapsedNs >= info.ceilingNs && info.ceilingNs == 50'000'000ULL, "FREEZE: elapsed >= ceiling in the freeze info");
  check(hHang == hClean, "FREEZE: pre-freeze committed hash equals the clean run's hash");
}

// ---- §6.3 FREEZE end-to-end (real SimThread + save/reload while frozen) ----
void testFreezeEndToEnd() {
  std::printf("== handshake FREEZE end-to-end (SimThread -> paused -> save/reload)\n");
  Scenario sc = buildScenario();
  ChunkMap& w = sc.wafer->chunks;
  const uint64_t hPre = hashWorld(w, sc.ws->signal(), 0);

  Latch latch;
  std::atomic<bool> hangStarted{false};
  litholattice::SimConfig cfg;
  cfg.tickRate = 512;
  cfg.radiusChunks = 8;
  cfg.belowWorldFill = sc.wafer->belowWorldFill;
  cfg.timingHandshake = true;
  cfg.handshakePhasesPerCycle = 4;
  cfg.handshakeWorkerThreads = 2;
  cfg.handshakeWatchdogMs = 50;
  auto sim = std::make_unique<litholattice::SimThread>(w, blockRegistry(), std::move(sc.ws), std::move(cfg),
                                                       [](std::shared_ptr<litholattice::MeshInput>) {});
  litholattice::TimingHandshakeAuthority* auth = sim->handshakeAuthority();
  check(auth != nullptr, "FREEZE e2e: authority constructed in handshake mode");
  if (auth) {
    litholattice::HsComponentDesc hd;
    hd.name = "test.hang";
    hd.cadence = litholattice::CadenceClass::EveryPhase;  // group 0: fires in the first phase of any group
    hd.threadAffine = false;
    auth->registerComponent(hd, [&latch, &hangStarted](uint64_t, uint64_t) {
      hangStarted.store(true);
      latch.wait();
    });
  }
  sim->setPlayerChunk(4, 4);
  sim->start();
  bool pausedSeen = false;
  for (int i = 0; i < 500 && !pausedSeen; ++i) {
    pausedSeen = sim->paused();
    if (!pausedSeen) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  check(hangStarted.load(), "FREEZE e2e: hang component started on a pull worker");
  check(pausedSeen, "FREEZE e2e: SimThread paused() is true — freeze landed on the tick-debugger machinery");
  check(sim->handshakeFrozen(), "FREEZE e2e: authority freeze latch is set");
  bool reportSeen = false;
  for (const litholattice::LogRecord& r : litholattice::engineLogTap().snapshot())
    if (r.text.find("timing-handshake-watchdog") != std::string::npos) reportSeen = true;
  check(reportSeen, "FREEZE e2e: a SimCorruptionReport with what=\"timing-handshake-watchdog\" was logged (no simFailFast)");

  // World permanence: the frozen world still saves, and the reload matches.
  // P0a/C2: stop() with the hang worker stuck must FAIL loudly (no detach).
  // The snapshot below reads committed state while that worker is parked
  // on the test latch (not executing); production save-completion gates
  // on this return value (see SimThread::stop()).
  check(!sim->stop(), "FREEZE e2e: stop() with a stuck worker fails loudly instead of detaching (C2)");
  const auto dir = std::filesystem::temp_directory_path() / "lithokraft_hs_freeze_e2e";
  std::filesystem::remove_all(dir);
  ChunkMap restored;
  restored.setRegistry(&blockRegistry());
  for (const auto& [k, col] : w.columns()) {
    auto c = std::make_unique<ChunkColumn>();
    c->cx = col->cx; c->cz = col->cz;
    restored.insert(std::move(c));
  }
  std::vector<uint8_t> blob;
  {
    WorldSave save(dir);
    auto& sig = static_cast<SignalWorldSimulation&>(sim->simulation()).signal();
    save.saveDimension("wafer", w, sig, true);
  }
  {
    WorldSave save(dir);
    save.loadDimension("wafer", restored, blob);
  }
  SignalSim sig2;
  sig2.scan(restored);
  const bool imported = sig2.importState(blob);
  const uint64_t hReload = hashWorld(restored, sig2, 0);
  check(imported && hReload == hPre, "FREEZE e2e: world saved while frozen reloads to the pre-freeze committed hash");
  std::filesystem::remove_all(dir);

  latch.release();  // the stuck worker can now return from its component
  // P0a/C2: the retried stop() joins it deterministically. (This replaces
  // the old 300 ms sleep workaround that only survived because the
  // detached worker happened to finish before the authority died.)
  check(sim->stop(), "FREEZE e2e: retried stop() joins the released worker (no detach, no sleep)");
  check(!sim->handshakeWorkersOutstanding(), "FREEZE e2e: no worker outstanding after the retried stop()");
  sim.reset();
}

// ---- P0a soak: repeated stop()/start() + save-pattern + C1 regression ----
void testStopStartSoak() {
  std::printf("== handshake stop/start soak (200 cycles, C1 regression + save pattern)\n");
  litholattice::TimingHandshakeAuthority::Config hc;
  hc.phasesPerCycle = 4;
  hc.workerThreads = 2;
  hc.watchdogCeiling = std::chrono::seconds(5);
  litholattice::TimingHandshakeAuthority auth(hc);
  std::atomic<uint64_t> fires{0};
  litholattice::HsComponentDesc d;
  d.name = "test.counter";
  auth.registerComponent(d, [&fires](uint64_t, uint64_t) { fires.fetch_add(1); });
  check(auth.start(), "SOAK: initial start() succeeds");
  uint64_t phase = 0;
  bool phasesOk = true, stopsOk = true, startsOk = true, noSpurious = true;
  for (int cycle = 0; cycle < 200; ++cycle) {
    for (int k = 0; k < 2; ++k) phasesOk = auth.runPhase(phase, phase / 4, 0) && phasesOk, ++phase;
    stopsOk = auth.stop() && stopsOk;
    // Save pattern: stop -> snapshot (no-op read of committed state) -> start.
    const uint64_t snapFires = fires.load();
    const uint64_t snapPhases = auth.stats().phases;
    if (snapFires != phase || snapPhases != phase) noSpurious = false;
    startsOk = auth.start() && startsOk;
    // C1 regression (the review's repro, every cycle): with NO new
    // publication after the restart, the component must not fire again.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (fires.load() != snapFires) noSpurious = false;
  }
  phasesOk = auth.runPhase(phase, phase / 4, 0) && phasesOk, ++phase;
  stopsOk = auth.stop() && stopsOk;
  check(phasesOk, "SOAK: every runPhase commits across 200 stop/start cycles");
  check(stopsOk && startsOk, "SOAK: every stop()/start() succeeds (no worker ever outstanding)");
  check(noSpurious && fires.load() == phase,
        "SOAK/C1: fire count equals published phases exactly — no stale re-execution after restart");
}

// ---- P0a stuck-worker stop(): no detach, no UAF, retry joins ----
void testStuckWorkerStop() {
  std::printf("== handshake stuck-worker stop (C2: fail loudly, poison, retry joins)\n");
  Latch latch;
  litholattice::TimingHandshakeAuthority::Config hc;
  hc.phasesPerCycle = 4;
  hc.workerThreads = 1;
  hc.watchdogCeiling = std::chrono::milliseconds(50);
  litholattice::TimingHandshakeAuthority auth(hc);
  std::atomic<uint64_t> fires{0};
  litholattice::HsComponentDesc d;
  d.name = "test.stuck";
  auth.registerComponent(d, [&fires, &latch](uint64_t, uint64_t) {
    fires.fetch_add(1);
    latch.wait();  // parked until the test releases it (not mutating)
  });
  check(auth.start(), "STUCK: start() succeeds");
  check(!auth.runPhase(0, 0, 0), "STUCK: runPhase freezes at the watchdog ceiling on the stuck component");
  check(!auth.stop(), "STUCK: stop() fails loudly with the worker still inside its component (no detach)");
  check(auth.workersOutstanding(), "STUCK: authority is poisoned while the worker is outstanding");
  check(!auth.start(), "STUCK: start() refuses while a worker is outstanding");
  latch.release();
  check(auth.stop(), "STUCK: retried stop() joins the released worker");
  check(!auth.workersOutstanding(), "STUCK: poison clears after the successful retry");
  check(auth.start(), "STUCK: start() succeeds again after the poison clears");
  check(auth.runPhase(1, 0, 0), "STUCK: phases run again after recovery (latch stays released)");
  check(fires.load() == 2, "STUCK: component fired exactly once per published phase");
  check(auth.stop(), "STUCK: final stop() succeeds");
}

// ---- rate scaling: divide gates pulls to aligned cycles (bare authority) ----
void testRateDivideScheduling() {
  std::printf("== handshake rate divide scheduling (cycle-aligned gating, multiply 1)\n");
  {  // Invalid rate refused pre-start, in a separate tiny authority so the
     // component ids in the main arm below stay clean.
    litholattice::TimingHandshakeAuthority::Config hc;
    hc.phasesPerCycle = 4;
    hc.workerThreads = 1;
    hc.watchdogCeiling = std::chrono::seconds(10);
    litholattice::TimingHandshakeAuthority auth(hc);
    litholattice::HsComponentDesc bad;
    bad.name = "rate.invalid";
    bad.rate = litholattice::TickRate{3, 1, 0};  // multiply 3 is not a power of two
    const uint32_t badId = auth.registerComponent(bad, [](uint64_t, uint64_t) {});
    check(badId == UINT32_MAX, "RATE: invalid rate {3,1,0} (non-pow2) refused at registration");
    check(auth.componentCount() == 0, "RATE: refused registration adds no component");
  }
  litholattice::TimingHandshakeAuthority::Config hc;
  hc.phasesPerCycle = 4;
  hc.workerThreads = 2;
  hc.watchdogCeiling = std::chrono::seconds(10);
  litholattice::TimingHandshakeAuthority auth(hc);
  std::set<uint64_t> cycDiv0, cycDiv2, cycNeutral;
  uint64_t pullsDiv0 = 0, pullsDiv2 = 0, pullsNeutral = 0;
  auto recorder = [](std::set<uint64_t>& cycles, uint64_t& pulls) {
    return [&cycles, &pulls](uint64_t, uint64_t cycleTick) { cycles.insert(cycleTick); ++pulls; };
  };
  litholattice::HsComponentDesc d0;
  d0.name = "rate.div4p0"; d0.rate = litholattice::TickRate{1, 4, 0};
  const uint32_t idDiv0 = auth.registerComponent(d0, recorder(cycDiv0, pullsDiv0));
  litholattice::HsComponentDesc d2;
  d2.name = "rate.div4p2"; d2.rate = litholattice::TickRate{1, 4, 2};
  const uint32_t idDiv2 = auth.registerComponent(d2, recorder(cycDiv2, pullsDiv2));
  litholattice::HsComponentDesc dn;
  dn.name = "rate.neutral"; dn.rate = litholattice::TickRate{};
  const uint32_t idNeutral = auth.registerComponent(dn, recorder(cycNeutral, pullsNeutral));
  check(auth.start(), "RATE: divide arm authority starts");
  bool allOk = true;
  for (uint64_t cycle = 0; cycle < 16; ++cycle)
    for (uint64_t sub = 0; sub < 4; ++sub)
      allOk = auth.runPhase(cycle * 4 + sub, cycle, 0) && allOk;
  auth.stop();
  check(allOk, "RATE: all 64 phases commit in the divide arm");
  check(cycDiv0 == std::set<uint64_t>{0, 4, 8, 12}, "RATE: div4 phase 0 fires exactly in cycles {0,4,8,12}");
  check(pullsDiv0 == 16, "RATE: div4 phase 0 pulls == 16 (4 phases x 4 due cycles)");
  check(cycDiv2 == std::set<uint64_t>{2, 6, 10, 14}, "RATE: div4 phase 2 fires exactly in cycles {2,6,10,14}");
  check(pullsDiv2 == 16, "RATE: div4 phase 2 pulls == 16 (4 phases x 4 due cycles)");
  check(cycNeutral.size() == 16 && pullsNeutral == 64, "RATE: neutral fires in all 16 cycles, 64 pulls");
  const litholattice::HsStats& st = auth.stats();
  check(idDiv0 < st.perComponent.size() && st.perComponent[idDiv0].fires == pullsDiv0,
        "RATE: stats fires == pull count for div4p0 (multiply 1)");
  check(idDiv2 < st.perComponent.size() && st.perComponent[idDiv2].fires == pullsDiv2,
        "RATE: stats fires == pull count for div4p2 (multiply 1)");
  check(idNeutral < st.perComponent.size() && st.perComponent[idNeutral].fires == pullsNeutral,
        "RATE: stats fires == pull count for neutral (multiply 1)");
}

// ---- rate scaling: multiply sub-pulls are internal to the phase slot ----
void testRateMultiplyOrdering() {
  std::printf("== handshake rate multiply ordering (sub-pulls inside the slot, final state past the barrier)\n");
  litholattice::TimingHandshakeAuthority::Config hc;
  hc.phasesPerCycle = 4;
  hc.workerThreads = 2;
  hc.watchdogCeiling = std::chrono::seconds(10);
  litholattice::TimingHandshakeAuthority auth(hc);
  uint64_t pulls = 0, internal = 0, published = 0;
  std::vector<uint64_t> subLog, readLog;
  litholattice::HsComponentDesc wd;
  wd.name = "rate.mul2";
  wd.cadence = litholattice::CadenceClass::CycleHead;
  wd.phaseDivider = 4;
  wd.phaseOffset = 0;
  wd.threadAffine = true;
  wd.rate = litholattice::TickRate{2, 1, 0};
  auth.registerComponent(wd, [&pulls, &internal, &published, &subLog](uint64_t, uint64_t) {
    const uint64_t sub = pulls % 2;  // a WorkFn tells sub-pulls apart by counting its own invocations
    subLog.push_back(sub);
    ++pulls;
    ++internal;
    if (sub == 1) published = internal;  // only the final sub-pull's state leaves the slot
  });
  litholattice::HsComponentDesc rd;
  rd.name = "rate.reader";
  rd.cadence = litholattice::CadenceClass::CycleHead;
  rd.phaseDivider = 4;
  rd.phaseOffset = 1;
  rd.threadAffine = true;
  auth.registerComponent(rd, [&published, &readLog](uint64_t, uint64_t) { readLog.push_back(published); });
  check(auth.start(), "RATE: multiply arm authority starts");
  bool allOk = true;
  for (uint64_t ph = 0; ph < 24; ++ph) allOk = auth.runPhase(ph, ph / 4, 0) && allOk;
  auth.stop();
  std::vector<uint64_t> wantSub;
  for (int i = 0; i < 12; ++i) wantSub.push_back(uint64_t(i % 2));
  check(allOk, "RATE: all 24 phases commit in the multiply arm");
  check(subLog == wantSub, "RATE: mul2 writer sub-pull log is [0,1] repeated 6 times (ascending sub-pulls)");
  check(pulls == 12, "RATE: mul2 writer invoked 12 times over 6 cycles (2 sub-pulls x 6 firings)");
  check(readLog == std::vector<uint64_t>{2, 4, 6, 8, 10, 12},
        "RATE: reader past the phase barrier sees only final sub-pull states {2,4,6,8,10,12}");
}

// ---- rate scaling: rates compose through committed state only ----
void testRateCrossing() {
  std::printf("== handshake rate crossing (publish discipline: fast->slow and slow->fast)\n");
  struct Cell { uint64_t readBuf = 0, writeBuf = 0; };
  {  // Arm A (fast -> slow): the writer stages 4 per cycle; the /2 reader
     // samples the buffer committed at the previous cycle's barrier.
    Cell cell;
    litholattice::TimingHandshakeAuthority::Config hc;
    hc.phasesPerCycle = 1;  // each runPhase is one cycle
    hc.workerThreads = 2;
    hc.watchdogCeiling = std::chrono::seconds(10);
    hc.commitFn = [&cell](uint64_t, uint64_t, uint32_t) { cell.readBuf = cell.writeBuf; };
    litholattice::TimingHandshakeAuthority auth(hc);
    litholattice::HsComponentDesc wd;
    wd.name = "rate.cross.writer"; wd.threadAffine = true; wd.rate = litholattice::TickRate{4, 1, 0};
    auth.registerComponent(wd, [&cell](uint64_t, uint64_t) { cell.writeBuf += 1; });  // 4 sub-pulls per cycle
    std::vector<uint64_t> samples;
    litholattice::HsComponentDesc rd;
    rd.name = "rate.cross.reader"; rd.threadAffine = true; rd.rate = litholattice::TickRate{1, 2, 0};
    auth.registerComponent(rd, [&cell, &samples](uint64_t, uint64_t) { samples.push_back(cell.readBuf); });
    check(auth.start(), "RATE: crossing arm A authority starts");
    bool allOk = true;
    for (uint64_t cycle = 0; cycle <= 8; ++cycle) allOk = auth.runPhase(cycle, cycle, 0) && allOk;
    auth.stop();
    check(allOk, "RATE: crossing arm A phases commit (cycles 0..8)");
    check(samples == std::vector<uint64_t>{0, 8, 16, 24, 32},
          "RATE: fast x4 writer -> slow /2 reader samples {0,8,16,24,32} (readBuf during cycle c is 4c, committed; reader due on even cycles)");
  }
  {  // Arm B (slow -> fast): the /2 writer stages a value; the neutral reader
     // sees it only from the NEXT cycle's commit boundary.
    Cell cell;
    litholattice::TimingHandshakeAuthority::Config hc;
    hc.phasesPerCycle = 1;
    hc.workerThreads = 2;
    hc.watchdogCeiling = std::chrono::seconds(10);
    hc.commitFn = [&cell](uint64_t, uint64_t, uint32_t) { cell.readBuf = cell.writeBuf; };
    litholattice::TimingHandshakeAuthority auth(hc);
    const uint64_t stages[4] = {100, 200, 300, 400};
    uint64_t stageIdx = 0;
    litholattice::HsComponentDesc wd;
    wd.name = "rate.cross.writer"; wd.threadAffine = true; wd.rate = litholattice::TickRate{1, 2, 0};
    auth.registerComponent(wd, [&cell, &stages, &stageIdx](uint64_t, uint64_t) {
      cell.writeBuf = stages[stageIdx];
      ++stageIdx;
    });
    std::vector<uint64_t> samples;
    litholattice::HsComponentDesc rd;
    rd.name = "rate.cross.reader"; rd.threadAffine = true;  // neutral rate
    auth.registerComponent(rd, [&cell, &samples](uint64_t, uint64_t) { samples.push_back(cell.readBuf); });
    check(auth.start(), "RATE: crossing arm B authority starts");
    bool allOk = true;
    for (uint64_t cycle = 0; cycle < 8; ++cycle) allOk = auth.runPhase(cycle, cycle, 0) && allOk;
    auth.stop();
    check(allOk, "RATE: crossing arm B phases commit (cycles 0..7)");
    check(stageIdx == 4, "RATE: slow /2 writer fired exactly 4 times in 8 cycles (even cycles)");
    check(samples == std::vector<uint64_t>{0, 100, 100, 200, 200, 300, 300, 400},
          "RATE: slow /2 writer -> fast reader samples {0,100,100,200,200,300,300,400} (staged value readable only at the next commit boundary, never half-advanced)");
  }
}

// ---- rate scaling: A-H1 — same-phase, cross-shard crossing, chosen contract ----
// The corrected contract (tick_rate.h): the authority stages nothing;
// final-only visibility is the WorkFn's obligation, and the final
// sub-pull is knowable via the authority's sub-pull query. This test
// pins exactly that, in the configuration the review found untested:
// TWO rated components in the SAME phase on DIFFERENT shards (writer
// registered first -> shard 0, reader second -> shard 1, round-robin
// over 2 workers) communicating through shared state. The writer stages
// privately and publishes only on isFinalSubPull(); the reader samples
// concurrently. Scheduling decides WHICH published value the reader
// sees on a given phase (previous final or this firing's final) — the
// pinned invariant is that it NEVER sees an intermediate (a value not
// divisible by the writer's multiply), and the writer's observed
// sub-pull context is the exact ascending (index, count, isFinal) run.
void testRateSamePhaseCrossing() {
  std::printf("== handshake rate A-H1 (same-phase cross-shard crossing; publish-on-final via the sub-pull query)\n");
  litholattice::TimingHandshakeAuthority::Config hc;
  hc.phasesPerCycle = 1;  // each runPhase is one phase and one cycle
  hc.workerThreads = 2;
  hc.watchdogCeiling = std::chrono::seconds(10);
  litholattice::TimingHandshakeAuthority auth(hc);
  check(auth.currentSubPullIndex() == 0 && auth.currentSubPullCount() == 1 && auth.isFinalSubPull(),
        "RATE/A-H1: outside any item the sub-pull query reports the single-pull defaults (0 of 1, final)");
  std::atomic<uint64_t> published{0};
  uint64_t internal = 0;                       // writer item only (its shard's worker, serial per phase)
  std::vector<std::array<uint32_t, 3>> ctxLog;   // writer item only: (index, count, isFinal)
  std::vector<uint64_t> samples;                 // reader item only
  litholattice::HsComponentDesc wd;
  wd.name = "rate.cross.samephase.writer";
  wd.rate = litholattice::TickRate{4, 1, 0};
  auth.registerComponent(wd, [&auth, &published, &internal, &ctxLog](uint64_t, uint64_t) {
    ctxLog.push_back({auth.currentSubPullIndex(), auth.currentSubPullCount(),
                      auth.isFinalSubPull() ? 1u : 0u});
    ++internal;                                        // private staging
    if (auth.isFinalSubPull()) published.store(internal, std::memory_order_release);  // publish on final only
  });
  litholattice::HsComponentDesc rd;
  rd.name = "rate.cross.samephase.reader";  // neutral rate, same phase, other shard
  auth.registerComponent(rd, [&published, &samples](uint64_t, uint64_t) {
    samples.push_back(published.load(std::memory_order_acquire));
  });
  check(auth.start(), "RATE/A-H1: same-phase crossing authority starts");
  bool allOk = true;
  for (uint64_t i = 0; i < 32; ++i) allOk = auth.runPhase(i, i, 0) && allOk;
  auth.stop();
  check(allOk, "RATE/A-H1: all 32 same-phase runPhase calls commit");
  bool ctxExact = ctxLog.size() == 128;
  for (size_t i = 0; i < ctxLog.size(); ++i) {
    const uint32_t wantIdx = uint32_t(i % 4);
    ctxExact = ctxExact && ctxLog[i][0] == wantIdx && ctxLog[i][1] == 4 && ctxLog[i][2] == (wantIdx == 3 ? 1u : 0u);
  }
  check(ctxExact, "RATE/A-H1: writer's sub-pull query reports (0,4,F),(1,4,F),(2,4,F),(3,4,T) ascending, 32 firings");
  bool finalOnly = samples.size() == 32, monotone = true;
  for (size_t i = 0; i < samples.size(); ++i) {
    finalOnly = finalOnly && (samples[i] % 4 == 0);
    if (i) monotone = monotone && samples[i] >= samples[i - 1];
  }
  check(finalOnly, "RATE/A-H1: same-phase cross-shard reader NEVER observes an intermediate sub-pull value (every sample is a published final)");
  check(monotone, "RATE/A-H1: reader's samples are non-decreasing (published state only moves forward at final sub-pulls)");
  check(published.load() == 128 && internal == 128, "RATE/A-H1: writer published exactly its final state 128 after 32 x4 firings");
}

// ---- rate scaling: registration order / name-map layout change nothing ----
void testRateShuffleInvariance() {
  std::printf("== handshake rate shuffle invariance (registration order and unordered_map layout are not inputs)\n");
  struct RateCase { const char* name; litholattice::TickRate rate; uint64_t mixConst; };
  const RateCase cases[6] = {
      {"rate.shuffle.neutral", litholattice::TickRate{}, 0xA11CEULL},
      {"rate.shuffle.div2p0", litholattice::TickRate{1, 2, 0}, 0xB0B5ULL},
      {"rate.shuffle.div2p1", litholattice::TickRate{1, 2, 1}, 0xC0DEULL},
      {"rate.shuffle.mul2", litholattice::TickRate{2, 1, 0}, 0xD00DULL},
      {"rate.shuffle.mul4", litholattice::TickRate{4, 1, 0}, 0xE571ULL},
      {"rate.shuffle.div4p3", litholattice::TickRate{1, 4, 3}, 0xF00DULL},
  };
  const uint64_t wantPulls[6] = {32, 16, 16, 64, 128, 8};  // 8 cycles x 4 phases per arm
  struct ShuffleOut {
    std::unordered_map<std::string, uint64_t> states, pulls;
    std::unordered_map<std::string, uint32_t> ids, queried;
    uint32_t absent = 0;
    size_t count = 0;
    bool allOk = false;
  };
  auto runArm = [&cases](bool reverse) {
    ShuffleOut out;
    litholattice::TimingHandshakeAuthority::Config hc;
    hc.phasesPerCycle = 4;
    hc.workerThreads = 2;
    hc.watchdogCeiling = std::chrono::seconds(10);
    std::mutex mapM;  // pull workers share the bookkeeping maps; each name's stream is its own component's
    litholattice::TimingHandshakeAuthority auth(hc);
    for (int i = 0; i < 6; ++i) {
      const int idx = reverse ? 5 - i : i;
      litholattice::HsComponentDesc d;
      d.name = cases[size_t(idx)].name;
      d.rate = cases[size_t(idx)].rate;
      out.ids[cases[size_t(idx)].name] = auth.registerComponent(
          d, [&out, &mapM, &cases, idx](uint64_t phase, uint64_t) {
            std::lock_guard lk(mapM);
            uint64_t& st = out.states[cases[size_t(idx)].name];
            st = mixState(st, cases[size_t(idx)].mixConst, phase);
            ++out.pulls[cases[size_t(idx)].name];
          });
    }
    out.count = auth.componentCount();
    for (const RateCase& rc : cases) out.queried[rc.name] = auth.componentIdByName(rc.name);
    out.absent = auth.componentIdByName("rate.absent");
    out.allOk = auth.start();
    for (uint64_t ph = 0; ph < 32 && out.allOk; ++ph) out.allOk = auth.runPhase(ph, ph / 4, 0) && out.allOk;
    auth.stop();
    return out;
  };
  const ShuffleOut a = runArm(false);  // table order
  const ShuffleOut b = runArm(true);   // reverse order: ids differ, names identical
  check(a.allOk && b.allOk, "RATE: shuffle arms start and all 32 phases commit in both");
  check(a.count == 6 && b.count == 6, "RATE: componentCount() == 6 in both shuffle arms");
  bool statesSame = true, pullsSame = true, pullsExact = true;
  auto getOr = [](const std::unordered_map<std::string, uint64_t>& m, const std::string& k) {
    const auto it = m.find(k);  // a name with no entry (never fired) reads as the sentinel, not a throw
    return it == m.end() ? UINT64_MAX : it->second;
  };
  for (int i = 0; i < 6; ++i) {
    const std::string name = cases[size_t(i)].name;
    const uint64_t stA = getOr(a.states, name), pA = getOr(a.pulls, name), pB = getOr(b.pulls, name);
    std::printf("  shuffle %-22s state %016llx pulls A %llu B %llu (want %llu)\n", cases[size_t(i)].name,
                (unsigned long long)stA, (unsigned long long)pA, (unsigned long long)pB,
                (unsigned long long)wantPulls[size_t(i)]);
    statesSame = statesSame && stA == getOr(b.states, name);
    pullsSame = pullsSame && pA == pB;
    pullsExact = pullsExact && pA == wantPulls[size_t(i)] && pB == wantPulls[size_t(i)];
  }
  check(statesSame, "RATE: shuffle: per-name state identical between table-order and reverse-order registration");
  check(pullsSame && pullsExact,
        "RATE: shuffle: per-name pulls identical and exact (neutral 32, div2 16/16, mul2 64, mul4 128, div4p3 8)");
  check(a.ids.at("rate.shuffle.neutral") != b.ids.at("rate.shuffle.neutral"),
        "RATE: shuffle: the two arms really assigned different ids to the same name (registration order differed)");
  check(a.queried.at("rate.shuffle.neutral") == a.ids.at("rate.shuffle.neutral") &&
            b.queried.at("rate.shuffle.neutral") == b.ids.at("rate.shuffle.neutral"),
        "RATE: componentIdByName returns each authority's own id for 'rate.shuffle.neutral'");
  check(a.queried.at("rate.shuffle.div4p3") == a.ids.at("rate.shuffle.div4p3") &&
            b.queried.at("rate.shuffle.div4p3") == b.ids.at("rate.shuffle.div4p3"),
        "RATE: componentIdByName returns each authority's own id for 'rate.shuffle.div4p3'");
  check(a.absent == UINT32_MAX && b.absent == UINT32_MAX, "RATE: componentIdByName('rate.absent') == UINT32_MAX in both arms");
}

// ---- rate scaling: game layer (SignalSim per-class rates, mini circuit) ----
void testRateGameLayer() {
  std::printf("== handshake rate game layer (SignalSim class tick rates on a mini circuit)\n");
  struct Mini { ChunkMap w; SignalSim sim; };  // built in place per arm (SignalSim is not movable)
  const SignalSim::DirtyFn noDirty = [](int, int, int) {};
  auto buildMini = [](Mini& m) {
    m.w.setRegistry(&blockRegistry());
    for (int cz = 0; cz < 2; ++cz)
      for (int cx = 0; cx < 2; ++cx) {
        auto col = std::make_unique<ChunkColumn>(); col->cx = cx; col->cz = cz;
        m.w.insert(std::move(col));
      }
    for (int z = 0; z < 8; ++z)
      for (int x = 0; x < 8; ++x) m.w.set(x, 9, z, kWaferSilicon);  // floor at y=9
    auto put = [&m](int x, int y, int z, BlockId b) { m.w.set(x, y, z, b); m.sim.onBlockChanged(x, y, z, b); };
    // Facing idiom (signal_selftest's putF / circuit_scene's put): the state
    // is set between w.set and onBlockChanged; facing 1 = east (+x), the
    // repeater's front, so its back reads the trace at x=1.
    auto putF = [&m](int x, int y, int z, BlockId b, int f) {
      m.w.set(x, y, z, b); m.w.setState(x, y, z, makeState(0, f)); m.sim.onBlockChanged(x, y, z, b);
    };
    put(0, 10, 0, kRedTorch);
    put(1, 10, 0, kRedTrace);
    putF(2, 10, 0, kRepeater, 1);
    put(3, 10, 0, kRedTrace);
    put(4, 10, 0, kLamp);
  };
  auto tick8 = [&noDirty](Mini& m) {
    uint64_t ticked = 0;
    for (int i = 0; i < 8; ++i) {
      m.sim.tick(m.w, 0, 0, 100, noDirty);
      ticked += m.sim.lastTickedComponents();
    }
    return ticked;
  };
  uint64_t hash8N = 0;
  {  // Arm N (neutral): the repeater is the only placed ComponentBehavior.
    Mini n; buildMini(n);
    const uint64_t ticked = tick8(n);
    hash8N = hashWorld(n.w, n.sim, 8);
    check(ticked == 8, "RATE: game neutral: summed lastTickedComponents over 8 ticks == 8 (repeater only)");
    for (int i = 0; i < 8; ++i) n.sim.tick(n.w, 0, 0, 100, noDirty);  // continue to 16 ticks
    check(n.w.getState(4, 10, 0) == 1, "RATE: game neutral: lamp state == 1 at tick 16");
  }
  uint64_t hash8D = 0;
  {  // Arm D (divide /2): period 2 over 8 consecutive ticks, whatever the per-cell phase.
    Mini d; buildMini(d);
    d.sim.setClassTickRate(kRepeater, litholattice::TickRate{1, 2, 0});
    const uint64_t ticked = tick8(d);
    hash8D = hashWorld(d.w, d.sim, 8);
    check(ticked == 4, "RATE: game divide /2: summed lastTickedComponents over 8 ticks == 4");
    check(hash8D != hash8N, "RATE: game divide /2: hash at tick 8 differs from neutral (rate engaged)");
    Mini d2; buildMini(d2);
    d2.sim.setClassTickRate(kRepeater, litholattice::TickRate{1, 2, 0});
    (void)tick8(d2);
    check(hashWorld(d2.w, d2.sim, 8) == hash8D, "RATE: game divide /2: second identical build hashes == first (determinism)");
  }
  uint64_t hash8X = 0;
  {  // Arm X (multiply x2): lastTicked counts firings, not sub-pulls.
    Mini x; buildMini(x);
    x.sim.setClassTickRate(kRepeater, litholattice::TickRate{2, 1, 0});
    const uint64_t ticked = tick8(x);
    hash8X = hashWorld(x.w, x.sim, 8);
    check(ticked == 8, "RATE: game multiply x2: summed lastTickedComponents over 8 ticks == 8 (firings, not sub-pulls)");
    check(hash8X != hash8N, "RATE: game multiply x2: hash at tick 8 differs from neutral (sub-pulls advanced the repeater)");
    Mini x2; buildMini(x2);
    x2.sim.setClassTickRate(kRepeater, litholattice::TickRate{2, 1, 0});
    (void)tick8(x2);
    check(hashWorld(x2.w, x2.sim, 8) == hash8X, "RATE: game multiply x2: second identical build hashes == first (determinism)");
  }
  {  // Arm O (override neutrality): set /2, then clear with a neutral rate.
    Mini o; buildMini(o);
    o.sim.setClassTickRate(kRepeater, litholattice::TickRate{1, 2, 0});
    o.sim.setClassTickRate(kRepeater, litholattice::TickRate{1, 1, 0});  // neutral clears the override
    (void)tick8(o);
    check(hashWorld(o.w, o.sim, 8) == hash8N, "RATE: game override cleared with {1,1,0}: hash == neutral exactly (the override seam itself is neutral)");
  }
  std::printf("  rate game hashes at tick 8: neutral %016llx div2 %016llx mul2 %016llx\n",
              (unsigned long long)hash8N, (unsigned long long)hash8D, (unsigned long long)hash8X);
}

// ---- rate scaling: A-H2 — a class-declared tickRate() is validated ----
// tickComponents resolves every placed component's rate through
// SignalSim::classTickRate(block, behavior); the catalog binds behaviours
// to blocks statically, so the resolution seam is where a test behaviour
// can be exercised directly (that seam is exactly what tickComponents
// consumes — the sub-pull loops below it only ever see its result).
void testRateInvalidClassRates() {
  std::printf("== handshake rate A-H2 (invalid class-declared rates refused, logged once, neutral substituted)\n");
  using litholattice::TickRate;
  class DeclaredRateBehavior final : public ComponentBehavior {
   public:
    explicit DeclaredRateBehavior(TickRate r) : rate_(r) {}
    ComponentStep tick(const ComponentInputs&, CellState st, ComponentMemory&) const override { return {0, 0, st}; }
    TickRate tickRate() const override { return rate_; }
   private:
    TickRate rate_;
  };
  auto isNeutral = [](TickRate r) { return r.multiply == 1 && r.divide == 1 && r.phase == 0; };
  auto invalidLogCount = [] {
    int n = 0;
    for (const litholattice::LogRecord& rec : litholattice::engineLogTap().snapshot())
      if (rec.level == litholattice::LogLevel::Error &&
          rec.text.find("invalid class tick rate") != std::string::npos)
        ++n;
    return n;
  };
  const int logs0 = invalidLogCount();
  {  // {0,1,0}: multiply 0 would run zero sub-pulls and publish the
     // default ComponentStep{} (state 0) — the state wipe of A-H2.
    DeclaredRateBehavior beh{TickRate{0, 1, 0}};
    SignalSim sim;
    const TickRate a = sim.classTickRate(kRepeater, &beh);
    const TickRate b = sim.classTickRate(kRepeater, &beh);  // second resolution: cached, no second log
    check(isNeutral(a) && isNeutral(b), "RATE/A-H2: class rate {0,1,0} is substituted with the neutral rate");
    check(invalidLogCount() == logs0 + 1, "RATE/A-H2: {0,1,0} logs an error exactly once per class (not per resolution/cell)");
  }
  {  // Oversized multiply: substituted, so a firing executes ONE pull.
    DeclaredRateBehavior beh{TickRate{1u << 20, 1, 0}};
    SignalSim sim;
    const TickRate r = sim.classTickRate(kRepeater, &beh);
    uint64_t exec = 0;
    for (uint32_t s = 0; s < r.multiply; ++s) ++exec;  // the tickComponents loop shape
    check(isNeutral(r) && exec == 1, "RATE/A-H2: class rate multiply 1<<20 is refused/substituted, not executed (1 pull, not 1048576)");
    check(invalidLogCount() == logs0 + 2, "RATE/A-H2: oversized multiply logs an error once");
  }
  {  // Non-pow2 divide 3.
    DeclaredRateBehavior beh{TickRate{1, 3, 0}};
    SignalSim sim;
    check(isNeutral(sim.classTickRate(kRepeater, &beh)), "RATE/A-H2: class rate divide 3 (non-pow2) is refused/substituted");
    check(invalidLogCount() == logs0 + 3, "RATE/A-H2: divide 3 logs an error once");
  }
  {  // Oversized divide 2^31 (pow2, past the /1024 cap): would starve.
    DeclaredRateBehavior beh{TickRate{1, 1u << 31, 0}};
    SignalSim sim;
    check(isNeutral(sim.classTickRate(kRepeater, &beh)), "RATE/A-H2: class rate divide 2^31 (past cap) is refused/substituted");
    check(invalidLogCount() == logs0 + 4, "RATE/A-H2: oversized divide logs an error once");
  }
  {  // A VALID declared rate passes through untouched and unlogged.
    DeclaredRateBehavior beh{TickRate{2, 4, 1}};
    SignalSim sim;
    const TickRate r = sim.classTickRate(kRepeater, &beh);
    check(r.multiply == 2 && r.divide == 4 && r.phase == 1, "RATE/A-H2: valid class rate {2,4,1} passes through verbatim");
    check(invalidLogCount() == logs0 + 4, "RATE/A-H2: a valid class rate logs no error");
  }
  {  // The wipe itself, composed the way tickComponents runs it: a
     // Memor-delegating behaviour declaring {0,1,0}. With the validated
     // (neutral) rate the loop runs once and the store lands; with the
     // raw rate it would run zero times and publish state 0.
    const ComponentBehavior* memor = BehaviorCatalog::get().component(kMemor);
    check(memor != nullptr, "RATE/A-H2: Memor behaviour is catalog-bound for the no-wipe composition");
    if (memor) {
      class MemorWithBadRate final : public ComponentBehavior {
       public:
        explicit MemorWithBadRate(const ComponentBehavior* inner) : inner_(inner) {}
        ComponentStep tick(const ComponentInputs& in, CellState st, ComponentMemory& m) const override {
          return inner_->tick(in, st, m);
        }
        TickRate tickRate() const override { return TickRate{0, 1, 0}; }
       private:
        const ComponentBehavior* inner_;
      };
      MemorWithBadRate beh{memor};
      SignalSim sim;
      const TickRate r = sim.classTickRate(kMemor, &beh);
      ComponentInputs in;
      in.side[3] = 17;  // west of an east-facing Memor is its back (write) input
      in.ch[3] = 1;
      ComponentMemory mem;
      ComponentStep step{};  // the default whose state 0 is the wipe artifact
      CellState cur = makeState(0, 1);  // east-facing, empty
      for (uint32_t s = 0; s < r.multiply; ++s) { step = beh.tick(in, cur, mem); cur = step.state; }
      check(stValue(step.state) == 17, "RATE/A-H2: Memor declaring {0,1,0} still stores 17 — cell state is NOT wiped to 0");
    }
  }
}

// ---- rate scaling: A-H3 — CellState chains across sub-pulls (Memor) ----
// The review's failure scenario verbatim: Memor at x2, input v changes.
// Sub-pull 1 stores v (m.prevIn = v); sub-pull 2 used to re-read the
// PRE-TICK state (old stored S0) and, seeing v == m.prevIn, keep S0 —
// the published final step silently discarded the store.
void testRateMemorChaining() {
  std::printf("== handshake rate A-H3 (CellState chaining across sub-pulls; Memor at x1/x2/x4)\n");
  struct Mini { ChunkMap w; SignalSim sim; };
  const SignalSim::DirtyFn noDirty = [](int, int, int) {};
  auto buildBase = [](Mini& m) {
    m.w.setRegistry(&blockRegistry());
    for (int cz = 0; cz < 2; ++cz)
      for (int cx = 0; cx < 2; ++cx) {
        auto col = std::make_unique<ChunkColumn>(); col->cx = cx; col->cz = cz;
        m.w.insert(std::move(col));
      }
    for (int z = 0; z < 8; ++z)
      for (int x = 0; x < 8; ++x) m.w.set(x, 9, z, kWaferSilicon);
  };
  // Slow path: the west wire is driven to 17 by hand before the first
  // tick (fullPass forces the generic input lookup); one tick must store.
  auto slowStore = [&noDirty, &buildBase](litholattice::TickRate rate) {
    Mini m; buildBase(m);
    auto put = [&m](int x, int y, int z, BlockId b) { m.w.set(x, y, z, b); m.sim.onBlockChanged(x, y, z, b); };
    put(1, 10, 0, kRedTrace);
    m.w.set(2, 10, 0, kMemor); m.w.setState(2, 10, 0, makeState(0, 1)); m.sim.onBlockChanged(2, 10, 0, kMemor);  // facing E
    put(3, 10, 0, kBlueTrace);
    if (!rate.neutral()) m.sim.setClassTickRate(kMemor, rate);
    m.w.setState(1, 10, 0, CellState(17));
    m.sim.invalidateNetwork();
    m.sim.tick(m.w, 0, 0, 100, noDirty);
    return stValue(m.w.getState(2, 10, 0));
  };
  // Fast path: nets are built and settled first, then a switch drives
  // the west wire for real; the storing tick runs the cached-pointer
  // input path. The switch toggle forces one full pass (wire seeds at
  // 64), the NEXT tick is the fast-path store.
  auto fastStore = [&noDirty, &buildBase](litholattice::TickRate rate) {
    Mini m; buildBase(m);
    auto put = [&m](int x, int y, int z, BlockId b) { m.w.set(x, y, z, b); m.sim.onBlockChanged(x, y, z, b); };
    put(0, 10, 0, kSwitch);
    put(1, 10, 0, kRedTrace);
    m.w.set(2, 10, 0, kMemor); m.w.setState(2, 10, 0, makeState(0, 1)); m.sim.onBlockChanged(2, 10, 0, kMemor);  // facing E
    put(3, 10, 0, kBlueTrace);
    if (!rate.neutral()) m.sim.setClassTickRate(kMemor, rate);
    m.sim.tick(m.w, 0, 0, 100, noDirty);
    m.sim.tick(m.w, 0, 0, 100, noDirty);       // nets built, fast path live
    m.sim.use(m.w, 0, 10, 0, noDirty);         // switch on
    m.sim.tick(m.w, 0, 0, 100, noDirty);       // full pass: wire seeded 64
    m.sim.tick(m.w, 0, 0, 100, noDirty);       // fast-path tick: Memor stores
    return stValue(m.w.getState(2, 10, 0));
  };
  using litholattice::TickRate;
  const int slow1 = slowStore(TickRate{}), slow2 = slowStore(TickRate{2, 1, 0}), slow4 = slowStore(TickRate{4, 1, 0});
  std::printf("  A-H3 slow-path Memor stores: x1 %d x2 %d x4 %d (want 17/17/17)\n", slow1, slow2, slow4);
  check(slow1 == 17, "RATE/A-H3: Memor x1 stores 17 (slow path, baseline)");
  check(slow2 == 17 && slow4 == 17, "RATE/A-H3: Memor x2 and x4 store the same 17 x1 stores (slow path; the store is not discarded)");
  const int fast1 = fastStore(TickRate{}), fast2 = fastStore(TickRate{2, 1, 0}), fast4 = fastStore(TickRate{4, 1, 0});
  std::printf("  A-H3 fast-path Memor stores: x1 %d x2 %d x4 %d (want 64/64/64)\n", fast1, fast2, fast4);
  check(fast1 == 64, "RATE/A-H3: Memor x1 stores 64 (fast path, baseline)");
  check(fast2 == 64 && fast4 == 64, "RATE/A-H3: Memor x2 and x4 store the same 64 x1 stores (fast path; the store is not discarded)");
}

// ---- rate scaling: perf-headless style snapshot on the real SimThread ----
void testRatePerfSnapshot() {
  std::printf("== handshake rate perf snapshot (512 TPS SimThread, div8 + mul4 components)\n");
  Scenario sc = buildScenario();
  ChunkMap& w = sc.wafer->chunks;
  litholattice::SimConfig cfg;
  cfg.tickRate = 512;
  cfg.radiusChunks = 8;
  cfg.belowWorldFill = sc.wafer->belowWorldFill;
  cfg.timingHandshake = true;
  cfg.handshakePhasesPerCycle = 4;
  cfg.handshakeWorkerThreads = 2;
  litholattice::ClockDomain domDiv4;
  domDiv4.name = "perf.div4"; domDiv4.hz = 128; domDiv4.enabled = true;
  litholattice::ClockDomain domDiv16;
  domDiv16.name = "perf.div16"; domDiv16.hz = 32; domDiv16.enabled = true;
  cfg.domains = {domDiv4, domDiv16};
  auto sim = std::make_unique<litholattice::SimThread>(w, blockRegistry(), std::move(sc.ws), std::move(cfg),
                                                       [](std::shared_ptr<litholattice::MeshInput>) {});
  std::atomic<uint64_t> counterDiv{0}, counterMul{0};
  uint32_t idDiv = UINT32_MAX, idMul = UINT32_MAX;
  litholattice::TimingHandshakeAuthority* auth = sim->handshakeAuthority();
  check(auth != nullptr, "RATE: perf: authority constructed in handshake mode");
  if (auth) {  // pre-start only: register the rate-scaled components on the production authority
    litholattice::HsComponentDesc dd;
    dd.name = "rate.perf.div8"; dd.rate = litholattice::TickRate{1, 8, 0};
    idDiv = auth->registerComponent(dd, [&counterDiv](uint64_t, uint64_t) { counterDiv.fetch_add(1, std::memory_order_relaxed); });
    litholattice::HsComponentDesc dm;
    dm.name = "rate.perf.mul4"; dm.rate = litholattice::TickRate{4, 1, 0};
    idMul = auth->registerComponent(dm, [&counterMul](uint64_t, uint64_t) { counterMul.fetch_add(1, std::memory_order_relaxed); });
  }
  check(idDiv != UINT32_MAX && idMul != UINT32_MAX, "RATE: perf: div8 and mul4 components registered pre-start");
  sim->setPlayerChunk(4, 4);
  sim->start();
  std::this_thread::sleep_for(std::chrono::seconds(3));
  const litholattice::SimStats& s = sim->stats();
  const uint64_t worldTick = s.worldTick;
  const float tickMs = s.tickMs;
  const float dom0 = s.domains[0].achieved, dom1 = s.domains[1].achieved;
  check(!sim->handshakeFrozen(), "RATE: perf: authority not frozen after 3 s at 512 TPS");
  check(sim->stop(), "RATE: perf: stop() returns true (no worker outstanding)");
  uint64_t firesDiv = 0, firesMul = 0;
  if (auth) {
    const litholattice::HsStats snap = auth->statsSnapshot();
    if (idDiv < snap.perComponent.size()) firesDiv = snap.perComponent[idDiv].fires;
    if (idMul < snap.perComponent.size()) firesMul = snap.perComponent[idMul].fires;
  }
  check(firesDiv > 0 && firesMul > 0, "RATE: perf: div8 and mul4 components both fired");
  check(counterDiv.load() == firesDiv, "RATE: perf: div8 pulls == stats fires (multiply 1; the fold counts every committed firing)");
  check(counterMul.load() == 4 * firesMul, "RATE: perf: mul4 pulls == 4 x stats fires (sub-pulls inside the firing)");
  check(worldTick >= uint64_t(512 * 3 / 2), "RATE: perf: worldTick advanced >= 512*3*0.5 in 3 s (loose wall-clock sanity)");
  check(dom0 >= 64.f && dom0 <= 192.f, "RATE: perf: domain[0] (128 Hz) achieved within [64, 192] (loose)");
  std::printf("  512 TPS [handshake, rate-scaled domains 512/128/32 + div8/mul4 components]: world ticks %llu in 3 s (%.0f achieved TPS), tick avg %.3f ms, div8 pulls %llu (fires %llu), mul4 pulls %llu (fires %llu), domain Hz %.1f / %.1f\n",
              (unsigned long long)worldTick, double(worldTick) / 3.0, double(tickMs),
              (unsigned long long)counterDiv.load(), (unsigned long long)firesDiv,
              (unsigned long long)counterMul.load(), (unsigned long long)firesMul,
              double(dom0), double(dom1));
}

}  // namespace

// ---- exports for the control-serialization harness (same world, same hash) ----
ControlScenarioWorld buildControlScenarioWorld() {
  Scenario sc = buildScenario();
  ControlScenarioWorld out;
  out.reg = std::move(sc.reg);
  out.wafer = sc.wafer;
  out.ws = std::move(sc.ws);
  out.y = sc.y;
  out.wireX = sc.wireX;
  out.wireZ = sc.wireZ;
  out.repX = sc.repX;
  out.repZ = sc.repZ;
  return out;
}

uint64_t hashControlWorld(const ChunkMap& world, const SignalSim& sig, uint64_t ticks) {
  return hashWorld(world, sig, ticks);
}

int runHandshakeScenario(int ticks, const std::string& mode, int workers, int phases) {
  const bool hs = mode == "handshake";
  const uint64_t h = runScenario(ticks, hs, workers, phases);
  std::printf("handshake-scenario ticks=%d mode=%s workers=%d phases=%d hash=%016llx\n", ticks, mode.c_str(),
              hs ? workers : 0, hs ? phases : 0, (unsigned long long)h);
  return 0;
}

int runHandshakeSelfTest() {
  const int before = gFails;
  std::printf("Timing Handshake determinism (256-tick circuit scenario, hs-design §6.4)\n");
  {  // the air-sub-chunk shortcut must equal the naive per-cell loop
    uint64_t naive = 12345;
    for (int i = 0; i < kSubChunkVolume; ++i) {
      naive = DeterministicScheduler::hashU64(kAirStableId, naive);
      naive = DeterministicScheduler::hashU64(0, naive);
    }
    check(hashAirSubChunk(12345) == naive, "handshake: outcome-hash air sub-chunk matches the naive loop");
  }
  const uint64_t legacy = runScenario(256, false, 0, 0);
  const uint64_t hs14 = runScenario(256, true, 1, 4);
  const uint64_t hs44 = runScenario(256, true, 4, 4);
  const uint64_t hs48 = runScenario(256, true, 4, 8);
  std::printf("  hashes: legacy=%016llx hs(w1,p4)=%016llx hs(w4,p4)=%016llx hs(w4,p8)=%016llx\n",
              (unsigned long long)legacy, (unsigned long long)hs14, (unsigned long long)hs44, (unsigned long long)hs48);
  check(hs14 == legacy, "handshake: 256-tick scenario hash matches legacy (workers 1, phases 4)");
  check(hs44 == legacy, "handshake: 256-tick scenario hash matches legacy (workers 4, phases 4)");
  check(hs48 == legacy, "handshake: 256-tick scenario hash matches legacy (workers 4, phases 8)");
  return gFails - before;
}

int runHandshakeTests() {
  gFails = 0;
  testWait();
  testOffenderNamePropagation();
  testFreezeBare();
  testFreezeEndToEnd();
  testStopStartSoak();
  testStuckWorkerStop();
  testRateDivideScheduling();
  testRateMultiplyOrdering();
  testRateCrossing();
  testRateSamePhaseCrossing();
  testRateShuffleInvariance();
  testRateGameLayer();
  testRateInvalidClassRates();
  testRateMemorChaining();
  std::printf("== handshake determinism scenario (256 ticks, summary; full matrix via --handshake-scenario)\n");
  runHandshakeSelfTest();
  testRatePerfSnapshot();
  std::printf("%s\n", gFails ? "HANDSHAKE TESTS: FAILED" : "HANDSHAKE TESTS: OK");
  return gFails ? 1 : 0;
}

}  // namespace lithokraft
