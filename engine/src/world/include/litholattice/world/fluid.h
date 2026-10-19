// LithoLattice fluids: engine interface.
// Flow levels 1-16 (1-based, like Minecraft's 0-15 but ours). Water, lava,
// chemicals, etc. Based on block properties.
#pragma once
#include <cstdint>

namespace litholattice {

constexpr uint8_t kMaxFluidLevel = 16;

enum class FluidType : uint8_t {
  kNone = 0,
  kWater = 1,
  kLava = 2,
  kChemical = 3,  ///< moddable: acids, etc.
};

/// Fluid state for a block (stored in block metadata).
struct FluidState {
  FluidType type = FluidType::kNone;
  uint8_t level = 0;  ///< 1-16 (1 = trickle, 16 = source)
  bool falling = false;
};

/// Fluid simulation: the engine ticks this, the game defines the fluids.
class IFluidSim {
 public:
  virtual ~IFluidSim() = default;
  /// Tick fluids for one sim tick. Spreads levels 1-16 to neighbors.
  virtual void tick() = 0;
  /// Can this fluid flow into the block at (x,y,z)?
  virtual bool canFlowInto(int x, int y, int z, FluidType type) const = 0;
};

}  // namespace litholattice
