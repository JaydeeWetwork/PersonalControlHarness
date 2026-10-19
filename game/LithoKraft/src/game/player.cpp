#include "lithokraft/game/player.h"
#include <algorithm>
#include "lithokraft/game/actions.h"
#include <cmath>

namespace lithokraft {

Vec3 Player::forward() const {
  return {std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)};
}

void Player::updateLook(const InputSystem& in, float dt, bool lookEnabled) {
  if (!lookEnabled) return;
  const AxisValue look = in.axis(Action::Look, dt);
  yaw += look.x;
  pitch = std::clamp(pitch - look.y, -1.55f, 1.55f);
}

}  // namespace lithokraft
