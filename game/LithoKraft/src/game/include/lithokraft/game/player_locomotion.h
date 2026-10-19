// PlayerLocomotion: game-side owner of the local player's locomotion body.
//
// The engine owns the movement rules (litholattice/entity/locomotion.h:
// ILocomotion — modes Walk/Fly, poses Stand/Sneak/Crawl with fit-tested
// heights, gravity, sprint, sneak edge-guard, forced crawl in 1-block
// spaces) and the AABB-vs-voxel sweep it drives (physics.h moveCollide).
// This host owns the one body the local player moves through the world
// with: an engine Entity (feet-center position, blocks/s velocity) plus its
// LocomotionController. Game steps it once per sim tick with a buffered
// LocomotionInput; everything else (adapter, cameras, HUD) reads the body.
//
// Solidity comes from the caller (Game passes the same getBlock-based query
// the item entities use), so this class stays world-agnostic — the app
// selftest drives it against a hand-built ChunkMap with a fixed dt.
#pragma once

#include <functional>

#include "litholattice/core/math.h"  // Vec3
#include "litholattice/entity/entity.h"
#include "litholattice/entity/locomotion.h"

namespace lithokraft {

class PlayerLocomotion {
 public:
  PlayerLocomotion() = default;

  /// (Re)seat the body at `feet` with zero velocity: spawn, respawn,
  /// teleports, dimension switches, and externally-driven presentation
  /// (menu dolly, bench paths, free camera) all route through here, so the
  /// body never fights an outside write to the player position.
  void place(const litholattice::Vec3& feet);

  /// True once place() has seated the body (Game places it on the first
  /// frame; before that the body sits at the origin and must not be read).
  bool placed() const { return placed_; }

  /// Advance the body one fixed step of dt seconds against the voxel world.
  void tick(const litholattice::LocomotionInput& in,
            const std::function<bool(int, int, int)>& isSolid, double dtSeconds);

  litholattice::Vec3 position() const {
    return {float(body_.x), float(body_.y), float(body_.z)};
  }  ///< feet center
  litholattice::Vec3 velocity() const {
    return {float(body_.vx), float(body_.vy), float(body_.vz)};
  }  ///< blocks/s
  bool onGround() const { return body_.onGround; }

  litholattice::LocomotionMode mode() const { return controller_.mode(); }
  void setMode(litholattice::LocomotionMode m) { controller_.setMode(m); }
  bool flying() const { return mode() == litholattice::LocomotionMode::Fly; }
  litholattice::LocomotionPose pose() const { return controller_.pose(); }
  /// Eye height of the current pose (feet-relative): Stand 1.62, Sneak
  /// 1.27, Crawl 0.4 at the contract defaults.
  float eyeHeight() const { return float(controller_.eyeHeight()); }

  const litholattice::Entity& body() const { return body_; }
  litholattice::ILocomotion& controller() { return controller_; }
  const litholattice::ILocomotion& controller() const { return controller_; }

 private:
  litholattice::Entity body_{};
  litholattice::LocomotionController controller_{};
  bool placed_ = false;
};

/// App-selftest section (fixed-dt drives against a hand-built world: fall +
/// land, wall stop, forced crawl through a 1-block tunnel, fly, and the
/// adapter's true-velocity handoff). Returns the failure count.
int runLocomotionSelfTest();

}  // namespace lithokraft
