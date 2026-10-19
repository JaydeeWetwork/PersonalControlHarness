// Block behaviours: one class per block type that does something.
//
// A block opts into a behaviour by name in the block registry (BlockDef::behavior, e.g. "repeater").
// BehaviorCatalog resolves those names to instances once, so neither the sim nor the HUD switches
// on block types; a new block is one class plus one registry line.
//  - BlockBehavior: describe() for the look-at readout (wire, torch, lamps, every component).
//  - ComponentBehavior: also ticks, emits signal and reacts to right-click (repeater, pulsor, ...).
// Behaviours are stateless and shared; per-cell runtime data lives in ComponentMemory.
#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "lithokraft/engine.h"
#include "lithokraft/content/blocks.h"
#include "litholattice/sim/tick_rate.h"

namespace lithokraft {

/// Per-cell runtime memory the sim keeps for each placed component.
struct ComponentMemory {
  uint8_t out = 0, out2 = 0;   ///< outputs this tick: main port / secondary port
  bool pulse = false;          ///< main output is a no-decay pulse this tick (pulsor)
  uint8_t prevIn = 0, prevSide = 0, prevClk = 0;  ///< edge detectors
  uint8_t history = 0;         ///< repeater delay line (bit i = input i ticks ago)
  uint8_t timer = 0;           ///< button press / pulsor animation countdown
  std::deque<std::pair<uint8_t, uint8_t>> keys;  ///< keyboard: queued (front, control) pulses
  uint8_t channel = 1;         ///< radio channel for the transmittor / receivor (1..64; 0 = the Daemon, reserved)
  bool jammer = false;         ///< Overclockor: jammer item installed (v1: toggled by use; item UI later)
  bool beacon = false;         ///< transmittor: generate its own signal instead of reading the wire
};

/// Signal arriving at a component this tick, by absolute direction (0 N, 1 E, 2 S, 3 W).
struct ComponentInputs {
  std::array<int, 4> side{};  ///< strength arriving from each horizontal neighbour
  int top = 0;                ///< wire lying on top of the block
  std::array<uint8_t, 4> ch{}; ///< colour channel of the wire / torch on each side (0 = none / component)
  int radioIn = 0;            ///< wireless signal on this cell's channel (receivors only)
  uint8_t radioCol = 0;       ///< colour of the received wireless signal (1 red, 2 blue)

  int back(int facing) const { return side[dirBack(facing)]; }
  int left(int facing) const { return side[dirLeft(facing)]; }
  int right(int facing) const { return side[dirRight(facing)]; }
};

/// Sounds the sim reports to the game (which turns them into audio commands).
enum class SoundCue : uint8_t { Place, Break, Click, PulsorFire, Tone };

/// Result of one component tick.
struct ComponentStep {
  int out = 0;         ///< main output strength (0..64)
  int out2 = 0;        ///< secondary port strength (keyboard control port)
  CellState state = 0; ///< new cell state (drives the look and the HUD readout)
  bool pulse = false;  ///< `out` travels the wire run without decay for one tick
  std::optional<SoundCue> sound{};///< emitted with the new state's value (pitch / strength)
};

/// Readout of a block type. Implementations must be stateless (one instance serves every cell).
class BlockBehavior {
 public:
  virtual ~BlockBehavior() = default;

  /// One-line HUD readout of a cell, e.g. "Repeater | delay 4 | out 64 | facing E".
  virtual std::string describe(const BlockDef& def, CellState /*state*/) const { return std::string(def.name); }
};

/// Logic of one signal component type.
class ComponentBehavior : public BlockBehavior {
 public:

  /// Computes this tick's outputs from the previous tick's inputs; may update `mem`.
  virtual ComponentStep tick(const ComponentInputs& in, CellState state, ComponentMemory& mem) const = 0;

  /// Strength driven toward absolute direction `dir`. Default: main output on the front only.
  virtual int emitToward(int dir, CellState state, const ComponentMemory& mem) const {
    return dir == stFacing(state) ? mem.out : 0;
  }

  /// Wire colour the output toward dir may enter (0 = any, else a wire channel: 1 red, 2 blue).
  virtual int emitChannel(int /*dir*/, CellState /*state*/, const ComponentMemory& /*mem*/) const { return 0; }

  /// Using an adjacent wire of colour channel (1 red, 2 blue): nullopt if that does nothing.
  virtual std::optional<CellState> useChannel(CellState /*state*/, int /*channel*/) const { return std::nullopt; }

  /// Right-click interaction. Returns the new cell state, or nullopt if the block is not usable.
  virtual std::optional<CellState> use(CellState /*state*/, ComponentMemory& /*mem*/) const { return std::nullopt; }
  /// True when, with the same inputs as last tick, tick() would change nothing (output, state,
  /// memory): the sim may then skip it until a neighbour changes (component sleep). Default: never.
  virtual bool quiescent(CellState /*state*/, const ComponentMemory& /*mem*/) const { return false; }
  /// Tick rate of this component class relative to the world tick (JD
  /// 2026-10-14, Timing Handshake rate scaling): divide = the sim pulls
  /// this class once per `divide` world ticks (its published outputs
  /// hold between pulls); multiply = the class is sub-pulled `multiply`
  /// times per world tick (ComponentMemory chains across sub-pulls; only
  /// the final sub-pull's step publishes). Default neutral (1/1): every
  /// tick, once — the pre-scaling behaviour. Lithography changes a chip
  /// class's rate by overriding THIS flag: a simple flag change on the
  /// class, nothing else (JD's ruling). Powers of two; see
  /// litholattice/sim/tick_rate.h.
  virtual litholattice::TickRate tickRate() const { return {}; }
  /// Sneak + use (secondary setting, e.g. a Catchor's match value). Default: nothing.
  virtual std::optional<CellState> useAlt(CellState /*state*/, ComponentMemory& /*mem*/) const { return std::nullopt; }
};

/// Name -> behaviour table, and BlockId -> behaviour resolved through the block registry.
class BehaviorCatalog {
 public:
  /// The catalog with every built-in behaviour, bound to the blocks that name them.
  static const BehaviorCatalog& get();

  /// Behaviour of block `b` (a plain name-only readout for blocks without one).
  const BlockBehavior& behavior(BlockId b) const;
  /// Component logic of block `b`, or nullptr if it is not a ticking component.
  const ComponentBehavior* component(BlockId b) const { return b < components_.size() ? components_[b] : nullptr; }
  /// Behaviour registered under `name`, or nullptr.
  const BlockBehavior* byName(std::string_view name) const;
  /// HUD readout of a cell: the block's behaviour describes its state.
  std::string describe(BlockId b, CellState state) const { return behavior(b).describe(blockRegistry().def(b), state); }

 private:
  BehaviorCatalog();
  void add(std::string_view name, std::unique_ptr<BlockBehavior> behavior);

  BlockBehavior plain_;
  std::vector<std::pair<std::string_view, std::unique_ptr<BlockBehavior>>> named_;
  std::vector<const BlockBehavior*> behaviors_;
  std::vector<const ComponentBehavior*> components_;
};

/// Compass name of a facing (N, E, S, W).
const char* facingName(int facing);

}  // namespace lithokraft
