// Signal propagation granularity policy (sim/): the engine-side policy layer
// for signal propagation. Propagation itself lives game-side (Trace/Signal
// graphs over ISignalNode); this header decides only the GRAIN at which the
// solver slices one tick's propagation work, in a way that composes with the
// deterministic scheduler (det_sched.h).
//
// ============================================================================
// DESIGN REASONING (for the builder — read before refining this file)
// ============================================================================
//
// WHY A POLICY LAYER, AND WHY THESE THREE GRAINS
// ----------------------------------------------
// JD's directive is "we like options": per-chunk, per-network, and
// whole-world granularity as runtime-selectable options, not three separate
// classes the game must #ifdef. The engine therefore owns the *policy* — the
// enum, the config field, the partition contract, and the deterministic
// ordering rules — while the game owns the *solver*: it implements
// SignalPropagationPolicy::partitionsForTick() against its own world data
// and reads granularity() to decide which slicing to emit. The seam is
// deliberately thin: the engine cannot know what a "network" is (that is
// game-side connectivity), but it CAN enforce that whatever slicing the game
// picks is yielded in a canonical order, because ordering is where
// determinism lives or dies.
//
// THE NON-NEGOTIABLE: DETERMINISTIC PARTITION ORDER
// -------------------------------------------------
// partitionsForTick() must return the SAME partition list for the same world
// state on every run, regardless of thread count, allocation addresses, or
// insertion history. Concretely:
//   - PerChunk:   one partition per ticking chunk, sorted ascending by
//                 ChunkKey::make(cx, cz). Sorting the uint64 key is a total
//                 order (lexicographic on the bit patterns of cx, cz); it is
//                 canonical, NOT spatial — negative coords sort after
//                 positive ones in two's-complement. Do not "improve" this
//                 with Morton/Z-order unless you document the new canonical
//                 order here, because the golden-trace hash depends on it.
//   - PerNetwork: one partition per active signal network, sorted ascending
//                 by a canonical network id. The id must be STABLE across
//                 ticks and address-independent: derive it from world state
//                 (e.g. the lowest packed node position in the component) or
//                 from a monotonic counter assigned during a discovery pass
//                 that itself iterates nodes in sorted order. An id taken
//                 from a pointer, a hash-map bucket, or an insertion counter
//                 fed by unordered iteration will silently break cross-run
//                 determinism — the golden trace catches it only if the game
//                 hashes partition ids per tick, which it should.
//   - WholeWorld: exactly one partition carrying kWholeWorldPartitionId.
// For golden-trace verification the game should hash the emitted partition
// ids in order each tick (see det_sched.h's hashU64): 1 vs N threads must
// agree bit-identically, or the partitioner broke the contract.
//
// TRADEOFFS PER GRANULARITY (JD asked for the reasoning on record)
// ----------------------------------------------------------------
// PerChunk — cost per tick ~ O(ticking chunks).
//   Wins: propagation walks contiguous chunk memory (cache-friendly); cost
//   scales with the loaded/ticking set, not world size; determinism falls
//   out of the fixed chunk-key order with zero discovery cost; matches how
//   the sim already thinks (ChunkMap, per-chunk dirty tracking). Wins
//   outright when signal networks are chunk-local (the common Trace-wiring
//   case: contraptions, lithography machines, local redstone-like logic).
//   Loses: wires crossing chunk borders. A network spanning two chunks is
//   sliced in two, so the game must define boundary semantics — typically a
//   boundary-resolution pass after partition propagation, or partition-local
//   propagation followed by an ordered cross-boundary fixpoint. Either way
//   the game must be careful that the boundary pass itself iterates in
//   canonical order, or determinism is lost at the seam between chunks.
// PerNetwork — cost per tick ~ O(active networks + discovery).
//   Wins: follows connectivity, so there is no boundary problem at all — a
//   network is a network wherever its nodes sit. Wins for sparse large
//   networks: a few long Trace buses (or wireless links modeled as
//   networks) stretched across a big world cost O(networks), where PerChunk
//   would pay O(ticking chunks) to visit mostly-empty chunk partitions.
//   Loses: it needs connected-component discovery, and discovery has a cost
//   of its own (~O(active signal nodes)). When networks churn every tick
//   (blocks placed/broken, wireless channels retuned), discovery can
//   dominate. Also the discovery pass itself must be deterministic (sorted
//   node iteration, canonical id assignment — see above), which is more
//   machinery than PerChunk's free ordering.
// WholeWorld — cost per tick ~ O(world, or the ticking set).
//   Wins: simplest to reason about — one partition, no discovery, no
//   boundaries, no ordering machinery beyond the single token. Wins for
//   dense global signal where locality does not exist anyway: wireless
//   Signal (radio) is the textbook case — a transmittor's broadcast has no
//   chunk or network to belong to, so slicing by chunk or network buys
//   nothing and costs bookkeeping.
//   Loses: pays for the whole world every tick even when three lamps blink
//   in one chunk. On large worlds this is the worst option by far; it is
//   the right choice only when the signal is genuinely global.
//
// WHY PerChunk IS THE DEFAULT
// ---------------------------
// kDefaultPropagationGranularity = PerChunk. Rationale: (1) it matches the
// sim's existing unit of work and the ticking-chunk set the engine already
// maintains; (2) its cost scales with what is loaded, not with world size;
// (3) its canonical order is free — no discovery machinery that the builder
// must get right before the first correct tick; (4) the common case (local
// Trace wiring) is chunk-local, and the uncommon cases (global wireless,
// sparse long-haul networks) can opt into WholeWorld/PerNetwork explicitly.
// A default should be the option that is correct-and-fast for the most
// worlds with the least new machinery. That is PerChunk.
//
// HOW EACH GRANULARITY MEETS THE DETERMINISTIC SCHEDULER
// ------------------------------------------------------
// The scheduler only sees declared ResourceAccess sets (det_sched.h): two
// systems conflict unless both only Read a resource, and conflicting systems
// land in different stages. Finer propagation granularity = more precise
// conflict declarations = more parallelism; coarser = simpler declarations
// but more serialization. Two deployment patterns:
//   Pattern A (recommended start): ONE propagation SimSystem declaring
//   writeRes(kSignalNetworkRes) — the same convention as TraceJitSystem in
//   trace_jit_compose.h — that internally iterates the tick's partitions in
//   canonical order. Simple, one declaration, serializes against every other
//   signal writer exactly as it should. The granularity then only changes the
//   *internal* work slicing (cache behavior, boundary passes), never the
//   stage plan. Start here.
//   Pattern B (max parallelism, earn it with profiling): one SimSystem per
//   partition, each declaring writeRes(partitionResourceId(partition)) —
//   disjoint partitions land in parallel stages. PerChunk partitions over
//   disjoint chunks propagate concurrently; per-network partitions over
//   disjoint networks likewise. Costs: the system set churns every tick, so
//   the scheduler rebuilds stages every tick; and "provably disjoint" is
//   the game's burden — PerChunk needs no cross-chunk wires (or an explicit
//   boundary protocol that runs in a later stage), PerNetwork needs
//   node-disjoint networks. Move to B only when profiling shows propagation
//   is the bottleneck AND the disjointness invariant is proven, not hoped.
// partitionResourceId() folds a partition key into a 32-bit resource id
// under a caller-allocated base. Note the fold can COLLIDE: two distinct
// keys may map to one id. That is safe by construction — a collision merges
// two conflict sets, which only over-serializes (costs parallelism, never
// correctness). If the game needs provable partition disjointness in the
// stage plan, it should allocate stable ids from a game-side registry keyed
// by SignalPartition::id and pass those to the scheduler instead of the fold.
//
// RUNTIME SWITCHING
// -----------------
// granularity is a plain field with a setter — no #ifdef, no subclass swap.
// Switching is legal BETWEEN ticks (the policy holds no in-flight state);
// partitionsForTick() reads the field on every call. For Pattern B, a switch
// changes the declared access set, so the scheduler's stage plan must be
// rebuilt between ticks — the same between-ticks-only rule as
// TraceJitSystem::recompile(). There is no such thing as switching mid-tick:
// the tick's partitions are already emitted by then.
//
// WHAT THE BUILDER OWNS FROM HERE
// -------------------------------
// - The game-side SignalPropagationPolicy subclass: chunk enumeration from
//   the ticking set (PerChunk), connected-component discovery with
//   canonical ids (PerNetwork), or the single token (WholeWorld).
// - Boundary semantics for PerChunk cross-chunk wires (document the chosen
//   protocol next to the implementation; the fixpoint pass must itself be
//   canonically ordered).
// - Hashing partition ids into the per-tick golden trace so 1-thread and
//   N-thread runs are comparable.
// - If randomness is ever needed inside discovery, key it per partition id
//   and tick (det_sched.h's detRand32) — never a shared stream, never
//   std::rand, never wall-clock.
// ============================================================================

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "litholattice/sim/det_sched.h"
#include "litholattice/sim/trace_jit_compose.h"  // kSignalNetworkRes convention
#include "litholattice/world/chunk.h"           // ChunkKey

namespace litholattice {

/// Tick-slicing grain for signal propagation. Runtime-selectable via
/// SignalPropagationPolicy::setGranularity() — never #ifdef'd.
enum class PropagationGranularity : uint8_t {
  PerChunk,    ///< one partition per ticking chunk; cost ~ O(ticking chunks)
  PerNetwork,  ///< one partition per active signal network; cost ~ O(active networks + discovery)
  WholeWorld,  ///< one partition for the whole world; cost ~ O(world) every tick
};

/// Default grain: PerChunk (reasoning in the design block above).
constexpr PropagationGranularity kDefaultPropagationGranularity = PropagationGranularity::PerChunk;

/// The single whole-world token's partition id. Distinct from every chunk key
/// by construction (all-bits-set is not a ChunkKey::make output for any
/// realistic coordinate... note: cx = cz = -1 WOULD produce it. The game must
/// not emit a PerChunk partition for chunk (-1, -1) with kind WholeWorld —
/// kinds are never mixed in one partitionsForTick() call, so this is only a
/// cross-kind sentinel, enforced by partitionsAreSorted()).
constexpr uint64_t kWholeWorldPartitionId = ~uint64_t(0);

/// One unit of propagation work for a tick. `id` is the canonical key:
/// ChunkKey::make(cx, cz) for PerChunk, a canonical network id for
/// PerNetwork, kWholeWorldPartitionId for WholeWorld. A single
/// partitionsForTick() call must emit exactly one kind, in non-decreasing
/// id order (checked by partitionsAreSorted()).
struct SignalPartition {
  PropagationGranularity kind = PropagationGranularity::WholeWorld;
  uint64_t id = 0;
};

/// Builds a PerChunk partition id from chunk coordinates (canonical key).
inline uint64_t chunkPartitionId(int cx, int cz) { return ChunkKey::make(cx, cz); }
/// Inverse of chunkPartitionId.
inline void unpackChunkPartitionId(uint64_t id, int& cx, int& cz) {
  cx = static_cast<int32_t>(static_cast<uint32_t>(id >> 32));
  cz = static_cast<int32_t>(static_cast<uint32_t>(id & 0xFFFFFFFFu));
}

/// Sorts partitions into canonical order (non-decreasing id). Call after
/// filling, before handing to the solver — determinism must never depend on
/// the game's discovery iteration order.
inline void sortPartitionsById(std::vector<SignalPartition>& parts) {
  std::sort(parts.begin(), parts.end(),
            [](const SignalPartition& a, const SignalPartition& b) { return a.id < b.id; });
}

/// Verifier for golden traces: one kind only, ids in non-decreasing order.
/// Empty is valid (a tick with no propagation work).
inline bool partitionsAreSorted(const std::vector<SignalPartition>& parts) {
  for (size_t i = 1; i < parts.size(); ++i) {
    if (parts[i].kind != parts[0].kind) return false;
    if (parts[i].id < parts[i - 1].id) return false;
  }
  return true;
}

/// Maps one partition to the scheduler's resource vocabulary under a
/// caller-allocated base (default: the kSignalNetworkRes convention from
/// trace_jit_compose.h). WholeWorld -> the base id itself. Finer grains ->
/// the base XOR-folded with the partition key (never equal to the base, so a
/// per-partition writer always conflicts with a whole-world writer on that
/// base — the safe direction). COLLISIONS ARE POSSIBLE between distinct
/// keys: they only over-serialize, never race. Games needing provable
/// disjointness should allocate stable ids from a game-side registry keyed
/// by SignalPartition::id instead of using this fold.
inline uint32_t partitionResourceId(const SignalPartition& p, uint32_t base = kSignalNetworkRes) {
  if (p.kind == PropagationGranularity::WholeWorld) return base;
  const uint32_t folded =
      static_cast<uint32_t>(p.id ^ (p.id >> 32)) | 0x1u;  // nonzero: never folds back to `base`
  return base ^ folded;
}

/// Convenience: the declared Write access for one partition (Pattern B).
inline ResourceAccess partitionAccess(const SignalPartition& p, uint32_t base = kSignalNetworkRes) {
  return writeRes(partitionResourceId(p, base));
}

/// Engine-side policy layer for signal propagation granularity. The game
/// implements partitionsForTick() against its own world data; the engine
/// owns the selectable granularity and the ordering contract.
///
/// Usage: the game subclasses this, fills partitionsForTick() from the
/// ticking-chunk set / its network discovery / the single token according to
/// granularity(), and yields them in canonical order. The solver (game-side)
/// then propagates partition by partition in the emitted order. For
/// scheduler integration see the design block (Pattern A vs Pattern B).
class SignalPropagationPolicy {
 public:
  virtual ~SignalPropagationPolicy() = default;

  /// Runtime selection of the propagation grain. Legal to call between
  /// ticks; the next partitionsForTick() call picks it up. (For Pattern B
  /// scheduler integration, changing granularity changes declared accesses,
  /// so rebuild the scheduler's stage plan between ticks too.)
  void setGranularity(PropagationGranularity g) { granularity_ = g; }
  PropagationGranularity granularity() const { return granularity_; }

  /// Yield this tick's propagation work partitions. CONTRACT:
  /// - exactly one SignalPartition::kind per call (the current granularity),
  /// - non-decreasing id order (use sortPartitionsById(); verify with
  ///   partitionsAreSorted()),
  /// - pure: same world state -> same list, every run, every thread count.
  ///   No wall-clock, no std::rand, no iteration over address-keyed maps.
  ///   If discovery needs randomness, key it per partition id and tick via
  ///   det_sched.h's detRand32.
  /// `out` is cleared before filling. `tick` is the world tick index,
  /// available for keyed RNG; partition shape itself is world-state-derived.
  virtual void partitionsForTick(uint64_t tick, std::vector<SignalPartition>& out) = 0;

 private:
  PropagationGranularity granularity_ = kDefaultPropagationGranularity;
};

/// Reference implementation (and test double): yields partitions from
/// caller-supplied chunk keys / network ids, sorted into canonical order
/// regardless of input order. Shows the intended subclass shape; the real
/// game implementation fills these from the world instead of vectors.
class VectorPropagationPolicy : public SignalPropagationPolicy {
 public:
  /// Chunk keys as produced by chunkPartitionId() (any order; sorted on emit).
  void setChunks(std::vector<uint64_t> keys) { chunks_ = std::move(keys); }
  /// Canonical network ids (any order; sorted on emit).
  void setNetworks(std::vector<uint64_t> ids) { networks_ = std::move(ids); }

  void partitionsForTick(uint64_t /*tick*/, std::vector<SignalPartition>& out) override {
    out.clear();
    switch (granularity()) {
      case PropagationGranularity::PerChunk:
        out.reserve(chunks_.size());
        for (uint64_t k : chunks_) out.push_back(SignalPartition{PropagationGranularity::PerChunk, k});
        break;
      case PropagationGranularity::PerNetwork:
        out.reserve(networks_.size());
        for (uint64_t id : networks_)
          out.push_back(SignalPartition{PropagationGranularity::PerNetwork, id});
        break;
      case PropagationGranularity::WholeWorld:
        out.push_back(SignalPartition{PropagationGranularity::WholeWorld, kWholeWorldPartitionId});
        return;  // single token is trivially sorted
    }
    sortPartitionsById(out);
  }

 private:
  std::vector<uint64_t> chunks_;
  std::vector<uint64_t> networks_;
};

}  // namespace litholattice
