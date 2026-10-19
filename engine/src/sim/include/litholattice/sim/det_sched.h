// Deterministic parallelism scheduler (gkz pattern): systems declare Read/Write
// resource access; the scheduler builds a conflict-DAG into greedy stages;
// systems in one stage run on a thread pool because they provably write disjoint
// data, stages run in order. Structural changes (spawn/despawn, component
// add/remove) go through per-system command buffers drained in (system, seq)
// order between stages -- the world is never mutated mid-stage. RNG is keyed
// per-entity (never a shared global stream), so threaded runs hash
// bit-identical to single-threaded runs (verify with hash-per-tick golden traces).
//
// This is a scheduler/library first: it schedules existing sim pieces, it does
// not rewrite the sim. A compiled Trace graph (ITraceJit::execute) can be wrapped
// as a SimSystem that declares Write access on the signal-network resource; the
// scheduler then parallelizes it against disjoint systems without touching the
// JIT itself.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "litholattice/core/job_system.h"
#include "litholattice/core/threading.h"

namespace litholattice {

// ---- deterministic RNG (per-entity keyed; thread-count independent) --------
// splitmix64: well-defined on uint64_t arithmetic, so identical on every
// platform and independent of which thread draws the value.
uint64_t detMix64(uint64_t x);
/// Stable seed for (entity, tick): draw detRand32(detKey(...), ...) per entity.
/// Never a shared stream: two entities never share state, so scheduling order
/// cannot change the values.
uint64_t detKey(uint64_t entity, uint64_t tick, uint64_t salt);
inline uint32_t detRand32(uint64_t entity, uint64_t tick, uint64_t salt) {
  return static_cast<uint32_t>(detKey(entity, tick, salt) >> 32);
}
/// Uniform double in [0, 1) from a keyed draw.
inline double detRand01(uint64_t entity, uint64_t tick, uint64_t salt) {
  return (detKey(entity, tick, salt) >> 11) * 0x1p-53;
}

// ---- declared resource access ----------------------------------------------
// Resource ids are caller-allocated (an enum in game code, e.g. kResCells = 1).
// Two systems conflict on a resource unless both only Read it; conflicting
// systems land in different stages. Reads/writes inside one system must stay
// inside what it declared: the scheduler can only parallelize what is declared.
struct ResourceAccess {
  uint32_t resource = 0;  ///< caller-allocated resource id
  bool write = false;     ///< true = Write, false = Read
};
inline ResourceAccess readRes(uint32_t r) { return ResourceAccess{r, false}; }
inline ResourceAccess writeRes(uint32_t r) { return ResourceAccess{r, true}; }

// ---- structural commands -----------------------------------------------------
// Deferred structural changes: spawn/despawn, component add/remove. Pushed
// during a system's update(), applied by the scheduler between stages in
// (system, seq) order. The world is never mutated mid-stage.
struct StructuralCommand {
  enum class Kind : uint8_t { Spawn, Despawn, AddComponent, RemoveComponent, Custom };
  Kind kind = Kind::Custom;
  uint64_t entity = 0;   ///< target entity (Despawn/AddComponent/RemoveComponent); unused for Spawn
  uint64_t payload = 0;  ///< game-defined (component id, archetype, ...)
  /// Custom commands: applied directly with the world pointer. The typed kinds
  /// are interpreted by the scheduler's ApplyFn.
  std::function<void(void* world)> apply;
};

/// One system's private command buffer. Each system owns one; systems in a
/// stage run on different threads but never touch each other's buffer.
class CommandBuffer {
 public:
  void push(StructuralCommand cmd) { cmds_.push_back(std::move(cmd)); }
  size_t size() const { return cmds_.size(); }
  bool empty() const { return cmds_.empty(); }
  /// Moves the commands out in push (seq) order; the buffer is empty after.
  /// The scheduler drains buffers in system registration order.
  void drainInto(std::vector<StructuralCommand>& out) {
    out.insert(out.end(), std::make_move_iterator(cmds_.begin()), std::make_move_iterator(cmds_.end()));
    cmds_.clear();
  }

 private:
  std::vector<StructuralCommand> cmds_;
};

// ---- systems -----------------------------------------------------------------
struct SchedTickContext {
  uint64_t tick = 0;          ///< world tick index
  void* world = nullptr;      ///< caller's state; touch only declared resources
  CommandBuffer& commands;    ///< this system's private structural command buffer
  /// Keyed RNG draw for `entity` this tick (thread-count independent).
  uint32_t rng32(uint64_t entity, uint64_t salt = 0) const { return detRand32(entity, tick, salt); }
  double rng01(uint64_t entity, uint64_t salt = 0) const { return detRand01(entity, tick, salt); }
};

class SimSystem {
 public:
  virtual ~SimSystem() = default;
  virtual const char* name() const = 0;
  /// Declared Read/Write resource access. The conflict-DAG is built from this.
  virtual std::vector<ResourceAccess> access() const = 0;
  /// One tick's work. Runs on a pool worker; must not touch other systems'
  /// state, must not mutate the world structurally (use ctx.commands), and
  /// must iterate in a deterministic order.
  virtual void update(SchedTickContext& ctx) = 0;
};

// ---- the scheduler -------------------------------------------------------------
class DeterministicScheduler {
 public:
  /// How a typed structural command is applied to the world. Always called
  /// single-threaded, between stages, in (system, seq) order (`systemId` is the
  /// registration order). Custom-kind commands with `apply` set bypass this
  /// and use their own callback.
  using ApplyFn = std::function<void(void* world, size_t systemId, const StructuralCommand& cmd)>;

  explicit DeterministicScheduler(ApplyFn apply) : apply_(std::move(apply)) {}

  /// Registers a system; returns its system id (registration order). Rebuilds
  /// the stage plan. Ids are stable: never reused or reordered.
  size_t addSystem(std::unique_ptr<SimSystem> sys);

  size_t systemCount() const { return systems_.size(); }
  /// Greedy conflict-DAG stages: stage[s] holds system ids; systems in one
  /// stage provably write disjoint resources and run in parallel. Deterministic
  /// for a fixed registration order.
  const std::vector<std::vector<size_t>>& stages() const { return stages_; }

  /// One tick: for each stage in order, submit each system as one job on
  /// `pool`, wait for the stage, then drain command buffers in (system, seq)
  /// order (commands become visible to the next stage). Throws
  /// std::runtime_error if a system job threw (the world may be half-updated;
  /// the caller must reset). The pool must be quiesced for the call and have
  /// at least one worker.
  void runTick(uint64_t tick, void* world, JobPool& pool);
  /// One tick on a work-stealing JobSystem: each stage's systems are submitted as
  /// Normal-priority jobs behind a JobFence, and the fence (not the pool) is the
  /// stage barrier — stage order, command-buffer drains, and failure semantics are
  /// identical to the JobPool path. Stealing only changes WHERE a job runs, never
  /// the stage order, so determinism is preserved (scheduler declares ORDER, the
  /// job system provides WHERE). The system must be quiesced for the call.
  void runTick(uint64_t tick, void* world, JobSystem& jobs);
  /// Convenience: runTick for [startTick, startTick + count).
  void runTicks(uint64_t startTick, uint64_t count, void* world, JobPool& pool) {
    for (uint64_t i = 0; i < count; ++i) runTick(startTick + i, world, pool);
  }
  /// Convenience: runTick for [startTick, startTick + count) on a JobSystem.
  void runTicks(uint64_t startTick, uint64_t count, void* world, JobSystem& jobs) {
    for (uint64_t i = 0; i < count; ++i) runTick(startTick + i, world, jobs);
  }

  // ---- golden-trace hashing --------------------------------------------------
  // FNV-1a helpers for per-tick state checksums. Hash the same bytes in the
  // same order after N ticks: threaded and single-threaded runs must agree
  // bit-identically ("determinism survives threading" verification).
  static constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
  static constexpr uint64_t kFnvPrime = 1099511628211ULL;
  static uint64_t hashBytes(const void* p, size_t n, uint64_t h = kFnvOffset);
  static uint64_t hashU64(uint64_t v, uint64_t h = kFnvOffset) { return hashBytes(&v, sizeof(v), h); }

 private:
  void rebuildStages();
  void drainStage(void* world);

  ApplyFn apply_;
  std::vector<std::unique_ptr<SimSystem>> systems_;
  std::vector<std::unique_ptr<CommandBuffer>> buffers_;  // one per system
  std::vector<std::vector<size_t>> stages_;
  std::vector<StructuralCommand> drainTmp_;
};

}  // namespace litholattice
