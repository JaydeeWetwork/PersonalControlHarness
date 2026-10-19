// LithoKraft's world simulation for the engine's SimThread: the signal sim plus the game's
// placement rules and sound cues (place / break / click), behind litholattice::IWorldSimulation.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "lithokraft/content/signal.h"
#include "lithokraft/content/silica.h"
#include "litholattice/sim/timing_handshake.h"
#include "litholattice/sim/world_simulation.h"

namespace lithokraft {

/// SimCommand codes LithoKraft sends through SimThread::edit (EditCmd::Kind::Command).
enum SimCommandCode : uint16_t {
  kCmdAltUse = 2,    ///< sneak + use on the component at (x, y, z)
  kCmdKeyPulse = 1,  ///< keyboard block at (x, y, z): a = front (SIXBIT code + 1), b = control code
  kCmdItemUse = 3,   ///< per-item use dispatch (brief B1a / A3): a = itemId, (x, y, z) = target cell;
                     ///< routed to content/item_use.h's registry on the sim thread
};

/// SimEvent kinds above the SoundCue range (SoundCue is uint8_t).
constexpr uint16_t kEvItemDrop = 0x0100;  ///< value = BlockId dropped; the game spawns an item entity

class SignalWorldSimulation final : public IWorldSimulation {
 public:
  void scan(const ChunkMap& world) override { signal_.scan(world); }
  bool canPlace(const ChunkMap& world, int x, int y, int z, BlockId block) const override;
  bool canAttach(const ChunkMap& world, int x, int y, int z, BlockId block, int face) const override;
  bool stillSupported(const ChunkMap& world, int x, int y, int z) const override;
  std::string inspect(const ChunkMap& w, int x, int y, int z) const override { return signal_.inspect(w, x, y, z); }
  void onBlockChanged(ChunkMap& world, int x, int y, int z, BlockId now) override;
  void onBlockPopped(ChunkMap& world, int x, int y, int z, BlockId was) override;
  /// Random-tick dispatch: silica grass growth/spread (workstream 5). The engine
  /// only calls this for kRandomTicks cells (8/section/tick); the silica logic
  /// itself never runs on the per-tick path.
  void onRandomTick(ChunkMap& world, int x, int y, int z, BlockId block, uint32_t rnd,
                    const DirtyFn& dirty) override;
  bool use(ChunkMap& world, int x, int y, int z, const DirtyFn& dirty) override;
  void command(ChunkMap& world, const SimCommand& cmd, const DirtyFn& dirty) override;
  void registerHandshake(litholattice::TimingHandshakeAuthority& authority) override;
  void tick(ChunkMap& world, const TickArea& area, const DirtyFn& dirty) override {
    if (!hsAuthority_) { signal_.tick(world, area.pcx, area.pcz, area.radius, dirty); return; }
    tickHandshake(world, area, dirty);
  }
  /// True when the last handshake tick was frozen by the watchdog mid-cycle.
  bool handshakeFrozenCycle() const { return hsFrozenCycle_; }
  size_t activeCount() const override { return signal_.componentCount(); }
  void takeEvents(std::vector<SimEvent>& out) override;

 SignalSim& signal() { return signal_; }  ///< only while the SimThread is stopped (save / load)

 private:
  void tickHandshake(ChunkMap& world, const TickArea& area, const DirtyFn& dirty);
  void onHsCommit(uint64_t phase, uint64_t cycleTick, uint32_t group);
  SignalSim signal_;
  // ---- Timing Handshake state (null authority = legacy path) ----
  litholattice::TimingHandshakeAuthority* hsAuthority_ = nullptr;
  ChunkMap* hsWorld_ = nullptr;   // valid only inside tickHandshake
  TickArea hsArea_{};
  DirtyFn hsDirty_;               // the real dirty fn of the in-flight tick
  bool hsFrozenCycle_ = false;
  bool hsLegacyFallbackLogged_ = false;
  std::vector<std::string> hsNames_;                    // component names (desc.name points here)
  struct HsDirtyPos { int x, y, z; };
  std::vector<std::vector<HsDirtyPos>> hsLampDirty_;   // per-shard collected dirties
  std::vector<std::vector<SignalSim::LampHoldUpdate>> hsLampHolds_;  // per-shard deferred hold merges
  std::vector<SoundEvent> sounds_;  // edit cues, in order; tick sounds are appended at takeEvents()
  std::vector<SimEvent> dropEvents_;  // item drops from popped blocks (drained at takeEvents())
};

/// A SimEvent from LithoKraft's simulation as the sound it stands for.
inline SoundEvent toSoundEvent(const SimEvent& e) { return {SoundCue(e.kind), e.value, e.x, e.y, e.z}; }

}  // namespace lithokraft
