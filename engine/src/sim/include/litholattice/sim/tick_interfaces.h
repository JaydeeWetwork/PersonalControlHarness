// Tick interfaces: small inheritable contracts for block behaviors.
// A block opts into what it needs; the scheduler calls only what the block
// implements. One shared behavior object per block type (flyweight); per-block
// memory stays game-side, looked up by position. Approved OOD 2026-10-10.
#pragma once

#include <cstdint>
#include <functional>

#include "litholattice/world/block.h"
#include "litholattice/world/chunk_map.h"

namespace litholattice {

/// Per-tick context handed to behaviors. `rng` is deterministic per position
/// and tick (splitmix64), matching the random-tick RNG.
/// (Not the scheduler's SchedTickContext in det_sched.h: that one is the
/// DeterministicScheduler's per-system context. The two were both named
/// TickContext — renamed to kill the latent ODR violation.)
struct TickContext {
  ChunkMap& world;
  int x = 0, y = 0, z = 0;
  uint64_t tick = 0;  ///< world tick index
  int tps = 32;       ///< this domain's rate
  uint64_t rng = 0;   ///< deterministic per position + tick
  std::function<void(int x, int y, int z)> dirty;  ///< mark a cell for remesh
};

/// Identity, properties, model binding. Data lives in BlockDef; this is the
/// behavior-side view a block type exposes to the scheduler.
class IBlock {
 public:
  virtual ~IBlock() = default;
};

/// Scheduled or change-driven logic.
class ITickable {
 public:
  virtual ~ITickable() = default;
  /// One tick for the block at ctx.(x,y,z). Reads inputs via ctx.world,
  /// writes a new CellState, reports visual changes through ctx.dirty.
  virtual void onTick(TickContext& ctx, CellState& state) = 0;
};

/// Random ticks (growth, decay, ...). Opt-in per block; the engine picks
/// cells per section per world tick and calls this for each.
class IRandomTickable {
 public:
  virtual ~IRandomTickable() = default;
  virtual void onRandomTick(TickContext& ctx, CellState& state) = 0;
};

/// Membership in a signal graph. The graph solver (game-side) talks to nodes
/// only through this interface; it never knows block names.
/// Covers both media: Trace (wired) and Signal (wireless).
class ISignalNode {
 public:
  virtual ~ISignalNode() = default;
  /// Strength this node emits toward `face` (0-3 horizontal, 4 up, 5 down),
  /// given its current state. 0 = nothing.
  virtual int output(CellState state, int face) const = 0;
  /// An input on `face` changed to `strength`. The node updates its state;
  /// the solver schedules downstream nodes.
  virtual void onInputChanged(TickContext& ctx, CellState& state, int face, int strength) = 0;
};

}  // namespace litholattice
