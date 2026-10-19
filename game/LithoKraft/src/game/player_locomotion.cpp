// PlayerLocomotion implementation. LF line endings.
#include "lithokraft/game/player_locomotion.h"

namespace lithokraft {

void PlayerLocomotion::place(const litholattice::Vec3& feet) {
  body_.x = feet.x;
  body_.y = feet.y;
  body_.z = feet.z;
  body_.vx = 0.0;
  body_.vy = 0.0;
  body_.vz = 0.0;
  body_.onGround = false;
  placed_ = true;
}

void PlayerLocomotion::tick(const litholattice::LocomotionInput& in,
                            const std::function<bool(int, int, int)>& isSolid,
                            double dtSeconds) {
  controller_.step(body_, in, isSolid, dtSeconds);
}

}  // namespace lithokraft
