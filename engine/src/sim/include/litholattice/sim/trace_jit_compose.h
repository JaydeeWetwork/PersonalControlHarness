// Trace JIT composition seam (sim/): wraps a compiled Trace graph as a
// DeterministicScheduler SimSystem.
//
// ============================================================================
// DESIGN REASONING (for the builder — read before refining this file)
// ============================================================================
//
// WHY WRAPPER-AS-SIMSYSTEM (this shape)
// -------------------------------------
// The scheduler's only truth is the declared ResourceAccess set: systems
// declare what they touch, the conflict-DAG turns those declarations into
// stages, and stages are what make parallelism deterministic. A compiled Trace
// graph is semantically one writer of the signal network: every node in the
// graph can write signal state, so the graph as a whole must serialize
// against any other system that writes the signal resource, while being free
// to run in parallel with systems that touch disjoint resources. The wrapper
// expresses exactly that contract — `access()` returns Write on the
// signal-network resource id — and `update()` does nothing the scheduler
// doesn't already know about: it calls `jit_->execute(graph_)` and returns.
// The JIT itself is untouched: the seam is compositional, not invasive.
//
// ALTERNATIVES CONSIDERED AND REJECTED
// ------------------------------------
// 1. Raw function-pointer system that bypasses declarations (access() returns
//    empty, or declares Read while actually writing). REJECTED: the scheduler
//    would then schedule the JIT graph in parallel with a genuine signal
//    writer. Both would write the signal network on different threads with
//    no ordering — a data race the golden-trace hash would catch only after
//    the fact, if at all. Declarations are load-bearing; lying in them
//    silently voids the whole determinism argument.
// 2. The JIT owning its own scheduling (JIT submits its own jobs / ticks
//    itself outside DeterministicScheduler::runTick). REJECTED: two schedulers
//    means the JIT can run while a stage's command buffers haven't drained,
//    or while another writer's stage is mid-flight. Stage order, drain order,
//    and the "world is never mutated mid-stage" invariant all assume ONE
//    scheduler sees every system. A self-scheduling JIT is also invisible to
//    the stage plan, so the builder could never reason about when Trace
//    evaluation happens relative to the rest of the sim.
// 3. Per-node SimSystems (one system per Trace node, letting the scheduler
//    order nodes). REJECTED: N nodes become N systems, and the conflict-DAG
//    rebuild is not free — addSystem rebuilds the stage plan, and every node
//    would declare Write on the signal resource, forcing N serial stages.
//    That is strictly worse than node-by-node evaluation: it serializes what
//    the JIT exists to fuse, and it makes the stage plan unreadable. The
//    entire point of compiling the graph is that intra-graph ordering is the
//    JIT's business (topological sort today, bytecode tomorrow), not the
//    scheduler's. The seam sits at graph granularity deliberately.
// 4. Inheriting from ITraceJit (a SimSystem that IS-A JIT). REJECTED: it
//    couples compile policy to scheduling. The builder must be able to swap
//    BasicTraceJit for the real bytecode JIT without touching scheduler-facing
//    code; has-a keeps the two axes independent. The wrapper owns graph
//    lifetime and scheduling visibility; the JIT owns compilation and
//    execution mechanics. Each side can be replaced without the other moving.
//
// OWNERSHIP DECISION
// ------------------
// The wrapper BORROWS the ITraceJit (raw pointer, must outlive the system)
// and OWNS its CompiledGraph (by value; freed in the destructor and on every
// recompile). Rationale: a JIT implementation is compile machinery —
// stateless enough to share, expensive enough not to duplicate. Multiple
// TraceJitSystems (one per lithographized chip) sharing one JIT is the
// intended deployment, and giving each system its own JIT would fork the
// graph-id sequence and any future compile cache. The graph, by contrast, is
// per-system state: exactly one system executes it, so exactly one system
// owns its lifetime. Copy/move are deleted so a graph can never be aliased
// across two systems (which would double-free on destruction or, worse,
// execute a freed graph on a worker thread).
//
// DETERMINISM CONTRACT
// --------------------
// - execute() must be tick-pure: same graph + same signal-network state ->
//   same result, every tick, on every thread count. No shared RNG, no
//   wall-clock, no std::rand, no iteration order that depends on addresses
//   (e.g. std::unordered_map over node pointers) anywhere in the execute
//   path. If the graph needs randomness, it must be keyed per node/tick like
//   det_sched.h's detRand32 — and note the current seam does NOT pass the
//   tick into execute() (see "when real bytecode lands" below).
// - The wrapper introduces no ordering dependence: it reads no sibling
//   system state, writes no shared state of its own (ticksExecuted_ is
//   written only by this system's own update), and never caches ctx.world
//   across ticks (the world pointer is only valid during update anyway).
// - update() is deliberately boring. Any cleverness added here (caching,
//   conditional execution, cross-system peeks) is a determinism smell.
//
// RECOMPILATION CONTRACT (read this twice)
// ----------------------------------------
// recompile() frees the old graph and compiles a new one. It is ONLY safe
// to call BETWEEN ticks — never from inside update(), never from a worker
// thread while runTick is in flight, never from a command-buffer drain.
// Reason: update() may be executing the graph on a pool worker RIGHT NOW;
// swapping the graph under it is use-after-free on another thread, and no
// lock can fix it without serializing the stage the wrapper sits in.
// The scheduler only rebuilds stages in addSystem(), and recompile() does
// not change access() (the resource id is fixed at construction), so no
// stage rebuild is needed — but that is exactly why the resource id must
// stay fixed: changing it post-registration would silently desync the
// stage plan.
//
// WHAT BREAKS IF YOU REFINE THIS CARELESSLY
// -----------------------------------------
// - Mid-tick swap (recompile from inside update() or from another system's
//   command apply): worker-thread use-after-free on graph_.bytecode.
// - Freeing via a DIFFERENT ITraceJit than the one that compiled: the
//   wrapper always frees through the borrowed jit_; if you ever allow the
//   JIT pointer to change, free the old graph with the OLD jit first.
// - ITraceJit::free is not guaranteed to sanitize: BasicTraceJit::free
//   clears bytecode but leaves nodeCount/id stale. freeGraph() zeroes those
//   fields AFTER calling free() so a stray execute() degrades to a no-op
//   instead of reading freed memory. Keep that sanitization if you touch
//   freeGraph().
// - Adding a second Write (or a Read on a contended resource) to access()
//   "because the graph also peeks at X": the stage plan is built from what
//   you declare. Under-declaring races; over-declaring serializes the sim
//   for no reason. If the graph's footprint grows, declare it — don't sneak
//   it past the scheduler.
// - Sharing ONE TraceJitSystem across two schedulers / two worlds: the
//   graph is single-owner, single-world state. One chip, one system, one
//   scheduler.
//
// EXACTLY WHAT TO WATCH WHEN REAL BYTECODE LANDS IN THE JIT
// ----------------------------------------------------------
// - v1 execute() ignores SchedTickContext because the flattened node pointers
//   carry their own state. A bytecode VM will need tick-scoped inputs
//   (notably the tick index, for any keyed randomness, and the signal
//   network base pointer). That means extending ITraceJit::execute's
//   signature or adding a bind step — either way, THIS wrapper is where the
//   SchedTickContext meets the graph, so the change lands here, not in the
//   scheduler.
// - Bytecode relocation: CompiledGraph is stored BY VALUE and assigned on
//   recompile. If the VM keeps interior pointers into bytecode, a vector
//   reallocation or a graph move invalidates them. Pin the bytecode or make
//   the VM position-independent; do not let execute() hold pointers across
//   a recompile.
// - The VM's hot loop must not iterate hash maps keyed by pointer, must not
//   call time(), must not touch thread-locals that differ per worker.
//   Verify with the golden-trace procedure from det_sched.h: same world,
//   1 vs 4 threads, hashes must be bit-identical, or the JIT broke the
//   contract this file documents.
// - compile() is currently single-threaded (between ticks). If the game
//   ever compiles on a worker, the JIT's id sequence and any compile cache
//   need their own synchronization — that is the JIT's problem, but the
//   symptom will show up here as duplicate graph ids.
// - kSignalNetworkRes is a CONVENTION, not a registry: every system that
//   reads or writes Trace signal state must declare this same id, or the
//   conflict-DAG under-declares and two writers run in parallel. When the
//   game wires up its real signal systems, grep for the id.
// ============================================================================

#pragma once

#include <cstdint>
#include <vector>

#include "litholattice/sim/det_sched.h"
#include "litholattice/sim/trace_jit.h"

namespace litholattice {

/// Caller-allocated resource id for the Trace signal network. Game code must
/// use this same id for EVERY system that reads or writes Trace signal
/// state, or the scheduler's conflict-DAG will under-declare and run two
/// signal writers in parallel. Kept clear of the engine_tests fixtures
/// (100/101/102/200/201) on purpose.
constexpr uint32_t kSignalNetworkRes = 300;

/// A compiled Trace graph wrapped as a DeterministicScheduler system.
///
/// Construction compiles the graph once (via the borrowed ITraceJit);
/// update() executes it once per tick; the destructor frees it. recompile()
/// swaps in a freshly compiled graph — legal only between ticks, never
/// mid-tick (see the design block above).
class TraceJitSystem : public SimSystem {
 public:
  /// @param jit      borrowed JIT; must outlive this system. Shared across
  ///                 systems on purpose (one JIT, many chips).
  /// @param nodes    graph to compile now (may be nullptr when count is 0).
  /// @param nodeCount number of nodes.
  /// @param signalResource resource id declared as Write in access().
  ///                 Defaults to kSignalNetworkRes; override only if the game
  ///                 has its own signal-resource numbering — and then use it
  ///                 consistently everywhere.
  /// @param name     system name for diagnostics; must outlive the system.
  TraceJitSystem(ITraceJit* jit, ISignalNode** nodes, uint32_t nodeCount,
                 uint32_t signalResource = kSignalNetworkRes,
                 const char* name = "trace_jit")
      : jit_(jit), signalResource_(signalResource), name_(name) {
    graph_ = jit_->compile(nodes, nodeCount);
  }

  ~TraceJitSystem() override { freeGraph(); }

  // Single-owner graph lifetime: never copy or move a TraceJitSystem.
  TraceJitSystem(const TraceJitSystem&) = delete;
  TraceJitSystem& operator=(const TraceJitSystem&) = delete;
  TraceJitSystem(TraceJitSystem&&) = delete;
  TraceJitSystem& operator=(TraceJitSystem&&) = delete;

  const char* name() const override { return name_; }

  /// Declares Write on the signal-network resource. This is the whole point
  /// of the wrapper: the scheduler serializes this system against every
  /// other signal writer and parallelizes it against disjoint systems.
  std::vector<ResourceAccess> access() const override {
    return {writeRes(signalResource_)};
  }

  /// One tick: execute the owned graph exactly once. v1 intentionally
  /// ignores ctx — the flattened node pointers carry their own state; see
  /// the design block for what changes when real bytecode lands.
  void update(SchedTickContext& /*ctx*/) override {
    jit_->execute(graph_);
    ++ticksExecuted_;
  }

  /// Swap in a freshly compiled graph. CONTRACT: call only BETWEEN ticks
  /// (single-threaded, never from inside update() or while runTick is in
  /// flight). The old graph is freed through the same JIT that compiled it.
  /// access() is unchanged, so the scheduler's stage plan stays valid.
  void recompile(ISignalNode** nodes, uint32_t nodeCount) {
    freeGraph();
    graph_ = jit_->compile(nodes, nodeCount);
  }

  const CompiledGraph& graph() const { return graph_; }
  uint32_t signalResource() const { return signalResource_; }
  /// Ticks this system has executed (its own private counter; not world state).
  uint64_t ticksExecuted() const { return ticksExecuted_; }

 private:
  /// Frees through the owning JIT, then sanitizes: free() is not guaranteed
  /// to zero the struct, and a stale nodeCount over cleared bytecode would
  /// turn a stray execute() into an out-of-bounds read.
  void freeGraph() {
    jit_->free(graph_);
    graph_.id = 0;
    graph_.nodeCount = 0;
    graph_.tickCost = 0;
  }

  ITraceJit* jit_;  ///< borrowed; must outlive this system
  CompiledGraph graph_;  ///< owned: compiled once, executed per tick, freed on recompile/destruction
  uint32_t signalResource_;
  const char* name_;
  uint64_t ticksExecuted_ = 0;
};

}  // namespace litholattice
