// LithoLattice lighting: 1-64 levels, sky light per dimension.
// Mob spawning below light 16 (1/4 of 64).
#pragma once
#include <cstdint>

namespace litholattice {

constexpr uint8_t kMaxLight = 64;
constexpr uint8_t kMobSpawnLight = 16;  ///< mobs spawn below this (1/4 of 64)

/// Sky light level per dimension (0-64). The engine owns this; the game reads it.
inline uint8_t skyLightForDimension(uint8_t dim) {
  switch (dim) {
    case 0: return 8;   // Underwafer: dim
    case 1: return 64;  // Wafer: full daylight
    case 2: return 48;  // Organic World: alien sky
    case 3: return 0;   // Void: no sky
    case 4: return 64;  // the Cosmic: harsh sunlight
    case 5: return 64;  // Ether: radiant
    case 6: return 0;   // Undefined: no light
    default: return 32;
  }
}

/// Lighting interface: the engine computes, the game queries.
class ILighting {
 public:
  virtual ~ILighting() = default;
  /// Light level at (x,y,z) in dimension dim (0-64).
  virtual uint8_t lightAt(int x, int y, int z, uint8_t dim) const = 0;
  /// Sky light for a dimension (0-64).
  virtual uint8_t skyLight(uint8_t dim) const = 0;
  /// Can mobs spawn at (x,y,z)? (light < 16)
  virtual bool canSpawnMob(int x, int y, int z, uint8_t dim) const {
    return lightAt(x, y, z, dim) < kMobSpawnLight;
  }
};

}  // namespace litholattice
