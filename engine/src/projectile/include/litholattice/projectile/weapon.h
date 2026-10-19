// LithoLattice weapon interface: engine-side.
// Weapons are items that deal damage. Three types: melee, projectile, shockwave.
// The game defines specific weapons; mods can add their own via IMod.
#pragma once
#include <cstdint>
#include "litholattice/projectile/projectile.h"

namespace litholattice {

enum class WeaponType : uint8_t {
  kMelee = 0,      ///< close range, no projectile
  kProjectile = 1, ///< fires a Projectile
  kShockwave = 2,  ///< area of effect
};

struct WeaponDef {
  uint32_t id = 0;
  WeaponType type = WeaponType::kMelee;
  uint8_t damage = 0;       ///< 0-64 (the theme)
  uint8_t range = 0;        ///< melee reach or projectile maxRange (blocks)
  uint8_t penetration = 0;  ///< vs BlockDef::density (projectile only)
  uint16_t cooldownTicks = 0;  ///< ticks between uses (powers of two)
  uint8_t shockwaveRadius = 0; ///< shockwave only (blocks)
};

/// Weapon behavior: what happens when it's used.
class IWeaponBehavior {
 public:
  virtual ~IWeaponBehavior() = default;
  /// Called when the player uses the weapon.
  /// @param ownerId player/entity using it
  /// @param x,y,z position, yaw/pitch direction
  virtual void onUse(uint64_t ownerId, double x, double y, double z,
                     float yaw, float pitch, const WeaponDef& def) = 0;
};

}  // namespace litholattice
