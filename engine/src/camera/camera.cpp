// Camera implementation. See camera.h for the design notes; the short
// version: exponential damping everywhere (frame-rate independent), spring
// arm pulls in instantly and eases out, shake is trauma^2 through smooth
// deterministic noise, and nothing here reads a clock or touches the sim.
#include "litholattice/camera/camera.h"

#include <algorithm>
#include <cmath>

namespace litholattice::camera {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.f;
constexpr float kMaxPitch = 1.55f;  // matches the game's player pitch clamp

Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float length(Vec3 a) { return std::sqrt(dot(a, a)); }
float clampF(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

/// Player::forward convention (game/player.cpp): yaw from +X toward +Z.
Vec3 dirFromYawPitch(float yaw, float pitch) {
  const float cp = std::cos(pitch);
  return {cp * std::cos(yaw), std::sin(pitch), cp * std::sin(yaw)};
}

float wrapPi(float a) {
  while (a > kPi) a -= 2.f * kPi;
  while (a < -kPi) a += 2.f * kPi;
  return a;
}

/// The frame-rate independent damp: exact solution of dx/dt = lambda*(t-x)
/// over the step. damp(x, target, lambda, dt) at 30 fps and 240 fps lands
/// on the same curve; lerp(x, target, lambda*dt) does not.
float damp(float cur, float target, float lambda, float dt) {
  return target + (cur - target) * std::exp(-lambda * dt);
}
Vec3 dampVec(Vec3 cur, Vec3 target, float lambda, float dt) {
  const float k = std::exp(-lambda * dt);
  return {target.x + (cur.x - target.x) * k, target.y + (cur.y - target.y) * k,
          target.z + (cur.z - target.z) * k};
}
float dampAngle(float cur, float target, float lambda, float dt) {
  return cur + wrapPi(target - cur) * (1.f - std::exp(-lambda * dt));
}

/// Deterministic smooth value noise in [-1, 1): hashed lattice points,
/// smootherstep interpolation. Same seed + time = same value on every
/// machine (integer hash, no rand(), no clock) — shake is presentation,
/// but free determinism is free.
float hashSigned(uint32_t n) {
  n ^= n >> 16;
  n *= 0x7feb352du;
  n ^= n >> 15;
  n *= 0x846ca68bu;
  n ^= n >> 16;
  return float((n >> 8) & 0xFFFFFFu) * (1.f / 8388608.f) - 1.f;  // [0,2) -> [-1,1)
}
float valueNoise(uint32_t seed, float t) {
  const float ft = std::floor(t);
  const auto i = static_cast<uint32_t>(static_cast<int32_t>(ft));
  const float f = t - ft;
  const float u = f * f * f * (f * (f * 6.f - 15.f) + 10.f);
  const float a = hashSigned(seed + i * 0x9E3779B9u);
  const float b = hashSigned(seed + (i + 1u) * 0x9E3779B9u);
  return a + (b - a) * u;
}

/// Orthonormal (forward, up) basis from a forward and a hint up, matching
/// lookDir's layout choices (right = normalize(cross(f, up))).
void orthonormalize(Vec3 fwd, Vec3 upHint, Vec3& outFwd, Vec3& outUp) {
  Vec3 f = normalize(fwd);
  Vec3 s = cross(f, upHint);
  if (dot(s, s) < 1e-10f) {  // forward parallel to the hint: pick another axis
    s = cross(f, {0.f, 0.f, 1.f});
    if (dot(s, s) < 1e-10f) s = {1.f, 0.f, 0.f};
  }
  s = normalize(s);
  outFwd = f;
  outUp = cross(s, f);
}

}  // namespace

// ---------------------------------------------------------------- Frustum

Frustum extractFrustum(const Mat4& vp) {
  // Same plane construction as core math's aabbInFrustum (Gribb-Hartmann on
  // rows of the combined matrix, column-major storage), then normalized so
  // signedDistance is a true distance.
  const float* m = vp.m;
  Frustum fr;
  for (int p = 0; p < 6; ++p) {
    const int row = p / 2;
    const float sgn = (p & 1) ? -1.f : 1.f;
    float a = m[3] + sgn * m[row], b = m[7] + sgn * m[4 + row];
    float c = m[11] + sgn * m[8 + row], d = m[15] + sgn * m[12 + row];
    const float inv = 1.f / std::max(std::sqrt(a * a + b * b + c * c), 1e-12f);
    fr.planes[p] = {a * inv, b * inv, c * inv, d * inv};
  }
  return fr;
}

bool Frustum::containsPoint(Vec3 p) const {
  for (const Plane& pl : planes)
    if (pl.signedDistance(p) < 0.f) return false;
  return true;
}

bool Frustum::intersectsAabb(Vec3 mn, Vec3 mx) const {
  for (const Plane& pl : planes) {
    // P-vertex: the corner farthest along the plane normal. If even it is
    // outside, the whole box is.
    const Vec3 v{pl.a >= 0 ? mx.x : mn.x, pl.b >= 0 ? mx.y : mn.y, pl.c >= 0 ? mx.z : mn.z};
    if (pl.signedDistance(v) < 0.f) return false;
  }
  return true;
}

// ---------------------------------------------------------------- Matrices

Mat4 cameraViewMatrix(const CameraPose& pose) {
  Vec3 f, u;
  orthonormalize(pose.forward, pose.up, f, u);
  const Vec3 s = normalize(cross(f, u));
  const Vec3 eye = pose.position;
  Mat4 r;
  r.m[0] = s.x;  r.m[4] = s.y;  r.m[8] = s.z;
  r.m[1] = u.x;  r.m[5] = u.y;  r.m[9] = u.z;
  r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
  r.m[12] = -dot(s, eye); r.m[13] = -dot(u, eye); r.m[14] = dot(f, eye); r.m[15] = 1.f;
  return r;
}

Mat4 cameraProjectionMatrix(float fovDeg, float aspect, float nearPlane, float farPlane) {
  const float clamped = clampF(fovDeg, 10.f, 120.f);
  return perspective(clamped * kDegToRad, aspect, nearPlane, farPlane);
}

// ---------------------------------------------------------------- Camera

Camera::Camera() {
  // Per-instance noise seed so two shaken cameras do not move in lockstep.
  static uint32_t s_counter = 0;
  shakeSeed_ = 0x51F15EEDu + (++s_counter) * 0x9E3779B9u;
  orbitDistance_ = cfg_.orbitDistance;
  followDistance_ = cfg_.followDistance;
  effectiveFovDeg_ = cfg_.fovDeg;
}

void Camera::seedFromTarget(const ICameraTarget& t) {
  yaw_ = t.facingYaw();
  pitch_ = clampF(t.facingPitch(), -kMaxPitch, kMaxPitch);
  orbitYaw_ = smoothOrbitYaw_ = yaw_;
  orbitPitch_ = smoothOrbitPitch_ = clampF(pitch_, cfg_.orbitPitchMin, cfg_.orbitPitchMax);
  smoothPivot_ = t.anchorPosition() + cfg_.pivotOffset;
  armLength_ = orbitDistance_;
  followSeeded_ = false;
  effectiveFovDeg_ = clampF(cfg_.fovDeg, 10.f, 120.f);
  stateSeeded_ = true;
}

void Camera::attach(std::weak_ptr<ICameraTarget> target) {
  target_ = std::move(target);
  stateSeeded_ = false;
  if (auto t = lockTarget()) seedFromTarget(*t);
}

void Camera::detach() {
  target_.reset();
  stateSeeded_ = false;
}

bool Camera::hasTarget() const { return !target_.expired(); }

void Camera::setMode(CameraMode mode) {
  if (mode == mode_) return;
  mode_ = mode;
  stateSeeded_ = false;
  if (auto t = lockTarget()) seedFromTarget(*t);
  if (mode_ == CameraMode::Fixed) fixedPos_ = basePose_.position;
  if (mode_ == CameraMode::Free) {
    freePos_ = basePose_.position;
    // Keep looking the way we were looking: re-derive yaw/pitch from the pose.
    yaw_ = std::atan2(basePose_.forward.z, basePose_.forward.x);
    pitch_ = std::asin(clampF(basePose_.forward.y, -1.f, 1.f));
  }
}

void Camera::addLookInput(float yawDeltaRad, float pitchDeltaRad) {
  sinceLookInputSec_ = 0.f;
  yaw_ += yawDeltaRad;
  pitch_ = clampF(pitch_ + pitchDeltaRad, -kMaxPitch, kMaxPitch);
  fpYawOffset_ += yawDeltaRad;
  fpPitchOffset_ = clampF(fpPitchOffset_ + pitchDeltaRad, -kMaxPitch, kMaxPitch);
  orbitYaw_ += yawDeltaRad;
  orbitPitch_ = clampF(orbitPitch_ + pitchDeltaRad, cfg_.orbitPitchMin, cfg_.orbitPitchMax);
}

void Camera::addZoomInput(float delta) {
  switch (mode_) {
    case CameraMode::ThirdPerson:
      orbitZoomed_ = true;  // CAM-3: a user zoom wins over the live config value from here on
      orbitDistance_ = clampF(orbitDistance_ - delta, cfg_.orbitMinDistance, cfg_.orbitMaxDistance);
      break;
    case CameraMode::Follow:
      followZoomed_ = true;  // CAM-3: same rule as the orbit boom
      followDistance_ = clampF(followDistance_ - delta, cfg_.orbitMinDistance, cfg_.orbitMaxDistance * 2.f);
      break;
    case CameraMode::Free:
      freePos_ = freePos_ + dirFromYawPitch(yaw_, pitch_) * (delta * cfg_.freeDollySpeed * 0.25f);
      if (!stateSeeded_) { /* free pose follows freePos_ directly */ }
      break;
    default:
      break;
  }
}

void Camera::snapToPose(const CameraPose& p) {
  basePose_ = p;
  pose_ = p;
  freePos_ = p.position;
  fixedPos_ = p.position;
  followPos_ = p.position;
  followSeeded_ = true;
  smoothPivot_ = p.position;
  yaw_ = std::atan2(p.forward.z, p.forward.x);
  pitch_ = std::asin(clampF(p.forward.y, -1.f, 1.f));
  orbitYaw_ = smoothOrbitYaw_ = yaw_;
  orbitPitch_ = smoothOrbitPitch_ = clampF(pitch_, cfg_.orbitPitchMin, cfg_.orbitPitchMax);
  stateSeeded_ = true;
}

void Camera::setPose(const CameraPose& pose) {
  CameraPose p = pose;
  orthonormalize(p.forward, p.up, p.forward, p.up);
  snapToPose(p);
}

void Camera::addTrauma(float amount) {
  trauma_ = clampF(trauma_ + amount, 0.f, 1.f);
}

void Camera::update(float dt, const ICameraWorld& world) {
  dt = clampF(dt, 0.f, 5.f);  // a hitching frame must not teleport the noise clock
  sinceLookInputSec_ += dt;
  // CAM-3: config distances are LIVE until the user zooms. The cvar path
  // (applyCameraCvars) writes cfg_.orbitDistance every frame, but the
  // working distances were copied from cfg only in the constructor, so
  // `camera.third_person_distance` never moved an existing camera. Sync
  // here — before either mode reads its distance — unless addZoomInput
  // has claimed that distance for the user.
  if (!orbitZoomed_) orbitDistance_ = cfg_.orbitDistance;
  if (!followZoomed_) followDistance_ = cfg_.followDistance;
  const auto target = lockTarget();

  CameraPose base = basePose_;  // default: hold the last pose (dead target / no target)
  bool haveBase = stateSeeded_ || target != nullptr;

  if (target && !stateSeeded_) seedFromTarget(*target);

  if (target) {
    const Vec3 anchor = target->anchorPosition();
    const Vec3 vel = target->velocity();
    switch (mode_) {
      case CameraMode::FirstPerson: {
        Vec3 eye = anchor + target->eyeOffset();
        if (cfg_.headBob) {
          // Phase advances with distance travelled, not wall time, so the
          // bob rate follows the stride; amplitude fades in with speed.
          const float hSpeed = std::sqrt(vel.x * vel.x + vel.z * vel.z);
          bobPhase_ += hSpeed * dt * cfg_.headBobFrequency;
          const float fade = clampF(hSpeed / 6.f, 0.f, 1.f);
          eye.y += std::sin(bobPhase_) * cfg_.headBobAmplitude * fade;
          eye.x += std::cos(bobPhase_ * 0.5f) * cfg_.headBobAmplitude * 0.4f * fade;
        }
        base.position = eye;
        // Orientation = the target's facing + this camera's own look
        // offset. The game drives look through the target (mouse -> player
        // yaw), so with zero offset the camera tracks it exactly; an agent
        // driving the camera directly accumulates an offset that rides
        // along when the target turns.
        const float fpYaw = target->facingYaw() + fpYawOffset_;
        const float fpPitch = clampF(target->facingPitch() + fpPitchOffset_, -kMaxPitch, kMaxPitch);
        Vec3 f, u;
        orthonormalize(dirFromYawPitch(fpYaw, fpPitch), {0.f, 1.f, 0.f}, f, u);
        base.forward = f;
        base.up = u;
        break;
      }
      case CameraMode::ThirdPerson: {
        // Auto-align: drift the boom behind the target's facing once the
        // player has been hands-off long enough and is actually moving.
        const float hSpeed = std::sqrt(vel.x * vel.x + vel.z * vel.z);
        if (cfg_.autoAlign && sinceLookInputSec_ > cfg_.autoAlignDelaySec && hSpeed > 1.f)
          orbitYaw_ = dampAngle(orbitYaw_, target->facingYaw(), cfg_.autoAlignRate, dt);
        smoothOrbitYaw_ = dampAngle(smoothOrbitYaw_, orbitYaw_, cfg_.rotationDamping, dt);
        smoothOrbitPitch_ = damp(smoothOrbitPitch_, orbitPitch_, cfg_.rotationDamping, dt);
        const Vec3 pivotTarget = anchor + cfg_.pivotOffset;
        smoothPivot_ = dampVec(smoothPivot_, pivotTarget, cfg_.positionDamping, dt);

        const Vec3 fwd = dirFromYawPitch(smoothOrbitYaw_, smoothOrbitPitch_);
        Vec3 right = cross(fwd, {0.f, 1.f, 0.f});
        right = dot(right, right) < 1e-10f ? Vec3{1.f, 0.f, 0.f} : normalize(right);
        const Vec3 pivot = smoothPivot_ + right * cfg_.shoulderOffset.x +
                           Vec3{0.f, 1.f, 0.f} * cfg_.shoulderOffset.y;
        // Spring arm: cast the boom; a hit shortens it NOW (never clip),
        // freedom lets it ease back out (no pop). Probe radius rides on top.
        const Vec3 boomDir = fwd * -1.f;
        float allowed = orbitDistance_;
        const CameraRayHit hit = world.castRay(pivot, boomDir, orbitDistance_ + cfg_.probeRadius);
        if (hit.hit) allowed = std::min(allowed, std::max(0.f, hit.distance - cfg_.probeRadius));
        if (allowed < armLength_)
          armLength_ = allowed;  // pull-in is a safety response: instant
        else
          armLength_ = damp(armLength_, allowed, 6.f, dt);  // ease-out rate
        base.position = pivot + boomDir * armLength_;
        Vec3 f, u;
        orthonormalize(sub(smoothPivot_, base.position), {0.f, 1.f, 0.f}, f, u);
        base.forward = f;
        base.up = u;
        break;
      }
      case CameraMode::Follow: {
        if (!followSeeded_) {
          smoothOrbitYaw_ = target->facingYaw();
          followPos_ = sub(anchor, dirFromYawPitch(smoothOrbitYaw_, 0.f) * followDistance_) +
                       Vec3{0.f, cfg_.followHeight, 0.f};
          followSeeded_ = true;
        }
        // Chase a smoothed heading (velocity direction beats facing when
        // strafing), from behind, at a lagged position.
        const float hSpeed = std::sqrt(vel.x * vel.x + vel.z * vel.z);
        float desiredYaw = smoothOrbitYaw_;
        if (hSpeed > 0.5f) desiredYaw = std::atan2(vel.z, vel.x);
        smoothOrbitYaw_ = dampAngle(smoothOrbitYaw_, desiredYaw, 3.f, dt);
        const Vec3 desired = sub(anchor, dirFromYawPitch(smoothOrbitYaw_, 0.f) * followDistance_) +
                             Vec3{0.f, cfg_.followHeight, 0.f};
        followPos_ = dampVec(followPos_, desired, cfg_.followLag, dt);
        base.position = followPos_;
        const Vec3 lookAt = anchor + Vec3{0.f, 1.5f, 0.f} + vel * cfg_.followLookAheadSec;
        Vec3 f, u;
        orthonormalize(sub(lookAt, base.position), {0.f, 1.f, 0.f}, f, u);
        base.forward = f;
        base.up = u;
        break;
      }
      case CameraMode::Fixed: {
        base.position = fixedPos_;
        const Vec3 aim = anchor + target->eyeOffset();
        Vec3 f, u;
        orthonormalize(sub(aim, base.position), {0.f, 1.f, 0.f}, f, u);
        base.forward = f;
        base.up = u;
        break;
      }
      case CameraMode::Free:
        break;  // handled below (works with or without a target)
    }
  }
  if (mode_ == CameraMode::Free) {
    base.position = freePos_;
    Vec3 f, u;
    orthonormalize(dirFromYawPitch(yaw_, pitch_), {0.f, 1.f, 0.f}, f, u);
    base.forward = f;
    base.up = u;
    haveBase = true;
  } else if (mode_ == CameraMode::Fixed && !target) {
    base.position = fixedPos_;
    Vec3 f, u;
    if (fixedLookAtSet_)
      orthonormalize(sub(fixedLookAt_, base.position), {0.f, 1.f, 0.f}, f, u);
    else
      orthonormalize(dirFromYawPitch(yaw_, pitch_), {0.f, 1.f, 0.f}, f, u);
    base.forward = f;
    base.up = u;
    haveBase = true;
  }
  (void)haveBase;

  // FOV: base (clamped) + speed kick, damped so the kick breathes instead
  // of snapping. Kick reads the target's speed in FP/TP/Follow only.
  float fovTarget = clampF(cfg_.fovDeg, 10.f, 120.f);
  if (cfg_.fovKick && target && mode_ != CameraMode::Free && mode_ != CameraMode::Fixed) {
    const Vec3 vel = target->velocity();
    const float speed = length(vel);
    fovTarget += cfg_.fovKickMaxDeg * clampF(speed / std::max(cfg_.fovKickReferenceSpeed, 1e-6f), 0.f, 1.f);
  }
  effectiveFovDeg_ = damp(effectiveFovDeg_, fovTarget, 8.f, dt);
  base.fovDeg = effectiveFovDeg_;
  basePose_ = base;

  // Trauma shake (rotational first, per the research notes): offsets from
  // smooth noise, scaled by trauma^2, applied to a COPY — basePose_ stays
  // clean so shake can never accumulate into drift.
  pose_ = base;
  shakeTime_ += dt * std::max(cfg_.shakeFrequency, 0.01f);
  trauma_ = std::max(0.f, trauma_ - cfg_.shakeDecayPerSec * dt);
  const float shake = trauma_ * trauma_ * cfg_.shakeScale;
  if (shake > 1e-6f) {
    const float yawOff = cfg_.shakeMaxYawDeg * kDegToRad * shake * valueNoise(shakeSeed_ + 11u, shakeTime_);
    const float pitchOff = cfg_.shakeMaxPitchDeg * kDegToRad * shake * valueNoise(shakeSeed_ + 23u, shakeTime_);
    const float roll = cfg_.shakeMaxRollDeg * kDegToRad * shake * valueNoise(shakeSeed_ + 37u, shakeTime_);
    const float offX = cfg_.shakeMaxOffset * shake * valueNoise(shakeSeed_ + 41u, shakeTime_);
    const float offY = cfg_.shakeMaxOffset * shake * valueNoise(shakeSeed_ + 53u, shakeTime_);
    Vec3 f = pose_.forward, u = pose_.up;
    Vec3 right = normalize(cross(f, u));
    Vec3 trueUp = cross(right, f);
    Vec3 f2 = normalize(f + right * yawOff + trueUp * pitchOff);  // small-angle rotation
    Vec3 right2 = normalize(cross(f2, trueUp));
    Vec3 up2 = cross(right2, f2);
    const float cr = std::cos(roll), sr = std::sin(roll);
    pose_.up = normalize(up2 * cr + right2 * sr);
    pose_.forward = f2;
    pose_.position = pose_.position + right2 * offX + up2 * offY;
  }
}

Mat4 Camera::viewMatrix() const { return cameraViewMatrix(pose_); }

Mat4 Camera::projectionMatrix() const {
  return cameraProjectionMatrix(pose_.fovDeg, cfg_.aspect, cfg_.nearPlane, cfg_.farPlane);
}

Mat4 Camera::viewProjectionMatrix() const { return mul(projectionMatrix(), viewMatrix()); }

}  // namespace litholattice::camera
