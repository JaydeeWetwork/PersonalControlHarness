// CameraManager: the "brain" — named virtual cameras, timed eased blends,
// one live output pose. See camera.h for the design notes.
#include "litholattice/camera/camera.h"

#include <algorithm>
#include <cmath>

namespace litholattice::camera {
namespace {

float clamp01(float t) { return std::min(std::max(t, 0.f), 1.f); }
Vec3 lerpVec(Vec3 a, Vec3 b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

CameraPose blendPoses(const CameraPose& a, const CameraPose& b, float t) {
  if (t <= 0.f) return a;
  if (t >= 1.f) return b;
  CameraPose out;
  out.position = lerpVec(a.position, b.position, t);
  Vec3 f = lerpVec(a.forward, b.forward, t);
  Vec3 u = lerpVec(a.up, b.up, t);
  // Lerp + orthonormalize (nlerp): fine for blend arcs, and it keeps the
  // house math (no quaternion type in core math to slerp with).
  if (dot(f, f) < 1e-10f) f = b.forward;
  f = normalize(f);
  Vec3 s = cross(f, u);
  if (dot(s, s) < 1e-10f) s = cross(f, b.up);
  s = normalize(s);
  out.forward = f;
  out.up = cross(s, f);
  out.fovDeg = a.fovDeg + (b.fovDeg - a.fovDeg) * t;
  return out;
}

}  // namespace

float cameraBlendFactor(CameraBlendEase ease, float t) {
  t = clamp01(t);
  switch (ease) {
    case CameraBlendEase::Cut: return 1.f;
    case CameraBlendEase::Linear: return t;
    case CameraBlendEase::EaseIn: return t * t;
    case CameraBlendEase::EaseOut: return 1.f - (1.f - t) * (1.f - t);
    case CameraBlendEase::EaseInOut: return t * t * (3.f - 2.f * t);
    case CameraBlendEase::HardIn: return t * t * t;
    case CameraBlendEase::HardOut: {
      const float u = 1.f - t;
      return 1.f - u * u * u;
    }
  }
  return t;
}

bool CameraManager::addCamera(const std::string& name, std::shared_ptr<ICamera> cam) {
  if (name.empty() || !cam) return false;
  const bool first = cameras_.empty();
  cameras_[name] = Entry{std::move(cam)};
  if (first && activeName_.empty()) {  // a lone camera is live by default
    activeName_ = name;
    outPose_ = cameras_[name].cam->pose();
  }
  return true;
}

void CameraManager::removeCamera(const std::string& name) {
  cameras_.erase(name);
  if (name == blendFromName_) blendFromName_.clear();
  if (name == activeName_) {
    activeName_.clear();
    blending_ = false;  // output holds its last pose
  }
}

ICamera* CameraManager::camera(const std::string& name) {
  const auto it = cameras_.find(name);
  return it == cameras_.end() ? nullptr : it->second.cam.get();
}
const ICamera* CameraManager::camera(const std::string& name) const {
  const auto it = cameras_.find(name);
  return it == cameras_.end() ? nullptr : it->second.cam.get();
}

bool CameraManager::setActive(const std::string& name, float blendSeconds, CameraBlendEase ease) {
  const auto it = cameras_.find(name);
  if (it == cameras_.end()) return false;
  if (name == activeName_ && !blending_) return true;  // already live
  blendFromName_ = activeName_;
  blendFromPose_ = outPose_;
  activeName_ = name;
  blendElapsed_ = 0.f;
  blendDuration_ = blendSeconds;
  blendEase_ = ease;
  blending_ = blendSeconds > 0.f && ease != CameraBlendEase::Cut && !blendFromName_.empty();
  if (!blending_) outPose_ = it->second.cam->pose();
  return true;
}

void CameraManager::update(float dt, const ICameraWorld& world) {
  // Every registered camera advances: damping state stays warm, so a blend
  // target never swoops in from wherever it was left.
  for (auto& [name, entry] : cameras_) entry.cam->update(dt, world);

  const ICamera* active = camera(activeName_);
  if (!active) return;  // output holds
  if (!blending_) {
    outPose_ = active->pose();
    return;
  }
  blendElapsed_ += dt;
  const float t = blendDuration_ > 0.f ? clamp01(blendElapsed_ / blendDuration_) : 1.f;
  // Live blend: the source pose is the source camera's CURRENT pose when it
  // is still registered (it keeps tracking its target mid-blend); the
  // captured snapshot is the fallback for a removed source.
  CameraPose from = blendFromPose_;
  if (const ICamera* src = camera(blendFromName_)) from = src->pose();
  outPose_ = blendPoses(from, active->pose(), cameraBlendFactor(blendEase_, t));
  if (t >= 1.f) {
    blending_ = false;
    outPose_ = active->pose();
  }
}

Mat4 CameraManager::projectionMatrix() const {
  if (const ICamera* active = camera(activeName_)) {
    const CameraConfig& c = active->config();
    return cameraProjectionMatrix(outPose_.fovDeg, c.aspect, c.nearPlane, c.farPlane);
  }
  return cameraProjectionMatrix(outPose_.fovDeg, 16.f / 9.f, 0.05f, 1000.f);
}

int CameraManager::chunkLoadRadiusChunks() const {
  if (const ICamera* active = camera(activeName_)) return active->chunkLoadRadiusChunks();
  return 0;
}

}  // namespace litholattice::camera
