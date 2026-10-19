// LithoLattice camera system (engine level): virtual cameras that attach to
// anything that moves — entities, players, agents, mobs — and produce the
// pose / view / projection the renderers consume.
//
// DESIGN TAKEAWAYS (clean-room: concepts from public talks, docs and articles
// on modern game cameras — Unreal SpringArm, Unity Cinemachine, Godot
// SpringArm3D/Camera3D, Eiserloh's GDC 2016 camera-shake talk — re-derived
// here, no code copied):
//  - Virtual cameras, not one camera: gameplay owns several named cameras
//    (first person, orbit, overwatch...) and a "brain" (CameraManager) picks
//    the live one and BLENDS poses between them with an easing curve and a
//    duration; a blend of 0 is a cut. Blending position + orientation (and
//    FOV) is what makes transitions feel intentional instead of teleporting.
//  - Third person = orbit + spring arm: the camera hangs off a pivot on a
//    boom of desired length. A collision probe cast from the pivot toward
//    the desired camera spot pulls the camera IN instantly when geometry
//    blocks the boom (never clip through a wall), but the boom eases back
//    OUT slowly — asymmetric response is what stops wall-adjacent jitter.
//  - Over-shoulder framing is just a lateral/up offset of the pivot in
//    camera space (Unreal's "socket offset"); it costs nothing and reads as
//    a deliberate composition choice, so it is a config value, not a mode.
//  - Damping must be frame-rate independent: use the exact exponential form
//    x' = target + (x - target) * exp(-lambda * dt). The naive
//    lerp(x, target, k*dt) converges twice as fast at 120 fps as at 60 fps
//    and is the single most common camera bug (Godot proposal discussions
//    and every "lerp is broken" writeup circle back to this).
//  - Shake is trauma, not noise amplitude: events ADD trauma in [0,1];
//    trauma decays linearly; the applied shake scales with trauma^2 so small
//    bumps stay subtle and only real hits shout. Offsets come from smooth
//    noise sampled over time (never per-frame random — that is buzzy and
//    frame-rate dependent), and in 3D the shake is mostly ROTATIONAL
//    (yaw/pitch/roll): translational shake swims with parallax and can push
//    the camera into geometry.
//  - FOV is a feel knob: a speed-based FOV kick (widen as the target
//    accelerates/sprints) sells speed without moving the camera. Kept as a
//    small clamped addition on top of the base FOV, never a replacement.
//  - Cameras are presentation only. They read target state and a world
//    query seam; they never write to the sim, never read a wall clock (dt
//    is passed in), and a dead target degrades to "hold last pose", never
//    to a dangling pointer.
//
// CONVENTIONS (matched to the engine, do not drift):
//  - Matrices: core/math.h — column-major Mat4, OpenGL clip space;
//    viewProjection = mul(perspective(...), view) exactly like Game and the
//    MCP senses adapter build it.
//  - Yaw/pitch are RADIANS, Player::forward convention:
//    forward = {cos(pitch)cos(yaw), sin(pitch), cos(pitch)sin(yaw)}
//    (yaw measured from +X toward +Z, pitch positive = looking up).
//  - FOV is DEGREES (the MCP CameraPose convention), clamped to [10, 120].
//  - Positions are Vec3 in world blocks, doubles nowhere: render-rate math
//    in float, like the rest of the render path.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "litholattice/core/math.h"

namespace litholattice::console {
class ConsoleRegistry;  // console/concmd.h — only the cvar TU includes it
}  // namespace litholattice::console

namespace litholattice::camera {

/// Anything a camera can attach to: a player, an agent pilot, a mob, a
/// projectile — if it moves and might want to see (or be seen from), it can
/// be a target. Implemented by adapters over game/engine objects; the camera
/// holds the target WEAKLY (see ICamera::attach), so a target may die at
/// any time without taking the camera with it.
class ICameraTarget {
 public:
  virtual ~ICameraTarget() = default;
  /// Root/feet position in world blocks. The camera adds eye/pivot offsets.
  virtual Vec3 anchorPosition() const = 0;
  /// Eye position relative to the anchor (first person). Default: standing
  /// eye height of the game's player proportions.
  virtual Vec3 eyeOffset() const { return {0.f, 1.625f, 0.f}; }
  /// Facing direction, radians, Player::forward convention (see header).
  virtual float facingYaw() const = 0;
  virtual float facingPitch() const { return 0.f; }
  /// World velocity, blocks/second. Drives FOV kick, follow look-ahead and
  /// auto-align. Zero when the target cannot report it.
  virtual Vec3 velocity() const { return {}; }
};

/// World queries the camera needs (spring-arm collision). A seam, not a
/// world pointer: the game implements this over its voxel raycast
/// (world/raycast.h DDA), tests implement it over nothing. Implementations
/// must be cheap — called once or twice per camera per frame.
struct CameraRayHit {
  bool hit = false;
  float distance = 0.f;  ///< blocks along the ray to the obstruction
};
class ICameraWorld {
 public:
  virtual ~ICameraWorld() = default;
  /// First obstruction along the ray, or no hit within maxDist.
  /// `dir` is normalized. Implementations should treat the camera as a
  /// sphere of `probeRadius` (e.g. shrink the hit distance by it) — the
  /// camera applies its own margin on top, so a plain ray is acceptable.
  virtual CameraRayHit castRay(Vec3 origin, Vec3 dir, float maxDist) const = 0;
};
/// A world with no geometry: rays never hit (open-sky cameras, tests, menus).
class NullCameraWorld final : public ICameraWorld {
 public:
  CameraRayHit castRay(Vec3, Vec3, float) const override { return {}; }
};

enum class CameraMode : uint8_t {
  FirstPerson,  ///< at the target's eye, looking along its facing (+ own look deltas)
  ThirdPerson,  ///< orbit boom behind the target (spring arm + collision)
  Free,         ///< independent fly/spectate camera; ignores the target
  Fixed,        ///< holds a set pose; aims at the target when one is attached
  Follow,       ///< chase camera: trails behind the target's motion with lag
};

/// One camera's output for a frame: an orthonormal pose + the FOV in
/// effect (post-kick). Everything a renderer needs that FrameParams does
/// not already carry.
struct CameraPose {
  Vec3 position{};
  Vec3 forward{1.f, 0.f, 0.f};  ///< normalized
  Vec3 up{0.f, 1.f, 0.f};       ///< normalized, perpendicular to forward
  float fovDeg = 70.f;
};

/// Tunables for one camera. Defaults are the engine's starting point
/// (MCP/agent cameras already use FOV 70 and the 10..120 clamp; the game
/// render path uses near 0.05 / far 1000). Damping values are exponential
/// rates lambda (1/second): bigger = tighter. See damp() in camera.cpp.
struct CameraConfig {
  float fovDeg = 70.f;         ///< base FOV, degrees, clamped [10, 120]
  float nearPlane = 0.05f;
  float farPlane = 1000.f;
  float aspect = 16.f / 9.f;   ///< consumer sets from the framebuffer size
  int chunkRadiusChunks = 4;   ///< JD's camera chunk budget (cvar camera.chunk_radius)

  // First person.
  bool headBob = false;              ///< OFF by default (motion sickness + agent vision)
  float headBobAmplitude = 0.045f;   ///< blocks, vertical
  float headBobFrequency = 11.f;     ///< radians of phase per block travelled... see camera.cpp
  bool fovKick = true;               ///< speed-based FOV widen hook
  float fovKickMaxDeg = 8.f;         ///< added at fovKickReferenceSpeed
  float fovKickReferenceSpeed = 60.f;  ///< blocks/sec (the game's sprint speed)

  // Third person (orbit + spring arm).
  float orbitDistance = 6.f;         ///< desired boom length, blocks
  float orbitMinDistance = 1.f;
  float orbitMaxDistance = 16.f;
  float orbitPitchMin = -1.45f;      ///< radians (~ +/-83 deg; inside the +/-1.55 player clamp)
  float orbitPitchMax = 1.45f;
  Vec3 pivotOffset{0.f, 1.5f, 0.f};  ///< boom pivot above the anchor
  Vec3 shoulderOffset{0.9f, 0.25f, 0.f};  ///< pivot shift in camera space: +x = over the right shoulder, +y = up
  float probeRadius = 0.3f;          ///< collision probe: boom shortens by this past a hit
  float positionDamping = 10.f;      ///< pivot follow rate (exp lambda)
  float rotationDamping = 14.f;      ///< orbit angle smoothing rate (exp lambda)
  bool autoAlign = false;            ///< drift the orbit behind the target's facing while it moves
  float autoAlignDelaySec = 2.f;     ///< ... after this long without look input
  float autoAlignRate = 1.2f;        ///< exp lambda of the drift

  // Follow (chase).
  float followDistance = 8.f;
  float followHeight = 3.f;
  float followLag = 4.f;             ///< position exp lambda (the "chase" softness)
  float followLookAheadSec = 0.35f;  ///< aim this far ahead of the anchor along velocity

  // Free (fly/spectate).
  float freeDollySpeed = 24.f;       ///< blocks/sec for zoom-input dolly

  // Shake (trauma model, applied to every mode's final pose).
  float shakeDecayPerSec = 1.2f;     ///< linear trauma decay rate
  float shakeMaxRollDeg = 2.5f;      ///< roll at trauma = 1
  float shakeMaxPitchDeg = 2.f;
  float shakeMaxYawDeg = 2.f;
  float shakeMaxOffset = 0.25f;      ///< translational shake, blocks (small: rotation does the work)
  float shakeFrequency = 22.f;       ///< noise sample rate, Hz-ish
  float shakeScale = 1.f;            ///< master multiplier (cvar camera.shake_scale)
};

/// A frustum as 6 normalized planes (inside = signed distance >= 0 on all
/// six), extracted from a view-projection matrix with the same plane
/// convention as core math's aabbInFrustum, so results agree with the
/// renderer's chunk culling.
struct Frustum {
  struct Plane {
    float a = 0, b = 0, c = 0, d = 1;
    float signedDistance(Vec3 p) const { return a * p.x + b * p.y + c * p.z + d; }
  };
  Plane planes[6];
  bool containsPoint(Vec3 p) const;
  /// P-vertex test: true when the box is at least partly inside.
  bool intersectsAabb(Vec3 mn, Vec3 mx) const;
};
/// Gribb-Hartmann extraction from a combined view-projection matrix.
Frustum extractFrustum(const Mat4& viewProj);

/// View matrix for a pose (lookDir layout, but honouring pose.up so shake
/// roll survives; reduces to core lookDir when up is the world-up basis).
Mat4 cameraViewMatrix(const CameraPose& pose);
/// Projection matrix for a lens (core perspective; fovDeg in degrees).
Mat4 cameraProjectionMatrix(float fovDeg, float aspect, float nearPlane, float farPlane);

/// The camera interface. One concrete implementation ships in this module
/// (class Camera); the interface exists so the game can substitute (e.g. a
/// scripted cinematic camera) without the manager caring.
///
/// Ownership: cameras are shared_ptr-managed by CameraManager or the game.
/// Targets are held WEAKLY: attach() takes a weak_ptr, and when it expires
/// the camera reports hasTarget() == false and holds its last pose.
class ICamera {
 public:
  virtual ~ICamera() = default;

  virtual void attach(std::weak_ptr<ICameraTarget> target) = 0;
  virtual void detach() = 0;
  /// True while an attached target is still alive.
  virtual bool hasTarget() const = 0;

  virtual void setMode(CameraMode mode) = 0;
  virtual CameraMode mode() const = 0;

  /// Look input, radians. FirstPerson: an offset on top of the target's
  /// facing (zero offset = track the target exactly, the game's mouse-look
  /// path). Free: rotates the camera's own yaw/pitch. ThirdPerson: orbits
  /// the boom. Fixed: ignored unless un-aimed.
  virtual void addLookInput(float yawDeltaRad, float pitchDeltaRad) = 0;
  /// Zoom input, abstract units (wheel notches / seconds of key hold):
  /// ThirdPerson dollys the boom (positive = closer), Free dollies along
  /// the view direction, Follow scales its distance.
  virtual void addZoomInput(float delta) = 0;
  /// Teleport/rewrite the camera's own pose (Free placement, Fixed pose).
  /// Snaps all damping state so the next update does not swoop.
  virtual void setPose(const CameraPose& pose) = 0;
  /// Fixed-mode aim point override: when set (and no live target), Fixed
  /// looks at this point instead of holding a yaw/pitch.
  virtual void setFixedLookAt(Vec3 point) = 0;
  virtual void clearFixedLookAt() = 0;

  /// Trauma-based shake: add in [0,1] (clamped, stacks). See header notes.
  virtual void addTrauma(float amount) = 0;
  virtual float trauma() const = 0;

  /// Advance one presentation frame. dt is seconds since the last update
  /// (passed in — no wall clock in here). `world` answers collision rays.
  virtual void update(float dt, const ICameraWorld& world) = 0;

  virtual CameraPose pose() const = 0;
  virtual Mat4 viewMatrix() const = 0;
  virtual Mat4 projectionMatrix() const = 0;
  virtual Mat4 viewProjectionMatrix() const = 0;
  virtual Frustum frustum() const = 0;
  /// Where this camera needs chunks resident: its own position (JD's rule:
  /// cameras load in a LIMITED radius, never the player's full distances).
  virtual Vec3 chunkLoadCenter() const = 0;
  virtual int chunkLoadRadiusChunks() const = 0;

  virtual CameraConfig& config() = 0;
  virtual const CameraConfig& config() const = 0;
};

/// The shipping camera: all five modes, spring-arm collision, trauma shake.
class Camera final : public ICamera {
 public:
  Camera();
  explicit Camera(CameraConfig cfg) : Camera() { cfg_ = cfg; }

  void attach(std::weak_ptr<ICameraTarget> target) override;
  void detach() override;
  bool hasTarget() const override;
  void setMode(CameraMode mode) override;
  CameraMode mode() const override { return mode_; }
  void addLookInput(float yawDeltaRad, float pitchDeltaRad) override;
  void addZoomInput(float delta) override;
  void setPose(const CameraPose& pose) override;
  void setFixedLookAt(Vec3 point) override { fixedLookAt_ = point; fixedLookAtSet_ = true; }
  void clearFixedLookAt() override { fixedLookAtSet_ = false; }
  void addTrauma(float amount) override;
  float trauma() const override { return trauma_; }
  void update(float dt, const ICameraWorld& world) override;
  CameraPose pose() const override { return pose_; }
  Mat4 viewMatrix() const override;
  Mat4 projectionMatrix() const override;
  Mat4 viewProjectionMatrix() const override;
  Frustum frustum() const override { return extractFrustum(viewProjectionMatrix()); }
  Vec3 chunkLoadCenter() const override { return pose_.position; }
  int chunkLoadRadiusChunks() const override { return cfg_.chunkRadiusChunks; }
  CameraConfig& config() override { return cfg_; }
  const CameraConfig& config() const override { return cfg_; }

 private:
  std::shared_ptr<ICameraTarget> lockTarget() const {
    return target_.lock();
  }
  void seedFromTarget(const ICameraTarget& t);
  void snapToPose(const CameraPose& p);

  CameraConfig cfg_;
  CameraMode mode_ = CameraMode::Free;
  std::weak_ptr<ICameraTarget> target_;

  // Own orientation state (radians). FP/Free use yaw_/pitch_; TP uses
  // orbitYaw_/orbitPitch_ + boom length; all seeded from the target.
  float yaw_ = 0.f, pitch_ = 0.f;
  float fpYawOffset_ = 0.f, fpPitchOffset_ = 0.f;  // FP: look deltas ride on top of target facing
  float orbitYaw_ = 0.f, orbitPitch_ = 0.2f;
  float smoothOrbitYaw_ = 0.f, smoothOrbitPitch_ = 0.2f;
  float orbitDistance_ = 6.f;   ///< current (zoom) desired boom length
  bool orbitZoomed_ = false;    ///< user zoomed: cfg_.orbitDistance no longer drives orbitDistance_ (CAM-3)
  bool followZoomed_ = false;   ///< user zoomed: cfg_.followDistance no longer drives followDistance_ (CAM-3)
  float armLength_ = 6.f;       ///< collision/damping-resolved boom length
  Vec3 smoothPivot_{};          ///< damped pivot (TP) / damped anchor (Follow)
  bool stateSeeded_ = false;    ///< damping state matches the target (no swoop pending)
  float sinceLookInputSec_ = 1e9f;

  Vec3 freePos_{};              ///< Free mode position
  Vec3 followPos_{};            ///< Follow mode damped position
  float followDistance_ = 8.f;  ///< Follow mode current distance (zoom-adjustable)
  bool followSeeded_ = false;

  Vec3 fixedPos_{};             ///< Fixed mode position
  bool fixedLookAtSet_ = false;
  Vec3 fixedLookAt_{};

  float bobPhase_ = 0.f;        ///< head-bob phase (radians), distance-driven
  float effectiveFovDeg_ = 70.f;

  float trauma_ = 0.f;
  float shakeTime_ = 0.f;       ///< noise clock, advanced by dt only
  uint32_t shakeSeed_ = 0;      ///< per-camera noise offset (set from an instance counter)

  CameraPose pose_{};           ///< last output pose (shake applied)
  CameraPose basePose_{};       ///< last mode pose BEFORE shake (shake never accumulates)
};

/// Blend curves for camera transitions (Cinemachine's vocabulary).
enum class CameraBlendEase : uint8_t { Cut, Linear, EaseIn, EaseOut, EaseInOut, HardIn, HardOut };
/// Evaluate an easing curve at t in [0,1].
float cameraBlendFactor(CameraBlendEase ease, float t);

/// Owns the named cameras and the live output pose (Cinemachine's "brain"
/// role): setActive() starts a timed blend from the current output pose —
/// through BOTH cameras' live poses — to the new camera. All registered
/// cameras update every frame so their damping state is warm when a blend
/// reaches them (no swoop-in from stale state).
class CameraManager {
 public:
  /// Registers (or replaces) a named camera. Returns false on empty name.
  bool addCamera(const std::string& name, std::shared_ptr<ICamera> cam);
  void removeCamera(const std::string& name);
  ICamera* camera(const std::string& name);
  const ICamera* camera(const std::string& name) const;
  size_t cameraCount() const { return cameras_.size(); }

  /// Make `name` the live camera. blendSeconds <= 0 (or ease == Cut) cuts.
  /// Returns false when the name is unknown.
  bool setActive(const std::string& name, float blendSeconds = 0.f,
                 CameraBlendEase ease = CameraBlendEase::EaseInOut);
  const std::string& activeName() const { return activeName_; }
  bool blending() const { return blending_; }

  void update(float dt, const ICameraWorld& world);

  /// The live (possibly blended) output. Before the first setActive, this
  /// is a default pose at the origin looking down +X with FOV 70.
  CameraPose pose() const { return outPose_; }
  Mat4 viewMatrix() const { return cameraViewMatrix(outPose_); }
  /// Projection from the ACTIVE camera's lens (near/far/aspect) and the
  /// blended FOV. Identity-ish default when no camera is active.
  Mat4 projectionMatrix() const;
  Mat4 viewProjectionMatrix() const { return mul(projectionMatrix(), viewMatrix()); }
  Frustum frustum() const { return extractFrustum(viewProjectionMatrix()); }
  Vec3 chunkLoadCenter() const { return outPose_.position; }
  /// The active camera's chunk radius (0 when no camera is active).
  int chunkLoadRadiusChunks() const;

 private:
  struct Entry {
    std::shared_ptr<ICamera> cam;
  };
  std::map<std::string, Entry> cameras_;
  std::string activeName_;
  // Blend state: from the pose captured when setActive ran (kept live by
  // also updating the source camera) to the active camera's live pose.
  std::string blendFromName_;
  CameraPose blendFromPose_{};
  bool blending_ = false;
  float blendElapsed_ = 0.f, blendDuration_ = 0.f;
  CameraBlendEase blendEase_ = CameraBlendEase::EaseInOut;
  CameraPose outPose_{};
};

/// Snapshot of the camera.* cvars, for per-frame config sync (the game's
/// cvarFloat/cvarInt read pattern): read once, apply to configs.
struct CameraCvarValues {
  int chunkRadiusChunks = 4;
  float defaultFovDeg = 70.f;
  float thirdPersonDistance = 6.f;
  float shakeScale = 1.f;
  bool headBob = false;
  bool fovKick = true;
};
/// Registers the camera.* cvars on a console registry (dotted scheme,
/// archived). False + `err` when a name is taken or invalid. Engine code
/// never owns a registry; the game calls this during its cvar setup.
bool registerCameraCvars(console::ConsoleRegistry& reg, std::string* err = nullptr);
/// Reads the current camera.* values back (defaults when unregistered).
CameraCvarValues readCameraCvars(const console::ConsoleRegistry& reg);
/// Applies a snapshot to a config (chunk radius, FOV, boom, shake, bob,
/// kick). Call after registerCameraCvars and whenever cvars may have moved.
void applyCameraCvars(const CameraCvarValues& v, CameraConfig& cfg);

}  // namespace litholattice::camera
