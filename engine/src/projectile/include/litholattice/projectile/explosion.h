// LithoLattice explosion: engine interface for AoE damage.
// IWeapon can emit IProjectile (ranged) or IExplosion (area).
#pragma once
#include <cstdint>

namespace litholattice {

/// An explosion in the world. Destroys blocks, damages entities.
struct Explosion {
  double x = 0, y = 0, z = 0;  ///< center
  uint8_t radius = 0;          ///< blocks (0-64)
  uint8_t damage = 0;          ///< 0-64 at center
  uint8_t penetration = 0;     ///< vs BlockDef::density (which blocks break)
  uint64_t ownerId = 0;        ///< who caused it (no self-damage if 0)
  uint8_t dimension = 1;
  bool damagesAttacker = true;  ///< risk/reward (default: yes)
};

/// Explosion behavior: the engine handles the physics, the game defines the effects.
class IExplosionBehavior {
 public:
  virtual ~IExplosionBehavior() = default;
  /// Called when the explosion detonates. Engine has already calculated
  /// affected blocks/entities. Implement visual/audio effects here.
  virtual void onDetonate(const Explosion& e) = 0;
};

}  // namespace litholattice
