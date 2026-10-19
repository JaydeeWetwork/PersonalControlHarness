// ILocomotion: the engine's character-movement layer (JD-named 2026-10-12).
//
// physics.h owns the AABB-vs-voxel sweep (moveCollide) and deliberately
// leaves gravity in behaviors, because different entities fall differently.
// This module IS that behavior layer for character-shaped bodies (the
// player, MCP-piloted agents, mobs): gravity, movement modes, variable-
// height poses, and Minecraft-reference speeds, all driving moveCollide.
//
// One controller per body. step() is a pure function of (entity state,
// input, isSolid, dt): no wall clock, no RNG — the same inputs replay the
// same motion, for humans and agents alike. The input struct carries a
// WORLD-SPACE wish direction plus button flags only, so the human input
// path and the MCP control surface feed the identical struct.
#pragma once

#include <functional>

#include "litholattice/entity/entity.h"

namespace litholattice {

/// Locomotion mode. Walk is the grounded game mode; Fly is the
/// creative/debug mode — still collision-swept (noclip is not locomotion's
/// business and is handled elsewhere, if at all).
enum class LocomotionMode { Walk, Fly };

/// Body pose. The pose selects the AABB height (and eye height) the sweep
/// uses this step: Stand 1.8, Sneak 1.5, Crawl 0.6 — a crawling body fits a
/// 1-block tunnel.
enum class LocomotionPose { Stand, Sneak, Crawl };

/// One step's intent. wishX/wishZ are a world-space horizontal direction
/// (the caller resolves facing into it); magnitude <= 1, clamped by the
/// controller — partial magnitude means partial speed (analog / fine
/// control). Buttons are level-triggered (held), not edge-triggered.
struct LocomotionInput {
  double wishX = 0, wishZ = 0;
  bool jump = false;
  bool sneak = false;
  bool sprint = false;
  bool crawl = false;
};

/// Tunables for one body's locomotion. Defaults are the Minecraft-reference
/// values (blocks, seconds); every field is per-body overridable.
struct LocomotionConfig {
  double halfWidth = 0.3;        ///< AABB half extent in x/z.
  double heightStand = 1.8;      ///< AABB heights per pose.
  double heightSneak = 1.5;
  double heightCrawl = 0.6;
  double eyeStand = 1.62;        ///< eye heights per pose (feet-relative).
  double eyeSneak = 1.27;
  double eyeCrawl = 0.4;
  double walkSpeed = 4.317;      ///< blocks/s.
  double sprintMultiplier = 1.3; ///< applied to walkSpeed when sprinting.
  double sneakSpeed = 1.31;      ///< blocks/s.
  double crawlSpeed = 1.31;      ///< blocks/s.
  double flySpeed = 10.9;        ///< blocks/s, horizontal (Fly).
  double flyVerticalSpeed = 10.9;///< blocks/s, vertical (Fly).
  double gravity = 32.0;         ///< blocks/s^2, downward (Walk).
  double terminalVelocity = -60.0; ///< fall-speed clamp (Walk), <= 0.
  /// Jump apex height in blocks. Jump velocity is DERIVED from it and the
  /// gravity above: v = sqrt(2 * gravity * jumpHeight) ~= 8.94 blocks/s at
  /// the defaults — tune the height, the velocity follows.
  double jumpHeight = 1.25;
  /// Horizontal velocity approach rates (1/s) for the frame-rate-independent
  /// exponential approach v += (target - v) * (1 - exp(-rate * dt)): snappy
  /// on the ground, drifting in the air.
  double groundAccel = 40.0;
  double airAccel = 4.0;
};

/// Character movement for one body: modes, poses, gravity, speeds.
///
/// Per step (Walk):
///  1. Pose resolution (sets the AABB the sweep uses): desired = Crawl if
///     crawl input, else Sneak if sneak input, else Stand; then reduced to
///     the tallest pose whose AABB actually fits at the body's position
///     (fit test = the same floor-based voxel scan moveCollide collides
///     with, so the two never disagree). Consequences: growing requires
///     room; a body in a space where only Crawl fits is in Crawl regardless
///     of inputs (forced crawl, 1-block tunnels); if nothing fits (the body
///     is embedded), Crawl is kept and the sweep proceeds. The fit test is
///     also applied at the position this step's intended horizontal travel
///     reaches: if the resolved pose would not fit there but a shorter one
///     would, the body lowers pre-emptively — that is what lets a walking
///     body drop into a 1-block tunnel mouth instead of stopping flush
///     against it (the sweep's clamp leaves the AABB 1e-4 short of the
///     boundary, where a purely positional test can never see the tunnel).
///     If no pose fits at the destination (a wall, a step), the probe
///     changes nothing and the sweep clamps as usual.
///  2. Horizontal velocity: target = wish (clamped to magnitude 1) *
///     speed(pose, sprint) — sprint only in Stand; exponential approach at
///     groundAccel when grounded, airAccel when airborne.
///  3. Vertical: jump (only when grounded at step start) sets vy to the
///     derived jump velocity, then gravity applies, clamped at
///     terminalVelocity.
///  4. Sneak edge-guard: a sneaking, grounded body must not be carried off
///     its support — after the swept move, if it ended airborne AND no
///     solid voxel exists within 1.05 blocks below any part of its
///     footprint at the new position, the full pre-move position and the
///     grounded flag are restored (the pre-move support state), as if the
///     step had not moved, and vy is zeroed by rule 5. Restoring y and the
///     flag along with x/z is load-bearing: a kept mid-step y sits
///     millimetres below the support's top surface, where the fit test's
///     scan includes the supporting voxel row itself and reports every
///     pose as unfit — collapsing the pose to Crawl, which would disarm
///     this Sneak-only guard on the next step. Support one block down
///     within 1.05 is a legal sneak step-down; stepping into void is not.
///     Walking off the same edge without sneak falls normally.
///  5. Integration via moveCollide, substepped so no single substep moves
///     any axis more than ~0.5 blocks: moveCollide is a discrete per-axis
///     move and would tunnel through 1-block floors at terminal velocity
///     under coarse dt. After integration, grounded + falling => vy = 0.
///
/// Fly: no gravity; vy is set directly from jump/sneak (+/- flyVertical
/// Speed; note the sneak input there does double duty — it also requests
/// the Sneak pose, per the mode-independent pose resolution above);
/// horizontal target = wish * flySpeed approached at groundAccel.
///
/// Deliberately omitted (future feel work, do not read as missing): swim /
/// climb modes, coyote time, jump buffering, auto-step.
class ILocomotion {
 public:
  virtual ~ILocomotion() = default;

  /// Advance the body one step of dt seconds against the voxel world.
  virtual void step(Entity& e, const LocomotionInput& in,
                    const std::function<bool(int, int, int)>& isSolid, double dt) = 0;

  virtual LocomotionMode mode() const = 0;
  virtual void setMode(LocomotionMode m) = 0;
  /// Current pose (post-resolution of the last step; Stand before it).
  virtual LocomotionPose pose() const = 0;
  /// Eye height / AABB height of the current pose; AABB half width.
  virtual double eyeHeight() const = 0;
  virtual double currentHeight() const = 0;
  virtual double halfWidth() const = 0;
};

/// The concrete controller. Construct one per body from its config.
class LocomotionController : public ILocomotion {
 public:
  explicit LocomotionController(LocomotionConfig cfg = {}) : cfg_(cfg) {}

  void step(Entity& e, const LocomotionInput& in,
            const std::function<bool(int, int, int)>& isSolid, double dt) override;

  LocomotionMode mode() const override { return mode_; }
  void setMode(LocomotionMode m) override { mode_ = m; }
  LocomotionPose pose() const override { return pose_; }
  double eyeHeight() const override;
  double currentHeight() const override;
  double halfWidth() const override { return cfg_.halfWidth; }

  const LocomotionConfig& config() const { return cfg_; }

 private:
  LocomotionConfig cfg_;
  LocomotionMode mode_ = LocomotionMode::Walk;
  LocomotionPose pose_ = LocomotionPose::Stand;
};

}  // namespace litholattice
