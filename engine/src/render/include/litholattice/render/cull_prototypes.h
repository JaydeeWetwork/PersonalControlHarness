#pragma once
// Frustum occlusion culling PROTOTYPES (JD 2026-10-11): three experimental
// cache layers on top of the base FOV-margin culling (cvar
// `render.frustum_margin_deg`, the default-on foundation). Each prototype is
// independently toggled by its own cvar; all default OFF. JD A/B tests them
// on his workstation; the base margin culling is untouched by all three.
//
// Wiring: renderers own a CullProtoState, call beginFrame() once per frame,
// then decide() per chunk. The pure-logic helpers (CometTail::visible,
// directionalMarginDeg, classifyTwoTier) are unit-tested in engine_tests.cpp.
#include <cmath>
#include <cstddef>
#include "litholattice/core/math.h"

namespace litholattice {

// Per-chunk decision under the prototype pipeline.
enum class CullDecision { Cull, MeshOnly, Full };

// Toggle set for the three prototypes (cvar-backed; see settings_cvars.cpp).
struct CullProtoConfig {
  int cometTailFrames = 0;        ///< render.comet_tail_frames (0 = off)
  bool velocityMargin = false;    ///< render.velocity_margin
  float velocityMarginGain = 1.f;  ///< render.velocity_margin_gain (deg per rad/s)
  bool twoTier = false;           ///< render.two_tier
  float twoTierOuterDeg = 10.f;   ///< render.two_tier_outer_deg
};

struct CullProtoStats {
  uint32_t full = 0;       ///< chunks fully submitted (drawn)
  uint32_t meshOnly = 0;   ///< chunks mesh-kept but not drawn (two-tier outer)
  uint32_t culled = 0;     ///< chunks fully culled
  uint32_t cometKept = 0;  ///< of full: kept alive by the comet tail
};

// ---------------------------------------------------------------------------
// Frustum occlusion culling method: Comet tail (temporal frustum union)
// Designed and discovered by Elfie (JD's AI collaborator), 2026-10-11,
// from JD's caching note (culled chunks stay resident and sim-ticked).
// Noted in the project history books per JD.
//
// Idea: the FOV margin pads *where* the camera looks; the comet tail pads
// *when*. Keep the last N camera frusta; a chunk inside ANY of them stays
// submitted. Fast continuous rotation reveals already-rendered chunks
// because the frustum *was* there a few frames ago. Directional history,
// not a symmetric shell: it remembers where you looked.
// Limitation (known): an instant 180-degree snap defeats prediction — the
// new view was never in history. JD's residency note bounds the damage:
// the snap costs re-submission of resident data, never a reload.
// ---------------------------------------------------------------------------
class CometTail {
 public:
  static constexpr int kMaxFrames = 64;

  void configure(int frames) {
    frames_ = frames < 0 ? 0 : (frames > kMaxFrames ? kMaxFrames : frames);
    count_ = 0;
    havePrev_ = false;
  }
  int frames() const { return frames_; }
  int stored() const { return count_; }

  // Push the new frame's viewProj. History holds strictly *past* frames: the
  // caller's current-frustum test runs separately, so the first beginFrame
  // only arms the previous-frame slot.
  void beginFrame(const Mat4& vp) {
    if (frames_ <= 0) return;
    if (havePrev_) {
      history_[head_] = prevVp_;
      head_ = (head_ + 1) % kMaxFrames;
      if (count_ < frames_) ++count_;
    }
    prevVp_ = vp;
    havePrev_ = true;
  }

  // True if the box is inside any remembered frustum, each tested with the
  // base margin. camPos feeds the margin's distance term (approximation for
  // past frames: the planes matter, the expansion distance is secondary).
  bool visible(const Vec3& mn, const Vec3& mx, const Vec3& camPos,
               float baseMarginDeg) const {
    for (int i = 0; i < count_; ++i) {
      const int idx = (head_ - 1 - i + 2 * kMaxFrames) % kMaxFrames;
      if (aabbInFrustumMargin(history_[idx], mn, mx, camPos, baseMarginDeg)) return true;
    }
    return false;
  }

 private:
  int frames_ = 0;
  int count_ = 0, head_ = 0;
  bool havePrev_ = false;
  Mat4 prevVp_{};
  Mat4 history_[kMaxFrames];
};

// ---------------------------------------------------------------------------
// Frustum occlusion culling method: Lean into the turn (velocity-adaptive margin)
// Designed and discovered by Elfie (JD's AI collaborator), 2026-10-11,
// from JD's caching note (culled chunks stay resident and sim-ticked).
// Noted in the project history books per JD.
//
// Idea: the static margin is symmetric and forgetful; a turning camera is
// neither. Track smoothed yaw/pitch angular velocity and stretch the margin
// asymmetrically — wider in the direction of rotation, narrower behind. A
// still camera degrades exactly to the base margin. The margin breathes
// with the input.
// ---------------------------------------------------------------------------
class VelocityTracker {
 public:
  void reset() {
    yawVel_ = 0.f;
    pitchVel_ = 0.f;
    havePrev_ = false;
  }

  // yaw/pitch in radians (Player::forward convention), time in seconds
  // (monotonic). First sample or a non-monotonic clock arms without emitting
  // velocity; yaw wrap at +/-pi is handled.
  void update(float yawRad, float pitchRad, double timeSec) {
    if (!havePrev_ || !(timeSec > prevT_)) {
      prevYaw_ = yawRad;
      prevPitch_ = pitchRad;
      prevT_ = timeSec;
      havePrev_ = true;
      return;
    }
    const float dt = float(timeSec - prevT_);
    if (dt > 1e-6f) {
      const float dyaw = wrapPi(yawRad - prevYaw_) / dt;
      const float dpitch = (pitchRad - prevPitch_) / dt;
      // Exponential smoothing, ~100ms time constant: steadies mouse jitter
      // without lagging real turns.
      const float alpha = dt / (dt + 0.1f);
      yawVel_ += alpha * (dyaw - yawVel_);
      pitchVel_ += alpha * (dpitch - pitchVel_);
      prevYaw_ = yawRad;
      prevPitch_ = pitchRad;
      prevT_ = timeSec;
    }
  }

  float yawVel() const { return yawVel_; }      // rad/s, + = turning right
  float pitchVel() const { return pitchVel_; }  // rad/s, + = looking up

 private:
  static float wrapPi(float a) {
    constexpr float kPi = 3.14159265358979323846f;
    while (a > kPi) a -= 2.f * kPi;
    while (a < -kPi) a += 2.f * kPi;
    return a;
  }
  float yawVel_ = 0.f, pitchVel_ = 0.f;
  float prevYaw_ = 0.f, prevPitch_ = 0.f;
  double prevT_ = 0.0;
  bool havePrev_ = false;
};

// Camera basis from yaw/pitch (same convention as Player::forward()).
inline Vec3 cullCamForward(float yawRad, float pitchRad) {
  const float cp = std::cos(pitchRad);
  return Vec3{cp * std::cos(yawRad), std::sin(pitchRad), cp * std::sin(yawRad)};
}
// right = forward x worldUp, projected to horizontal (stable at high pitch).
inline Vec3 cullCamRight(float yawRad, float pitchRad) {
  const Vec3 f = cullCamForward(yawRad, pitchRad);
  Vec3 r{-f.z, 0.f, f.x};
  const float l = std::sqrt(r.x * r.x + r.z * r.z);
  if (l < 1e-6f) return Vec3{1.f, 0.f, 0.f};  // looking straight up/down
  return Vec3{r.x / l, 0.f, r.z / l};
}
inline Vec3 cullCamUp(float yawRad, float pitchRad) {
  const Vec3 f = cullCamForward(yawRad, pitchRad);
  const Vec3 r = cullCamRight(yawRad, pitchRad);
  // up = right x forward
  return Vec3{r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x};
}

// Per-chunk effective margin under the velocity prototype.
// chunkDir/camRight/camUp are unit vectors; gain is degrees of extra margin
// per rad/s of angular velocity. Turning right (yawVel > 0) widens the
// margin for chunks toward camRight and narrows it behind; still camera
// (|vel| ~ 0) returns exactly baseDeg. Clamped to [0, 30].
inline float directionalMarginDeg(float baseDeg, float yawVel, float pitchVel,
                                  float gain, const Vec3& chunkDir,
                                  const Vec3& camRight, const Vec3& camUp) {
  const float alignYaw = chunkDir.x * camRight.x + chunkDir.y * camRight.y +
                         chunkDir.z * camRight.z;
  const float alignPitch = chunkDir.x * camUp.x + chunkDir.y * camUp.y +
                           chunkDir.z * camUp.z;
  float m = baseDeg + gain * (yawVel * alignYaw + pitchVel * alignPitch);
  if (m < 0.f) m = 0.f;
  if (m > 30.f) m = 30.f;
  return m;
}

// ---------------------------------------------------------------------------
// Frustum occlusion culling method: Two-tier margin (mesh vs. submit)
// Designed and discovered by Elfie (JD's AI collaborator), 2026-10-11,
// from JD's caching note (culled chunks stay resident and sim-ticked).
// Noted in the project history books per JD.
//
// Idea: split the margin shell into two tiers. Inner tier = full render
// (today's behavior). Outer tier = build and KEEP the chunk mesh but skip
// draw submission. Rotating into the outer tier costs only draw calls —
// never a mesh rebuild. Residency and submission become separate dials.
// (In the GL/VK backends meshes are never visibility-evicted today, so
// MeshOnly currently means "skip the draw, mesh stays uploaded" — the
// classification, stats, and pin-against-future-eviction are the prototype.)
// ---------------------------------------------------------------------------
inline CullDecision classifyTwoTier(const Mat4& vp, const Vec3& mn, const Vec3& mx,
                                    const Vec3& camPos, float innerMarginDeg,
                                    float outerMarginDeg) {
  if (aabbInFrustumMargin(vp, mn, mx, camPos, innerMarginDeg)) return CullDecision::Full;
  if (outerMarginDeg > innerMarginDeg &&
      aabbInFrustumMargin(vp, mn, mx, camPos, outerMarginDeg))
    return CullDecision::MeshOnly;
  return CullDecision::Cull;
}

// Per-renderer (GL/VK) or per-camera (vision) prototype state. Owns the comet
// history and the velocity tracker; composes the three prototypes with the
// base margin: inner test (velocity-adjusted or base) -> two-tier outer ->
// comet-tail history -> cull.
class CullProtoState {
 public:
  void configure(const CullProtoConfig& c) {
    // Reconfigure sub-objects only on real change: the comet history and the
    // velocity smoother survive cvar reads that change nothing.
    if (c.cometTailFrames != cfg_.cometTailFrames) comet_.configure(c.cometTailFrames);
    if (c.velocityMargin != cfg_.velocityMargin && !c.velocityMargin) vel_.reset();
    cfg_ = c;
  }
  const CullProtoConfig& config() const { return cfg_; }

  // Once per frame, before the chunk loop. yaw/pitch in radians.
  void beginFrame(const Mat4& vp, float yawRad, float pitchRad, double timeSec) {
    comet_.beginFrame(vp);
    if (cfg_.velocityMargin) vel_.update(yawRad, pitchRad, timeSec);
    lastYaw_ = yawRad;
    lastPitch_ = pitchRad;
    stats_ = CullProtoStats{};
  }

  // Per chunk. baseMarginDeg is render.frustum_margin_deg (the foundation).
  CullDecision decide(const Mat4& vp, const Vec3& mn, const Vec3& mx,
                      const Vec3& camPos, float baseMarginDeg) {
    float m = baseMarginDeg;
    if (cfg_.velocityMargin) {
      const Vec3 c{(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f};
      Vec3 d{c.x - camPos.x, c.y - camPos.y, c.z - camPos.z};
      const float l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
      if (l > 1e-6f) {
        d.x /= l;
        d.y /= l;
        d.z /= l;
      } else {
        d = Vec3{0.f, 0.f, 1.f};
      }
      m = directionalMarginDeg(baseMarginDeg, vel_.yawVel(), vel_.pitchVel(),
                               cfg_.velocityMarginGain, d,
                               cullCamRight(lastYaw_, lastPitch_),
                               cullCamUp(lastYaw_, lastPitch_));
    }
    if (aabbInFrustumMargin(vp, mn, mx, camPos, m)) {
      ++stats_.full;
      return CullDecision::Full;
    }
    if (cfg_.twoTier) {
      const CullDecision t =
          classifyTwoTier(vp, mn, mx, camPos, m, m + cfg_.twoTierOuterDeg);
      if (t == CullDecision::MeshOnly) {
        ++stats_.meshOnly;
        return CullDecision::MeshOnly;
      }
    }
    if (cfg_.cometTailFrames > 0 && comet_.visible(mn, mx, camPos, baseMarginDeg)) {
      ++stats_.full;
      ++stats_.cometKept;
      return CullDecision::Full;
    }
    ++stats_.culled;
    return CullDecision::Cull;
  }

  CullProtoStats stats() const { return stats_; }

 private:
  CullProtoConfig cfg_;
  CometTail comet_;
  VelocityTracker vel_;
  float lastYaw_ = 0.f, lastPitch_ = 0.f;
  CullProtoStats stats_;
};

}  // namespace litholattice
