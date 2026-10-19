// Timing Handshake authority + pull workers (hs-design.md §1, §2).
// Determinism is a property of the protocol (snapshot reads, barrier commit),
// never of the schedule: phases publish strictly in order, a worker cannot
// see phase N+1 until the authority has committed N, and the barrier — not
// the poll — is the synchroniser.
#include "litholattice/sim/timing_handshake.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <stdexcept>

#include "litholattice/core/fail_fast.h"
#include "litholattice/core/log.h"

namespace litholattice {

namespace {
// Sub-pull context (A-H1 mechanism-lite): the frame of the item this
// thread is currently executing inside runItem(). Thread-local because
// items execute concurrently on the pull workers (and inline on the
// authority thread for affine items); each executing thread sees only
// its own item's frame. The scope object saves/restores the previous
// frame, so a WorkFn that (unusually) drives another authority inline
// cannot corrupt the outer item's context, and a throwing WorkFn still
// restores it (the destructor runs as the try block unwinds).
struct SubPullFrame {
  const TimingHandshakeAuthority* auth = nullptr;
  uint32_t index = 0;
  uint32_t count = 1;
};
thread_local SubPullFrame tSubPull;
struct SubPullScope {
  SubPullFrame prev;
  explicit SubPullScope(const TimingHandshakeAuthority* a, uint32_t i, uint32_t n) : prev(tSubPull) {
    tSubPull.auth = a; tSubPull.index = i; tSubPull.count = n;
  }
  ~SubPullScope() { tSubPull = prev; }
  SubPullScope(const SubPullScope&) = delete;
  SubPullScope& operator=(const SubPullScope&) = delete;
};
}  // namespace

uint32_t TimingHandshakeAuthority::currentSubPullIndex() const {
  return tSubPull.auth == this ? tSubPull.index : 0;
}
uint32_t TimingHandshakeAuthority::currentSubPullCount() const {
  return tSubPull.auth == this ? tSubPull.count : 1;
}
bool TimingHandshakeAuthority::isFinalSubPull() const {
  return currentSubPullIndex() + 1 >= currentSubPullCount();
}

uint64_t TimingHandshakeAuthority::nowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

TimingHandshakeAuthority::TimingHandshakeAuthority(Config cfg) : cfg_(std::move(cfg)) {
  if (cfg_.phasesPerCycle < 1) cfg_.phasesPerCycle = 1;
  if (cfg_.phasesPerCycle > 64) cfg_.phasesPerCycle = 64;
  ceiling_ = cfg_.watchdogCeiling.count() > 0 ? cfg_.watchdogCeiling : std::chrono::milliseconds(250);
  if (cfg_.workerThreads > 0) {
    workerCount_ = cfg_.workerThreads;
  } else {
    const unsigned hw = std::thread::hardware_concurrency();
    workerCount_ = int(std::min(4u, hw > 1 ? hw - 1 : 1u));
    if (workerCount_ < 1) workerCount_ = 1;
  }
  shards_.assign(size_t(workerCount_), {});
  shardDoneSeq_.clear();
  for (int i = 0; i < workerCount_; ++i) shardDoneSeq_.emplace_back(0);
  workerExited_.clear();
  for (int i = 0; i < workerCount_; ++i) workerExited_.emplace_back(false);
}

TimingHandshakeAuthority::~TimingHandshakeAuthority() {
  if (!stop()) {
    // C2 contract: a poisoned authority must not be destroyed while a
    // worker is outstanding — its members are exactly what the worker
    // dereferences. Owners that must outlive a stuck worker (SimThread)
    // deliberately LEAK the authority instead of destroying it. Reaching
    // this destructor with a worker outstanding is a caller bug: fail
    // loudly here rather than use-after-free in the member teardown.
    logError("timing-handshake: authority destroyed with a worker still outstanding — aborting (caller must retry stop() until it succeeds, or leak the authority)");
    std::abort();
  }
}

uint32_t TimingHandshakeAuthority::registerComponent(HsComponentDesc desc, WorkFn fn) {
  if (startedOnce_) {
    logError("timing-handshake: registerComponent('%s') after start() — registration is frozen", desc.name);
    return UINT32_MAX;
  }
  // Rate scaling (JD 2026-10-14): refuse an invalid tick rate at
  // registration — powers of two within the caps (see tick_rate.h) — so a
  // malformed rate can never reach dueFor()/runItem() and silently skew
  // a component's cadence.
  if (!desc.rate.valid()) {
    logError("timing-handshake: registerComponent('%s') refused — invalid tick rate (multiply %u, divide %u): powers of two, multiply <= %u, divide <= %u",
             desc.name ? desc.name : "", desc.rate.multiply, desc.rate.divide, kMaxRateMultiply, kMaxRateDivide);
    return UINT32_MAX;
  }
  const uint32_t id = uint32_t(comps_.size());
  desc.id = id;
  // Own the name (P1-WAIT / sweep §5): copy it into authority storage NOW.
  // Callers pass pointers into their own bookkeeping — e.g. SSO strings in
  // a std::vector<std::string> that reallocates as more components register
  // (exactly how 'signal.mark'/'signal.prep' came to be flagged as '').
  // An empty/null registration gets a synthesized non-empty name so the
  // offender diagnostic can never log ''.
  std::string ownedName = desc.name ? desc.name : "";
  if (ownedName.empty()) ownedName = "unnamed-" + std::to_string(id);
  ownedNames_.push_back(std::move(ownedName));
  desc.name = ownedNames_.back().c_str();
  // Name index (JD 2026-10-14, rate scaling): first registration of a
  // name wins (emplace never overwrites); lookup only — see
  // componentIdByName in the header.
  compIndex_.emplace(ownedNames_.back(), id);
  Comp c;
  c.desc = desc;
  c.fn = std::move(fn);
  if (!desc.threadAffine && desc.tickCritical && desc.cadence != CadenceClass::FreeRun) {
    // Static round-robin by registration order (mirrors the prototype).
    size_t target = 0;
    for (size_t s = 1; s < shards_.size(); ++s)
      if (shards_[s].size() < shards_[target].size()) target = s;
    // Round-robin strictly: choose the shard whose turn it is. The
    // least-loaded pick above equals round-robin for sequential registration.
    c.shard = int(target);
    shards_[target].push_back(id);
  }
  comps_.push_back(std::move(c));
  HsComponentStats cs;
  cs.name = ownedNames_.back();
  stats_.perComponent.push_back(std::move(cs));
  cycleAccum_.push_back(CycleAccum{});
  return id;
}

uint32_t TimingHandshakeAuthority::componentIdByName(std::string_view name) const {
  // Lookup only (JD 2026-10-14, rate scaling): the unordered_map index is
  // never iterated for scheduling — execution order is registration id
  // order, always.
  const auto it = compIndex_.find(std::string(name));
  return it != compIndex_.end() ? it->second : UINT32_MAX;
}

void TimingHandshakeAuthority::setCommitFn(std::function<void(uint64_t, uint64_t, uint32_t)> fn) {
  // Review A-L9: a second installation replaces the first hook — previously
  // silently, so a Config-supplied hook would vanish the moment a subsystem
  // (Signal) registered its own. The replacement semantics stay (last
  // registration wins, matching Signal's production wiring), but they are
  // now loud: a warning in all builds plus a debug assert for test runs.
  if (cfg_.commitFn) {
    logWarn("timing-handshake: setCommitFn overwrites an already-installed commit hook; the previous hook will no longer run");
    assert(!cfg_.commitFn && "setCommitFn: overwriting an installed commit hook (review A-L9)");
  }
  cfg_.commitFn = std::move(fn);
}

std::chrono::nanoseconds TimingHandshakeAuthority::phaseBudgetForGroup(uint32_t group) const {
  if (group == 1 && cfg_.phaseBudgetGroup1.count() > 0) return cfg_.phaseBudgetGroup1;
  if (group == 2 && cfg_.phaseBudgetGroup2.count() > 0) return cfg_.phaseBudgetGroup2;
  return cfg_.phaseBudget;
}

std::chrono::nanoseconds TimingHandshakeAuthority::cycleBudgetForGroup(uint32_t group) const {
  const std::chrono::nanoseconds pb = phaseBudgetForGroup(group);
  if (pb.count() <= 0) return pb;  // accounting off for this group
  // Group 2 runs ONE phase per master step: its cycle is that single phase.
  // Every other group runs phasesPerCycle phases per cycle (the four Signal
  // phases are one logical tick — sweep decision C2).
  if (group == 2) return pb;
  return pb * cfg_.phasesPerCycle;
}

bool TimingHandshakeAuthority::dueFor(const Comp& c, uint64_t phase, uint64_t cycle, uint32_t group) const {
  const HsComponentDesc& d = c.desc;
  if (!d.tickCritical || d.cadence == CadenceClass::FreeRun) return false;
  // Group matching, BOTH directions explicit (A-M1): a group-0 COMPONENT
  // participates in every caller group; a caller group matches only its
  // own group — group 0 is a real group on the caller side, NOT a
  // wildcard. The one caller wildcard is kAllGroups.
  if (group != kAllGroups && d.group != 0 && d.group != group) return false;
  uint32_t divider = 1;
  switch (d.cadence) {
    case CadenceClass::EveryPhase: divider = 1; break;
    case CadenceClass::HalfCycle: divider = uint32_t(std::max(1, cfg_.phasesPerCycle / 2)); break;
    case CadenceClass::CycleHead: divider = uint32_t(cfg_.phasesPerCycle); break;
    case CadenceClass::FreeRun: return false;
  }
  if (d.phaseDivider != 0) divider = d.phaseDivider;  // general form wins when set (0 = unset; explicit 1 forces every-phase, A-L9)
  if (divider > 1 && (phase % divider) != (d.phaseOffset % divider)) return false;
  // Rate scaling (JD 2026-10-14): the rate divide gates on the CYCLE (the
  // caller's tick), phase-aligned via the rate's own phase — the cycle-axis
  // generalization of the phase-axis divider test above, in this same
  // gate. Neutral rates skip the test entirely so default behaviour is
  // bit-identical.
  if (d.rate.divide > 1 && (cycle % d.rate.divide) != (d.rate.phase % d.rate.divide)) return false;
  return true;
}

bool TimingHandshakeAuthority::runItem(Comp& c, uint64_t phase, uint64_t cycleTick) {
  const uint64_t t0 = nowNs();
  c.startStampNs.store(t0, std::memory_order_release);
  c.started.store(true, std::memory_order_release);
  bool ok = true;
  try {
    // Rate scaling (JD 2026-10-14): sub-pulls execute in ascending order
    // inside the item's phase slot — they are rounds inside the slot and
    // do NOT consume phase slots. One try/catch, one started/finished
    // pair, and one duration spanning all sub-pulls — own-work
    // attribution is unchanged.
    // Visibility (A-H1 corrected contract, tick_rate.h): this loop is
    // ALL the authority does — there is no staging or publish step, so
    // a WorkFn writing shared state publishes immediately, even mid-
    // firing. Final-only visibility is the WorkFn's obligation, and it
    // is knowable: each invocation runs under a SubPullScope, so the
    // WorkFn can query currentSubPullIndex() / currentSubPullCount() /
    // isFinalSubPull() on this authority (or count its own invocations)
    // and publish its staged state only on the final sub-pull.
    const uint32_t subs = c.desc.rate.multiply >= 1 ? c.desc.rate.multiply : 1;
    for (uint32_t s = 0; s < subs; ++s) {
      SubPullScope scope(this, s, subs);
      if (c.fn) c.fn(phase, cycleTick);
    }
  } catch (const std::exception& e) {
    // C3: a throwing pull component must not escape the worker thread
    // (std::terminate, no report). Record it; runPhase converts it via
    // simFailFastFromException at the next phase boundary, before commit.
    recordFailure(c, phase, cycleTick, e.what());
    ok = false;
  } catch (...) {
    recordFailure(c, phase, cycleTick, "unknown non-std::exception escaped a handshake component");
    ok = false;
  }
  const uint64_t t1 = nowNs();
  c.durationNs.store(t1 - t0, std::memory_order_release);
  c.finished.store(true, std::memory_order_release);
  return ok;
}

void TimingHandshakeAuthority::recordFailure(const Comp& c, uint64_t phase, uint64_t cycleTick,
                                             const char* what) {
  {
    std::lock_guard lk(failureM_);
    if (!pendingFailure_.present) {  // first failure wins
      pendingFailure_.present = true;
      pendingFailure_.message = what ? what : "unknown exception";
      pendingFailure_.componentName = c.desc.name ? c.desc.name : "";
      pendingFailure_.componentId = c.desc.id;
      pendingFailure_.phase = phase;
      pendingFailure_.cycleTick = cycleTick;
    }
  }
  // No further phase may commit past a failed component: trip the freeze
  // latch now; the authority thread performs the fail-fast conversion at
  // the runPhase boundary (see convertPendingFailureIfAny).
  frozen_.store(true, std::memory_order_release);
  logError("timing-handshake: component '%s' (id %u) threw in phase %llu (cycle %llu): %s",
           c.desc.name, c.desc.id, (unsigned long long)phase, (unsigned long long)cycleTick,
           what ? what : "unknown exception");
}

void TimingHandshakeAuthority::convertPendingFailureIfAny() {
  PendingFailure f;
  {
    std::lock_guard lk(failureM_);
    if (!pendingFailure_.present) return;
    f = pendingFailure_;
  }
  // Mirror SimThread's tickGuard: rebuild the exception from the recorded
  // text (the original object died on the worker thread) and take the
  // standard fail-fast path — report, then abort. Never log-and-continue:
  // the world may be half-updated by the throwing component.
  const std::string msg = "timing-handshake: component '" + f.componentName + "' (id " +
                          std::to_string(f.componentId) + ") threw in phase " +
                          std::to_string(f.phase) + " (cycle " + std::to_string(f.cycleTick) +
                          "): " + f.message;
  simFailFastFromException(std::runtime_error(msg), "timing_handshake", f.cycleTick);
}

bool TimingHandshakeAuthority::readPublished(PublishedPhase& out) const {
  // CH-H2: seqlock-style revalidation around the publication tuple.
  const uint64_t gen1 = generation_.load(std::memory_order_acquire);
  const uint64_t seq1 = publishSeq_.load(std::memory_order_acquire);
  if (seq1 == 0) return false;
  out.phase = publishedPhase_.load(std::memory_order_acquire);
  out.cycle = publishedCycle_.load(std::memory_order_acquire);
  out.group = publishedGroup_.load(std::memory_order_acquire);
  out.openStampNs = phaseOpenStamp_.load(std::memory_order_acquire);
  out.dueWork = false;
  const uint64_t seq2 = publishSeq_.load(std::memory_order_acquire);
  const uint64_t gen2 = generation_.load(std::memory_order_acquire);
  if (seq1 != seq2 || gen1 != gen2) return false;  // tuple raced a publish/reset: caller retries
  out.seq = seq1;
  out.generation = gen1;
  return true;
}

void TimingHandshakeAuthority::resetAfterFreeze() {
  // CH-H1: bump the generation under m_ first — a stale worker's barrier
  // arrival revalidates (seq, generation) under the same mutex, so it can
  // no longer be counted toward the next phase — then clear the latch.
  {
    std::lock_guard lk(m_);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    arrivals_.store(0, std::memory_order_relaxed);
  }
  frozen_.store(false, std::memory_order_release);
  publishCv_.notify_all();
}

bool TimingHandshakeAuthority::start() {
  if (workersOutstanding_.load(std::memory_order_acquire)) {
    // C2 poison: an old worker is still running against this authority
    // (and, through its WorkFn captures, the world). Refuse to spawn a
    // second pool alongside it.
    logError("timing-handshake: start() refused — a worker from a failed stop() is still outstanding");
    return false;
  }
  if (running_.exchange(true)) return true;  // already running
  startedOnce_ = true;
  stop_.store(false);
  frozen_.store(false);
  {
    // C1: reset ALL publication state before spawning. Previously
    // publishSeq_/shardDoneSeq_/arrivals_/phaseOpenStamp_ survived a
    // stop()/start() cycle, and a fresh worker (seenSeq = 0) treated the
    // stale last publication as new work and re-executed it outside any
    // tick/barrier. The generation bump additionally invalidates any
    // snapshot a worker from a previous life could still be holding.
    std::lock_guard lk(m_);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    publishSeq_.store(0, std::memory_order_release);
    publishedPhase_.store(0, std::memory_order_relaxed);
    publishedCycle_.store(0, std::memory_order_relaxed);
    publishedGroup_.store(0, std::memory_order_relaxed);
    phaseOpenStamp_.store(0, std::memory_order_relaxed);
    committedPhase_.store(0, std::memory_order_relaxed);
    arrivals_.store(0, std::memory_order_relaxed);
    for (auto& d : shardDoneSeq_) d.store(0, std::memory_order_relaxed);
  }
  {
    std::lock_guard lk(failureM_);
    pendingFailure_ = PendingFailure{};
  }
  for (auto& e : workerExited_) e.store(false);
  workers_.clear();
  for (int s = 0; s < workerCount_; ++s)
    workers_.emplace_back([this, s] { workerLoop(s); });
  return true;
}

bool TimingHandshakeAuthority::stop() {
  if (!startedOnce_ && workers_.empty()) return true;
  stop_.store(true, std::memory_order_release);
  publishCv_.notify_all();
  barrierCv_.notify_all();
  if (workers_.empty()) { running_.store(false); return true; }
  // NO-DETACH bounded join (C2; L7: per-worker deadline, saturating
  // multiply — previously one global deadline was shared by all workers
  // and ceiling x 2 could overflow). Each worker gets its own full grace
  // of ceiling x 2 to leave its component and the poll loop.
  const uint64_t ceilNs = uint64_t(ceiling_.count());
  const uint64_t graceNs = ceilNs > UINT64_MAX / 2 ? UINT64_MAX : ceilNs * 2;
  bool allJoined = true;
  for (size_t i = 0; i < workers_.size(); ++i) {
    if (!workers_[i].joinable()) continue;
    const uint64_t start = nowNs();
    const uint64_t deadline = graceNs > UINT64_MAX - start ? UINT64_MAX : start + graceNs;
    while (!workerExited_[i].load(std::memory_order_acquire) && nowNs() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (workerExited_[i].load(std::memory_order_acquire)) {
      workers_[i].join();
    } else {
      // Fail loudly and poison (see the header for the full protocol):
      // the thread object stays joinable in workers_, the worker keeps
      // running against LIVE storage, and stop() may be retried once the
      // component returns. Never detach into freed storage.
      logError("timing-handshake: worker %zu still inside a component at stop(); NOT detaching — "
               "stop() FAILED, authority poisoned until a retry joins it (no save/start/destroy is safe)",
               i);
      allJoined = false;
    }
  }
  if (allJoined) {
    workers_.clear();
    workersOutstanding_.store(false, std::memory_order_release);
    running_.store(false);
    return true;
  }
  workersOutstanding_.store(true, std::memory_order_release);
  // running_ stays true: the authority is not stopped while a worker is
  // outstanding, and start() refuses via workersOutstanding_ above.
  return false;
}

bool TimingHandshakeAuthority::pollPhase(uint32_t shard, uint64_t seenSeq, PublishedPhase& out) const {
  // THE poll (A-M3): snapshot read + newness + due scan in exactly one
  // place. workerLoop and canTick both go through here.
  if (shard >= shards_.size()) return false;
  if (!readPublished(out)) return false;
  if (out.seq == seenSeq) return false;  // nothing newer for this caller
  out.dueWork = false;
  if (shardDoneSeq_[shard].load(std::memory_order_acquire) != out.seq) {
    for (uint32_t id : shards_[shard])
      if (dueFor(comps_[id], out.phase, out.cycle, out.group)) { out.dueWork = true; break; }
  }
  return true;
}

bool TimingHandshakeAuthority::canTick(uint32_t shard, uint64_t& phaseOut) const {
  if (shard >= shards_.size()) return false;
  // "Not yet pushed for this publication" == the shard's done-seq is the
  // caller's seen marker: poll for anything newer than what it pushed.
  PublishedPhase snap;
  if (!pollPhase(shard, shardDoneSeq_[shard].load(std::memory_order_acquire), snap)) return false;
  if (!snap.dueWork) return false;
  phaseOut = snap.phase;
  return true;
}

HsStats TimingHandshakeAuthority::statsSnapshot() const {
  std::lock_guard lk(statsM_);
  return stats_;
}

void TimingHandshakeAuthority::workerLoop(int shard) {
  uint64_t seenSeq = 0;
  uint64_t idleSinceNs = nowNs();
  while (!stop_.load(std::memory_order_acquire)) {
    // A-M3: the worker polls through pollPhase() — the SAME implementation
    // canTick() exposes — never an inline re-implementation. CH-H2: the
    // snapshot it returns is revalidated, never a tuple assembled from
    // separate loads across a publish.
    PublishedPhase snap;
    if (pollPhase(uint32_t(shard), seenSeq, snap)) {
      seenSeq = snap.seq;
      idleSinceNs = nowNs();
      const uint64_t ph = snap.phase;
      const uint64_t ct = snap.cycle;
      const uint32_t g = snap.group;
      if (snap.dueWork) {
        // Clock-monotonicity tripwire (review A-L8, relabelled): both stamps
        // come from the same shared steady clock and this pull stamp is read
        // after the open stamp was stored, so pullStamp < openStamp can only
        // mean the steady clock itself went backwards. This verifies clock
        // monotonicity — it is NOT a check that two distinct QPC sources
        // agree (there is only one clock here by design).
        const uint64_t pullStamp = nowNs();
        if (pullStamp < snap.openStampNs)
          qpcViolations_.fetch_add(1, std::memory_order_relaxed);
        bool stale = false;
        for (uint32_t id : shards_[size_t(shard)]) {
          Comp& c = comps_[id];
          if (!dueFor(c, ph, ct, g)) continue;
          // CH-H1: revalidate the generation before every item — after a
          // resetAfterFreeze()/restart this publication is stale and its
          // remaining items must not execute.
          if (generation_.load(std::memory_order_acquire) != snap.generation) { stale = true; break; }
          // C3: on a recorded failure, stop executing this publication's
          // remaining items (the world may be half-updated) but still
          // arrive at the barrier below so runPhase can convert the
          // failure at the phase boundary instead of hanging to the
          // watchdog ceiling.
          if (!runItem(c, ph, ct)) break;
        }
        if (!stale) {
          // CH-H1: the arrival itself revalidates (seq, generation) under
          // m_ — runPhase resets arrivals_ under the same mutex when it
          // publishes, so a stale arrival can never be counted toward a
          // later phase's barrier.
          bool counted = false;
          {
            std::lock_guard lk(m_);
            if (publishSeq_.load(std::memory_order_relaxed) == snap.seq &&
                generation_.load(std::memory_order_relaxed) == snap.generation) {
              shardDoneSeq_[size_t(shard)].store(snap.seq, std::memory_order_release);
              arrivals_.fetch_add(1, std::memory_order_relaxed);
              counted = true;
            }
          }
          if (counted) barrierCv_.notify_all();
        }
      } else {
        std::lock_guard lk(m_);
        if (publishSeq_.load(std::memory_order_relaxed) == snap.seq &&
            generation_.load(std::memory_order_relaxed) == snap.generation)
          shardDoneSeq_[size_t(shard)].store(snap.seq, std::memory_order_release);
      }
      continue;
    }
    if (cfg_.pureSpin) { std::this_thread::yield(); continue; }
    // Hybrid poll (D9): spin-with-yield for ~50 us, then block on the CV.
    if (nowNs() - idleSinceNs < 50'000) { std::this_thread::yield(); continue; }
    std::unique_lock lk(m_);
    publishCv_.wait_for(lk, std::chrono::milliseconds(1), [&] {
      return stop_.load(std::memory_order_acquire) ||
             publishSeq_.load(std::memory_order_acquire) != seenSeq;
    });
  }
  if (shard >= 0 && size_t(shard) < workerExited_.size())
    workerExited_[size_t(shard)].store(true, std::memory_order_release);
}

bool TimingHandshakeAuthority::runPhase(uint64_t phase, uint64_t cycleTick, uint32_t group) {
  // C3: a worker failure recorded since the last boundary converts here,
  // before anything else in this phase (noreturn: fail-fast abort).
  convertPendingFailureIfAny();
  if (frozen_.load(std::memory_order_acquire)) return false;
  const uint64_t openStamp = nowNs();
  phaseOpenStamp_.store(openStamp, std::memory_order_release);

  // Due set, in registration (id) order; active shards = shards with at
  // least one due non-affine component.
  std::vector<Comp*> affine;
  std::vector<char> shardActive(size_t(workerCount_), 0);
  uint32_t activeShards = 0;
  for (auto& c : comps_) {
    c.started.store(false, std::memory_order_relaxed);
    c.finished.store(false, std::memory_order_relaxed);
    c.durationNs.store(0, std::memory_order_relaxed);
    if (!dueFor(c, phase, cycleTick, group)) continue;
    if (c.shard >= 0) {
      if (!shardActive[size_t(c.shard)]) { shardActive[size_t(c.shard)] = 1; ++activeShards; }
    } else if (c.desc.threadAffine) {
      affine.push_back(&c);
    }
  }

  const bool haveWorkers = running_.load(std::memory_order_acquire) && !workers_.empty();
  // Affine work runs BEFORE the phase is published: affine items are the
  // phase prologue (e.g. Signal's mark/prep prepares the very work the pull
  // shards then execute), so workers must not see the phase until every
  // affine item has finished. The pull stamp still post-dates the open
  // stamp, and the barrier still gates the commit.
  for (Comp* c : affine) runItem(*c, phase, cycleTick);
  // C3: an affine throw is converted here too (bare-authority callers have
  // no tickGuard); in SimThread the abort simply happens one frame earlier
  // than the guard's, through the same simFailFastFromException path.
  convertPendingFailureIfAny();

  uint64_t seq = 0;
  {
    std::lock_guard lk(m_);
    arrivals_.store(0, std::memory_order_relaxed);
    publishedPhase_.store(phase, std::memory_order_relaxed);
    publishedCycle_.store(cycleTick, std::memory_order_relaxed);
    publishedGroup_.store(group, std::memory_order_relaxed);
    seq = publishSeq_.load(std::memory_order_relaxed) + 1;
    publishSeq_.store(seq, std::memory_order_release);
  }
  publishCv_.notify_all();

  uint64_t barrierWaitNs = 0;
  if (activeShards > 0) {
    if (!haveWorkers) {
      // No workers (authority never started): execute shard work inline in
      // id order. Deterministic fallback; the barrier is trivially met.
      for (auto& c : comps_)
        if (c.shard >= 0 && dueFor(c, phase, cycleTick, group)) runItem(c, phase, cycleTick);
      convertPendingFailureIfAny();  // C3: inline path converts before commit
    } else {
      const uint64_t waitStart = nowNs();
      std::unique_lock lk(m_);
      while (arrivals_.load(std::memory_order_acquire) < activeShards) {
        if (barrierCv_.wait_for(lk, std::chrono::milliseconds(1)) == std::cv_status::timeout) {
          const uint64_t t = nowNs();
          if (t - openStamp > uint64_t(ceiling_.count())) {
            // Watchdog: FREEZE. Name the slowest unfinished component:
            // the started-but-unfinished item with the earliest start
            // stamp; a shard that started nothing names its first due
            // component.
            HsFreezeInfo info;
            info.phase = phase; info.cycleTick = cycleTick;
            info.openStampNs = openStamp; info.tripStampNs = t;
            info.elapsedNs = t - openStamp; info.ceilingNs = uint64_t(ceiling_.count());
            const Comp* pick = nullptr;
            uint64_t bestStart = UINT64_MAX;
            const Comp* firstDue = nullptr;
            for (auto& c : comps_) {
              if (c.shard < 0 || !shardActive[size_t(c.shard)] || !dueFor(c, phase, cycleTick, group)) continue;
              if (shardDoneSeq_[size_t(c.shard)].load(std::memory_order_acquire) == seq) continue;
              if (!firstDue) firstDue = &c;
              if (c.started.load(std::memory_order_acquire) && !c.finished.load(std::memory_order_acquire)) {
                const uint64_t s = c.startStampNs.load(std::memory_order_acquire);
                if (s < bestStart) { bestStart = s; pick = &c; }
              }
            }
            if (!pick) pick = firstDue;
            if (pick) { info.componentId = pick->desc.id; info.componentName = pick->desc.name; }
            lastFreeze_ = info;
            frozen_.store(true, std::memory_order_release);
            return false;  // no commit, no force-advance
          }
        }
      }
      barrierWaitNs = nowNs() - waitStart;
    }
  }

  // C3: workers have arrived; a recorded worker failure converts NOW,
  // before the commit hook — a phase with a throwing component is never
  // committed.
  convertPendingFailureIfAny();

  // Commit hook (single, per authority — the game's buffer flip lands here).
  if (cfg_.commitFn) cfg_.commitFn(phase, cycleTick, group);
  committedPhase_.store(phase, std::memory_order_release);

  // Fold timings into stats; WAIT accounting (hs-design §1.6, rewritten
  // P1-WAIT per review CH-H4 + sweep decision C2 + review A-M2).
  //
  // Two separate ledgers, deliberately not conflated:
  // (a) PHASE diagnostics: this phase's wall duration vs THIS GROUP's phase
  //     budget (per-group budgets, A-M2), counted per group and in total.
  //     Wall duration includes affine work, barrier wait and preemption —
  //     it is a load diagnostic and charges NO component.
  // (b) COMPONENT attribution: a component earns an overrun only when the
  //     sum of its OWN runItem durations within one cycle exceeds the
  //     group's CYCLE budget (the phases are one logical tick, C2).
  //     Charging is eager: the moment the running sum crosses the cycle
  //     budget, one overrun is recorded (excess = own sum − cycle budget)
  //     and the sum restarts at zero, so a component must again exceed a
  //     full cycle budget on its own to earn another. This cannot flag an
  //     innocent: the only quantities that enter a component's sum are
  //     intervals measured around that component's own function — never
  //     the phase total, never another component's work, never the
  //     barrier wait or authority-thread preemption. (An item preempted
  //     by the OS mid-run still bills that stall to its own interval;
  //     with the threshold judged per-cycle instead of per-phase, a lone
  //     stall can no longer cross it unless the component's cycle work —
  //     stalls included — truly exceeds a whole tick's budget.)
  const uint64_t dur = nowNs() - openStamp;
  // A-M6: the fold is the stats writer; hold statsM_ so a cross-thread
  // statsSnapshot() never copies a half-folded struct (vector included).
  std::lock_guard statsLk(statsM_);
  stats_.phases += 1;
  stats_.barrierWaitNsTotal += barrierWaitNs;
  stats_.qpcViolations = qpcViolations_.load(std::memory_order_relaxed);
  const uint64_t phaseBudgetNs = uint64_t(phaseBudgetForGroup(group).count());
  const uint64_t cycleBudgetNs = uint64_t(cycleBudgetForGroup(group).count());
  HsGroupStats& gs = stats_.perGroup[group <= 2 ? size_t(group) : 0];
  gs.phases += 1;
  if (phaseBudgetNs > 0 && dur > phaseBudgetNs) {
    stats_.overrunPhases += 1;
    stats_.overrunNsTotal += dur - phaseBudgetNs;
    gs.overrunPhases += 1;
    gs.overrunNsTotal += dur - phaseBudgetNs;
  }
  for (auto& c : comps_) {
    if (!dueFor(c, phase, cycleTick, group)) continue;
    HsComponentStats& cs = stats_.perComponent[c.desc.id];
    cs.fires += 1;
    const uint64_t d = c.durationNs.load(std::memory_order_relaxed);
    if (d > cs.maxWorkNs) cs.maxWorkNs = d;
    if (cycleBudgetNs == 0) continue;
    CycleAccum& a = cycleAccum_[c.desc.id];
    if (!a.active || a.group != group || a.cycleTick != cycleTick) {
      a.group = group;
      a.cycleTick = cycleTick;
      a.workNs = 0;
      a.active = true;
    }
    a.workNs += d;
    // Rate scaling (JD 2026-10-14, on the C2 own-work lineage above): a
    // multiplied component legitimately does multiply-x the work per
    // cycle — runItem's single duration already spans all its sub-pulls —
    // so its cycle budget is judged per granted pulls, scaled by its
    // multiply. Neutral rates scale by 1: accounting for existing
    // components is bit-identical. (cycleBudgetNs == 0 already continued
    // above, so accounting-off groups never reach this scaling.)
    const uint64_t compCycleBudgetNs = cycleBudgetNs * uint64_t(c.desc.rate.multiply >= 1 ? c.desc.rate.multiply : 1);
    if (a.workNs > compCycleBudgetNs) {
      cs.overruns += 1;
      const uint64_t excess = a.workNs - compCycleBudgetNs;
      if (excess > cs.maxOverrunNs) cs.maxOverrunNs = excess;
      a.workNs = 0;  // restart the window: the next charge needs another full cycle budget of own work
      if (!cs.repeatOffender && cs.overruns >= cfg_.offenderThreshold) {
        cs.repeatOffender = true;
        if (!c.offenderLogged) {
          c.offenderLogged = true;
          logWarn("timing-handshake: component '%s' (id %u) is a repeat overrun offender (%llu overruns; "
                  "own work within one cycle exceeded the cycle budget)",
                  cs.name.c_str(), c.desc.id, (unsigned long long)cs.overruns);
        }
      }
    }
  }
  return true;
}

void TimingHandshakeAuthority::resetStats() {
  std::lock_guard lk(statsM_);  // A-M6: same mutex as the fold + snapshots
  HsStats fresh;
  fresh.perComponent.assign(stats_.perComponent.size(), HsComponentStats{});
  for (size_t i = 0; i < fresh.perComponent.size(); ++i)
    fresh.perComponent[i].name = stats_.perComponent[i].name;  // names are registration data, not stats
  stats_ = std::move(fresh);
  qpcViolations_.store(0);
  for (auto& c : comps_) c.offenderLogged = false;
  for (auto& a : cycleAccum_) a = CycleAccum{};  // partial-cycle own work does not survive a stats reset
}

}  // namespace litholattice
