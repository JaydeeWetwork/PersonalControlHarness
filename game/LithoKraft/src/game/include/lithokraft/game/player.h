// Player: the local player's presentation state (position, look, inventory).
// Position is the body's FEET: Game's locomotion (player_locomotion.h — the
// engine's ILocomotion driving the voxel sweep) integrates it per sim tick,
// and this struct never moves itself. Look turning stays here, per render
// frame: mouse feel is a frame-rate concern, movement is a tick concern.
#pragma once
#include "lithokraft/game/engine.h"
#include "litholattice/core/math.h"
#include "litholattice/input/input_system.h"
#include "lithokraft/game/inventory.h"

namespace lithokraft {

struct Player {
  Vec3 pos{20.f, 92.f, 20.f};
  float yaw = 0.785f, pitch = -0.35f;
  Inventory inventory;  ///< 42 carry + 8 gear
  Vec3 forward() const;
  bool flying = false;  ///< mirrors the locomotion mode (Game keeps it in sync): Fly mode = true
  /// Turns by the Look axis when `lookEnabled` (mouse captured, no menu open).
  void updateLook(const InputSystem& in, float dt, bool lookEnabled);
};

}  // namespace lithokraft
