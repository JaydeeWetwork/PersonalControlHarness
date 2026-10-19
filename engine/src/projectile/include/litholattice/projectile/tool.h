// LithoLattice tool: engine interface for block interaction.
// Tools mine, break, and shape blocks. Weapons deal damage to entities.
// Some items are both (a pickaxe is a tool that can be swung as a weapon).
#pragma once
#include <cstdint>

namespace litholattice {

enum class ToolType : uint8_t {
  kPickaxe = 0,  ///< mines stone/ore
  kAxe = 1,      ///< chops wood
  kShovel = 2,   ///< digs dirt/sand
  kShears = 3,   ///< cuts leaves/wire
  kWrench = 4,   ///< interacts with Trace components
};

struct ToolDef {
  uint32_t id = 0;
  ToolType type = ToolType::kPickaxe;
  uint8_t efficiency = 0;  ///< 1-64 (blocks per tick multiplier)
  uint16_t durability = 0;  ///< uses before breaking (0 = infinite)
  uint8_t meleeDamage = 0;  ///< 0-64 if swung as a weapon
};

/// Tool behavior: what happens when it's used on a block.
class IToolBehavior {
 public:
  virtual ~IToolBehavior() = default;
  /// Called when the player uses the tool on a block.
  virtual void onUseOnBlock(uint64_t userId, int x, int y, int z,
                            const ToolDef& def) = 0;
};

}  // namespace litholattice
