// Timing Handshake authority (pull-tick integration, hs-design.md §1;
// promoted to the standard sim timing path 2026-10-14 by JD's directive).
// The authority (owned by SimThread) publishes phase windows; pull worker
// threads poll via pollPhase() (canTick() is the same poll, exposed as the
// design's public query — ONE implementation, used by the workers
// themselves), execute their shard's due components against the phase
// snapshot, push, and rendezvous at a barrier the authority commits.
// Missed-window policy: WAIT (a phase extends; nothing is ever skipped).
// Watchdog policy: FREEZE (runPhase returns false past the ceiling; the
// caller freezes the sim clock — the authority never force-advances).
// Overrun attribution (P1-WAIT, review CH-H4 + sweep decision C2): phase
// wall time is a per-group diagnostic ONLY — it never charges a component.
// A component earns an overrun only when the sum of its OWN measured work
// inside one cycle exceeds that group's cycle budget (see runPhase). A
// bystander therefore cannot be flagged for another component's work, for
// barrier wait, or for authority-thread preemption.
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "litholattice/sim/tick_rate.h"

namespace litholattice {

enum class CadenceClass : uint8_t {
  EveryPhase,  // fires in every phase slot
  HalfCycle,   // fires on phases where (phase % halfDivider) == phaseOffset, with
               // halfDivider = max(1, phasesPerCycle/2) — INTEGER division, so an
               // odd phasesPerCycle rounds DOWN (5 -> divider 2; review A-L9: the
               // header previously implied an exact half that cannot exist for
               // odd counts; the code's integer form is the definition)
  CycleHead,   // fires on phases where (phase % phasesPerCycle) == phaseOffset
  FreeRun,     // never queries, never rendezvous (non-tick-critical only)
};

/// runPhase caller-group sentinel (review A-M1): a runPhase call with this
/// group fires components of EVERY group. Distinct from group 0, which is
/// a real group: a group-0 runPhase call fires only group-0 components
/// (plus nothing else). The COMPONENT side is the mirror image and is
/// unchanged: a component registered with group 0 participates in every
/// runPhase group. Both directions are deliberate — a bare-authority
/// caller that passes the default group 0 gets ONLY its ungrouped
/// components, never a surprise firing of grouped (Signal/domain) work;
/// callers that truly want everything must say kAllGroups out loud.
inline constexpr uint32_t kAllGroups = UINT32_MAX;

struct HsComponentDesc {
  uint32_t id = 0;                 // stable, registration order; canonical sort key
  const char* name = "";           // diagnostic name (watchdog names this). The authority
                                   // COPIES it at registration and owns the copy for its
                                   // lifetime: the caller's string need not outlive the
                                   // registerComponent() call (see registerComponent).
  CadenceClass cadence = CadenceClass::EveryPhase;
  uint32_t phaseDivider = 0;       // general form: 0 = unset (the cadence class decides).
                                   // Any nonzero value WINS over the cadence class —
                                   // including an explicit 1, which forces every-phase
                                   // firing even for HalfCycle/CycleHead descriptors
                                   // (review A-L9: the old "!= 1 means set" test made
                                   // explicit 1 indistinguishable from the default).
  uint32_t phaseOffset = 0;
  TickRate rate{};                 // tick rate vs the cycle (JD 2026-10-14, rate scaling):
                                   // divide gates pulls to cycles where
                                   // (cycle % divide) == (phase % divide) of the rate;
                                   // multiply repeats the pull within its phase slot.
                                   // Default neutral (1/1) = pre-scaling behaviour exactly.
                                   // GENERALIZES phaseDivider above inside this same
                                   // descriptor and the same dueFor gate — phaseDivider
                                   // is the divider-only, sub-cycle special case (it
                                   // subdivides phases; it cannot express a multiplier
                                   // or a cycle-axis divide); TickRate adds the cycle
                                   // axis and the multiply direction. NOT a parallel
                                   // system: one descriptor, one gate (see tick_rate.h).
                                   // VISIBILITY (A-H1 corrected contract, tick_rate.h):
                                   // the authority stages NOTHING — a WorkFn writing
                                   // shared state publishes immediately, including to
                                   // same-phase items on other shards. Final-only
                                   // visibility is the WorkFn's obligation: stage
                                   // intermediates privately, publish only on the
                                   // final sub-pull — knowable via currentSubPullIndex()
                                   // / currentSubPullCount() / isFinalSubPull() below.
  bool threadAffine = false;       // true = executes on the authority thread inside the phase window
  bool tickCritical = true;        // false = excluded from barrier & watchdog
  uint32_t group = 0;              // COMPONENT side: 0 = participates in every runPhase group;
                                   // otherwise only in runPhase calls with the same group
                                   // (or kAllGroups). Caller side: runPhase group 0 fires
                                   // ONLY group-0 components; kAllGroups fires all (A-M1).
};

struct HsComponentStats {
  std::string name;  // registered component name, owned by the authority (never a
                     // dangling caller pointer); "unnamed-<id>" when the
                     // registration name was empty/null. Offender diagnostics
                     // (log + stats) always carry this name.
  uint64_t fires = 0, overruns = 0, maxWorkNs = 0, maxOverrunNs = 0;
  bool repeatOffender = false;
};

// Per-group phase diagnostics (review A-M2): groups run at different natural
// periods, so their phase counts/overruns are kept apart. Index = runPhase
// group for groups 0..2; any other group folds into slot 0 (production uses
// only 1 = Signal, 2 = clock domains, 0 = bare/default).
struct HsGroupStats {
  uint64_t phases = 0, overrunPhases = 0, overrunNsTotal = 0;
};

struct HsStats {
  // qpcViolations (review A-L8, relabelled): both stamps come from the SAME
  // shared steady clock (nowNs()), and the pull stamp is necessarily read
  // after the open stamp was stored — so a count here means the clock
  // itself went backwards (broken/non-monotonic steady_clock), NOT that
  // "two synchronized QPC calls" disagreed. It is a clock-monotonicity
  // tripwire, kept because a non-monotonic clock would invalidate every
  // duration the watchdog and the overrun accounting compute.
  uint64_t phases = 0, overrunPhases = 0, overrunNsTotal = 0, qpcViolations = 0, barrierWaitNsTotal = 0;
  std::array<HsGroupStats, 3> perGroup{};      // split of the totals above, by group
  std::vector<HsComponentStats> perComponent;  // by registration id
};

struct HsFreezeInfo {
  uint32_t componentId = 0;
  const char* componentName = "";
  uint64_t phase = 0, cycleTick = 0;
  uint64_t openStampNs = 0, tripStampNs = 0;  // both from the shared steady clock
  uint64_t elapsedNs = 0, ceilingNs = 0;
};

class TimingHandshakeAuthority {
 public:
  struct Config {
    int phasesPerCycle = 4;                    // tunable; 1..64, power-of-two not required
    int workerThreads = 0;                     // 0 = auto: min(4, hardware_concurrency - 1), floor 1
    std::chrono::nanoseconds phaseBudget{0};  // nominal phase window; 0 = overrun accounting off.
                                              // Fallback for group 0 and any group without an override below.
    // Per-group phase windows (review A-M2): each group is budgeted from
    // its OWN period instead of one double-divided master-rate figure.
    // Group 1 (Signal): phasesPerCycle phases run per world tick, so
    // SimThread sets this to (world-tick period / phasesPerCycle).
    // Group 2 (clock domains): one phase runs per master step, so
    // SimThread sets this to the master-step period (no division).
    // 0 = fall back to phaseBudget for that group.
    std::chrono::nanoseconds phaseBudgetGroup1{0};
    std::chrono::nanoseconds phaseBudgetGroup2{0};
    std::chrono::nanoseconds watchdogCeiling{0};  // 0 = derive: 250 ms (D3)
    uint32_t offenderThreshold = 3;            // overruns before the repeat-offender flag
    bool pureSpin = false;                     // D9: true = prototype's pure spin poll
    // One commit hook, run on the authority thread after each successful
    // barrier, before the phase is folded into stats. Group is the runPhase
    // group so a hook can ignore phases that are not its own.
    std::function<void(uint64_t phase, uint64_t cycleTick, uint32_t group)> commitFn;
  };
  using WorkFn = std::function<void(uint64_t phase, uint64_t cycleTick)>;
  using FreezeFn = std::function<void(const HsFreezeInfo&)>;

  explicit TimingHandshakeAuthority(Config cfg);
  ~TimingHandshakeAuthority();  // calls stop()

  TimingHandshakeAuthority(const TimingHandshakeAuthority&) = delete;
  TimingHandshakeAuthority& operator=(const TimingHandshakeAuthority&) = delete;

  /// Pre-start only; returns the assigned id (registration order), or
  /// UINT32_MAX when called after start(). Shard assignment is static
  /// round-robin by registration order over the non-affine components.
  uint32_t registerComponent(HsComponentDesc desc, WorkFn fn);
  /// Components are LISTED by name in an unordered_map index built at
  /// registration (JD 2026-10-14, rate scaling) — lookup only: execution
  /// order is always registration id order, and the map's iteration order
  /// never reaches the schedule. Returns the registration id for `name`,
  /// or UINT32_MAX when no component was registered under that name.
  uint32_t componentIdByName(std::string_view name) const;
  size_t componentCount() const { return comps_.size(); }
  // Sub-pull context of the item executing on the CALLING thread
  // (A-H1 mechanism-lite, 2026-10-15; also closes A-I2 for this layer):
  // while a WorkFn runs inside runItem(), these report its 0-based
  // sub-pull index, the firing's sub-pull count (the component's rate
  // multiply), and whether this is the final sub-pull — the moment a
  // disciplined WorkFn publishes its staged state (see the crossing
  // contract in tick_rate.h). Outside any item (or on a thread running
  // another authority's item) they report the single-pull defaults:
  // index 0, count 1, final == true, so publish-on-final code behaves
  // correctly when a WorkFn is invoked directly in a unit test.
  uint32_t currentSubPullIndex() const;
  uint32_t currentSubPullCount() const;
  bool isFinalSubPull() const;
  void setFreezeCallback(FreezeFn fn) { freezeFn_ = std::move(fn); }  // pre-start only
  // Installs the commit hook. Review A-L9: overwriting an already-set hook
  // (e.g. one supplied via Config::commitFn) is NOT silent — it logs a
  // warning (and asserts in debug builds), because the previous hook's
  // owner would otherwise lose its per-phase commit with no signal.
  void setCommitFn(std::function<void(uint64_t, uint64_t, uint32_t)> fn);

  int workerCount() const { return workerCount_; }
  int phasesPerCycle() const { return cfg_.phasesPerCycle; }
  std::chrono::nanoseconds watchdogCeiling() const { return ceiling_; }

  // Spawns pull workers. Returns false (and spawns nothing) while a
  // previous stop() left workers outstanding (poisoned state, see stop()):
  // a still-running old worker must never share the world with a new pool.
  bool start();  // spawns pull workers
  // Signals workers out of the poll loop and joins them. NO-DETACH
  // protocol (P0a / review C2, supersedes hs-design D4's detach): a worker
  // still stuck inside a component after its per-worker bounded grace
  // (ceiling x 2, per worker, saturating) is NEVER detached — detaching
  // left it dereferencing the authority / SimThread / world after those
  // owners could be destroyed (use-after-free), and stop() is also the
  // save path, so a detached worker could mutate world state during a
  // supposedly consistent snapshot. Instead stop() returns false, the
  // worker stays joinable and owned by this authority, and the authority
  // is POISONED: workersOutstanding() is true, start() refuses, and the
  // owner (SimThread) refuses to destroy this authority (it deliberately
  // leaks it) while any worker is outstanding. stop() is retryable: once
  // the stuck component returns, a later stop() joins the worker and
  // clears the poison. Idempotent when it succeeds.
  bool stop();
  // True while a failed stop() left at least one worker unjoined.
  bool workersOutstanding() const { return workersOutstanding_.load(std::memory_order_acquire); }

  // --- authority side (the owning sim thread only) ---
  // Opens `phase` (stamps it), runs thread-affine due items inline, waits at
  // the barrier (WAIT: as long as the slowest shard takes), runs the commit
  // hook, returns true. Returns false iff the watchdog fired: lastFreeze()
  // names the slowest unfinished component, the frozen latch is set, and
  // further runPhase calls return false immediately until resetAfterFreeze()
  // or stop()/start().
  // `group` selects which grouped components fire: the same group, plus
  // group-0 components (which participate in every group). Group 0 is NOT
  // a wildcard on the caller side — it fires only group-0 components;
  // pass kAllGroups to fire every group (A-M1).
  bool runPhase(uint64_t phase, uint64_t cycleTick, uint32_t group = 0);
  // Invokes the registered freeze callback with lastFreeze(). Called by the
  // owner (SimThread) after runPhase returned false, per hs-design §1.7.
  void notifyFreeze() const { if (freezeFn_) freezeFn_(lastFreeze_); }
  bool frozen() const { return frozen_.load(std::memory_order_acquire); }
  const HsFreezeInfo& lastFreeze() const { return lastFreeze_; }
  // Clears the freeze latch AND bumps the publication generation (P0a /
  // review CH-H1). In-flight workers of the frozen phase revalidate the
  // generation before every item and again at the barrier arrival under
  // m_: a stale arrival is discarded, never counted toward the next
  // phase's barrier, and stale items are not executed to completion.
  void resetAfterFreeze();

  // --- pull side (worker threads; also the public poll query of the design) ---
  // One consistent, revalidated view of the currently published phase
  // (P0a / review CH-H2), filled by pollPhase(). `dueWork` is true when
  // this publication has due work for the polling shard that the shard
  // has not yet pushed (its done-seq differs from seq).
  struct PublishedPhase {
    uint64_t seq = 0, generation = 0, phase = 0, cycle = 0;
    uint32_t group = 0;
    uint64_t openStampNs = 0;
    bool dueWork = false;
  };
  // THE poll implementation (review A-M3): workerLoop polls with this,
  // and canTick() below is this same poll — there is no second, divergent
  // inline poll. Returns false when there is no publication newer than
  // `seenSeq` for the caller (or the tuple raced a publish/reset and must
  // be re-polled); on true, `out` holds the full snapshot + dueWork.
  bool pollPhase(uint32_t shard, uint64_t seenSeq, PublishedPhase& out) const;
  // True exactly when the currently published phase has due work for `shard`
  // that the shard has not yet pushed for this publication.
  bool canTick(uint32_t shard, uint64_t& phaseOut) const;

  // Thread contract (review A-M6): stats() returns the LIVE struct, which
  // runPhase mutates on the authority thread — call it only from the
  // authority thread, or while the authority is stopped. Any other thread
  // must use statsSnapshot(), which copies the stats out under statsM_
  // (runPhase's fold and resetStats hold the same mutex).
  const HsStats& stats() const { return stats_; }
  HsStats statsSnapshot() const;
  void resetStats();

  /// Monotonic nanoseconds from the shared steady clock (the "QPC" of the
  /// design: CLOCK_MONOTONIC on Linux, QPC-backed steady_clock on Windows).
  static uint64_t nowNs();

 private:
  struct Comp {
    HsComponentDesc desc;
    WorkFn fn;
    int shard = -1;  // -1 = affine / never pulled
    Comp() = default;
    Comp(Comp&& o) noexcept
        : desc(o.desc), fn(std::move(o.fn)), shard(o.shard),
          startStampNs(o.startStampNs.load(std::memory_order_relaxed)),
          durationNs(o.durationNs.load(std::memory_order_relaxed)),
          started(o.started.load(std::memory_order_relaxed)),
          finished(o.finished.load(std::memory_order_relaxed)),
          offenderLogged(o.offenderLogged) {}
    Comp& operator=(Comp&& o) noexcept {
      desc = o.desc; fn = std::move(o.fn); shard = o.shard;
      startStampNs.store(o.startStampNs.load(std::memory_order_relaxed));
      durationNs.store(o.durationNs.load(std::memory_order_relaxed));
      started.store(o.started.load(std::memory_order_relaxed));
      finished.store(o.finished.load(std::memory_order_relaxed));
      offenderLogged = o.offenderLogged;
      return *this;
    }
    Comp(const Comp&) = delete;
    Comp& operator=(const Comp&) = delete;
    // Per-phase scratch: written by the executing thread (worker or the
    // authority for affine items), read by the authority at/after the
    // barrier or during a watchdog scan. Atomics keep the watchdog scan
    // race-free while a worker is still running its item.
    std::atomic<uint64_t> startStampNs{0};
    std::atomic<uint64_t> durationNs{0};
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    bool offenderLogged = false;  // authority thread only
  };
  bool dueFor(const Comp& c, uint64_t phase, uint64_t cycle, uint32_t group) const;
  // Effective budgets for a runPhase group (A-M2 + sweep C2).
  // phaseBudgetForGroup: the group's own phase window (override or fallback).
  // cycleBudgetForGroup: the budget repeat-offender accounting is judged
  // against — phaseBudgetForGroup x phasesPerCycle, EXCEPT group 2, whose
  // cycle is a single phase (one phase per master step), so x1.
  std::chrono::nanoseconds phaseBudgetForGroup(uint32_t group) const;
  std::chrono::nanoseconds cycleBudgetForGroup(uint32_t group) const;
  void workerLoop(int shard);
  // Executes one component. Exception boundary (P0a / review C3): a throw
  // from the WorkFn is caught here, recorded via recordFailure(), and
  // reported by returning false — it never escapes a worker thread
  // (std::terminate with no report) and never escapes runPhase raw.
  bool runItem(Comp& c, uint64_t phase, uint64_t cycleTick);
  // First failure wins. Sets the pending-failure record + the freeze latch
  // (no further phase may commit); runPhase converts the record via
  // simFailFastFromException at the next phase boundary (before any
  // commit), mirroring SimThread's tickGuard.
  void recordFailure(const Comp& c, uint64_t phase, uint64_t cycleTick, const char* what);
  // If a worker failure is pending, convert it now (noreturn in practice:
  // simFailFastFromException aborts after logging the report).
  void convertPendingFailureIfAny();

  // One consistent publication snapshot (P0a / review CH-H2): seq +
  // generation + phase/cycle/group + open stamp are read, then seq and
  // generation are re-read; the snapshot is accepted only if both are
  // unchanged, so a worker can never execute one phase's tuple under
  // another phase's sequence number. Fills the public PublishedPhase
  // (its dueWork field is pollPhase's job, left false here).
  bool readPublished(PublishedPhase& out) const;

  Config cfg_;
  std::chrono::nanoseconds ceiling_{0};
  int workerCount_ = 1;
  std::vector<Comp> comps_;
  // Owned copies of the registered component names (deque: element
  // addresses are stable across push_back). Each Comp's desc.name and each
  // HsComponentStats.name-equivalent pointer refers here, NEVER to caller
  // storage — caller name strings are routinely SSO strings inside a
  // reallocating vector (the sweep §5 empty-name defect: 'signal.mark' /
  // 'signal.prep' flagged as '').
  std::deque<std::string> ownedNames_;
  // Name -> registration id index (JD 2026-10-14, rate scaling): components
  // are listed by name here for lookup only (see componentIdByName); the
  // first registration of a name wins. Execution order is always
  // registration id order — this map is never iterated for scheduling.
  std::unordered_map<std::string, uint32_t> compIndex_;
  // Per-cycle own-work accumulators for offender accounting (authority
  // thread only, parallel to comps_ by registration id). A component's
  // window is keyed by (group, cycleTick); entering a new window discards
  // the old one (charging is eager — see runPhase — so nothing is owed).
  struct CycleAccum {
    uint32_t group = 0;
    uint64_t cycleTick = 0;
    uint64_t workNs = 0;
    bool active = false;
  };
  std::vector<CycleAccum> cycleAccum_;
  std::vector<std::vector<uint32_t>> shards_;  // shard -> component ids (ascending)
  FreezeFn freezeFn_;
  HsStats stats_;
  // Guards stats_ for cross-thread snapshots (A-M6): held by runPhase's
  // stats fold, resetStats(), and statsSnapshot(). The authority thread's
  // own direct stats() reads need no lock (same thread as the fold).
  mutable std::mutex statsM_;
  HsFreezeInfo lastFreeze_{};

  std::vector<std::thread> workers_;
  std::deque<std::atomic<bool>> workerExited_;
  bool startedOnce_ = false;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::atomic<bool> frozen_{false};

  // Published phase tuple. publishSeq_ is an internal monotonic publication
  // counter (phase numbers supplied by different callers may repeat or go
  // backwards across groups; the sequence is what workers track).
  std::atomic<uint64_t> publishSeq_{0};
  // Publication generation: bumped by start() and resetAfterFreeze().
  // Workers capture it with the snapshot and revalidate it before every
  // item and at the barrier arrival (under m_); a mismatch means the
  // publication they were executing is stale and must neither finish its
  // item list nor count toward any barrier.
  std::atomic<uint64_t> generation_{0};
  std::atomic<uint64_t> publishedPhase_{0};
  std::atomic<uint64_t> publishedCycle_{0};
  std::atomic<uint32_t> publishedGroup_{0};
  std::atomic<uint64_t> phaseOpenStamp_{0};
  std::atomic<uint64_t> committedPhase_{0};
  std::atomic<uint32_t> arrivals_{0};
  std::deque<std::atomic<uint64_t>> shardDoneSeq_;
  std::atomic<uint64_t> qpcViolations_{0};
  std::atomic<bool> workersOutstanding_{false};  // C2 poison flag (see stop())

  // Pending worker failure (C3): first recorded failure, converted by
  // runPhase at the next phase boundary.
  struct PendingFailure {
    bool present = false;
    std::string message;  // exception what() (or a non-std marker)
    std::string componentName;
    uint32_t componentId = 0;
    uint64_t phase = 0, cycleTick = 0;
  };
  std::mutex failureM_;
  PendingFailure pendingFailure_;

  mutable std::mutex m_;
  std::condition_variable publishCv_;  // workers block here (hybrid poll)
  std::condition_variable barrierCv_;  // authority waits here for arrivals
};

}  // namespace litholattice
