// The game's world simulation, as the engine's tick scheduler (SimThread) sees it. SimThread owns
// the dimension's ChunkMap on its own thread and calls these hooks; the game implements them
// (LithoKraft: the signal sim). Everything here runs on the sim thread only.
#pragma once
#include <string>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <vector>
#include "litholattice/world/chunk_map.h"

namespace litholattice {

class TimingHandshakeAuthority;

/// Something the simulation reports to the game thread (a sound cue, ...). `kind` and `value`
/// are game-defined; the cell position lets the game place it in the world.
struct SimEvent {
  uint16_t kind = 0;
  int value = 0;
  int x = 0, y = 0, z = 0;
};

/// A game-defined command from the game thread (e.g. a key press routed to a block).
struct SimCommand {
  uint16_t code = 0;
  int x = 0, y = 0, z = 0;
  int a = 0, b = 0;
};

/// Chunks around the player that tick; everything outside is frozen.
struct TickArea {
  int pcx = 0, pcz = 0, radius = 0;  ///< player chunk and radius in chunks
  bool contains(int x, int z) const { return std::abs((x >> 4) - pcx) <= radius && std::abs((z >> 4) - pcz) <= radius; }
};

class IWorldSimulation {
 public:
  using DirtyFn = std::function<void(int x, int y, int z)>;  ///< a cell's look changed (remesh it)
  virtual ~IWorldSimulation() = default;

  /// Indexes the freshly loaded world (called once, before the first tick).
  virtual void scan(const ChunkMap& world) = 0;
  /// Timing Handshake (hs-design §3.1): called once by SimThread
  /// after scan() when handshake mode is on, so the simulation can register
  /// its phase components with the authority. Default: no-op — simulations
  /// that do not opt in are never touched by the handshake, and in legacy
  /// mode this is never called at all.
  virtual void registerHandshake(TimingHandshakeAuthority& /*authority*/) {}
  /// May the player place `block` into the empty cell (x, y, z)? Default: yes.
  virtual bool canPlace(const ChunkMap& /*world*/, int /*x*/, int /*y*/, int /*z*/, BlockId /*block*/) const { return true; }
  /// After a neighbour lost its solid block: may the (non-attachable) block at x, y, z stay? false pops it.
  /// One debug line about the cell (look-at inspector): ids, raw state, network, component memory.
  virtual std::string inspect(const ChunkMap&, int, int, int) const { return {}; }
  virtual bool stillSupported(const ChunkMap& /*world*/, int /*x*/, int /*y*/, int /*z*/) const { return true; }
  /// Attachable placement rule: may `block` mount at (x, y, z) on its support at attach face
  /// `face` (stAttach numbering)? Default: any face.
  virtual bool canAttach(const ChunkMap& /*world*/, int /*x*/, int /*y*/, int /*z*/, BlockId /*block*/, int /*face*/) const { return true; }
  /// A player edit changed (x, y, z) to `now` (kAir for a break). The world already holds it.
  virtual void onBlockChanged(ChunkMap& world, int x, int y, int z, BlockId now) = 0;
  /// A block popped off (lost support, etc.): `was` is what it was. Default: nothing.
  /// The game uses this to spawn item drops.
  virtual void onBlockPopped(ChunkMap& /*world*/, int /*x*/, int /*y*/, int /*z*/, BlockId /*was*/) {}
  /// Right-click on an interactive block. Returns false if nothing happened.
  virtual bool use(ChunkMap& /*world*/, int /*x*/, int /*y*/, int /*z*/, const DirtyFn& /*dirty*/) { return false; }
  /// A game-defined command (SimThread::edit with EditCmd::Kind::Command).
  virtual void command(ChunkMap& /*world*/, const SimCommand& /*cmd*/, const DirtyFn& /*dirty*/) {}
  /// One world tick over `area`.
  virtual void tick(ChunkMap& world, const TickArea& area, const DirtyFn& dirty) = 0;
  /// Simulated elements (HUD statistic).
  /// Random-tick hook (growth, decay, ...), after tick() on every world tick; `tickIndex`
  /// seeds the game's own deterministic choice of cells. Cells it changes must go to `dirty`.
  virtual void randomTick(ChunkMap& /*world*/, const TickArea& /*area*/, uint64_t /*tickIndex*/, const DirtyFn& /*dirty*/) {}
  /// A random tick picked cell (x, y, z) of an opted-in block (kRandomTicks); `rnd` is a
  /// deterministic per-pick random value. Changed cells must go to `dirty` (remesh + save).
  virtual void onRandomTick(ChunkMap& /*world*/, int /*x*/, int /*y*/, int /*z*/, BlockId /*block*/, uint32_t /*rnd*/,
                            const DirtyFn& /*dirty*/) {}
  virtual size_t activeCount() const { return 0; }
  /// Moves the events produced since the last call into `out` (appends).
  virtual void takeEvents(std::vector<SimEvent>& /*out*/) {}
};

}  // namespace litholattice
