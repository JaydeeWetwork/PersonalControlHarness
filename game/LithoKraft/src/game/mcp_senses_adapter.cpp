// McpSensesAdapter: game-side MCP pilot-senses adapters (see the header for
// the threading model, the downgrade ladder, and the chunk-loading note).
#include "lithokraft/game/mcp_senses_adapter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>

#include "litholattice/core/math.h"
#include "litholattice/world/chunk.h"  // packPos

namespace lithokraft {
namespace {

using litholattice::mcp::AudioChunk;
using litholattice::mcp::CameraPose;
using litholattice::mcp::VisionFrame;

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
constexpr uint32_t kSkyClear = 0xFF94B3DB;  // wafer day sky, from Game::frameParams (0xAARRGGBB)
constexpr int kMinDim = 16, kMaxDim = 512;
constexpr float kNear = 0.05f, kFar = 1000.0f;  // same clip range as the game camera
constexpr float kTapRate = 48000.0f;            // AudioEngine renders at 48 kHz
// Largest single drain: tier 0 needs 1024 * (48000/8000) tap frames.
constexpr size_t kTapScratchFrames =
    litholattice::mcp::kAudioChunkFrames * size_t(kTapRate / litholattice::mcp::kAudioTierRates[0]);

litholattice::Mat4 translation(float x, float y, float z) {
  litholattice::Mat4 t = litholattice::Mat4::identity();
  t.m[12] = x;
  t.m[13] = y;
  t.m[14] = z;
  return t;
}

}  // namespace

McpSensesAdapter::McpSensesAdapter(litholattice::AudioEngine& audio,
                                   std::function<McpPlayerView::GameState()> snapshot,
                                   std::function<uint64_t()> tick,
                                   std::shared_ptr<PilotCameraRig> rig)
    : audio_(audio), snapshot_(std::move(snapshot)), tick_(std::move(tick)), rig_(std::move(rig)) {
  tapScratch_.resize(kTapScratchFrames);
  audio_.setTapEnabled(true);  // arm the mixed-output tap once; the tap hears the pilot's ears
}

McpSensesAdapter::~McpSensesAdapter() { audio_.setTapEnabled(false); }

void McpSensesAdapter::noteMesh(const litholattice::ChunkMesh& m) {
  const int64_t k = litholattice::packPos(m.cx, m.sy, m.cz);
  std::lock_guard<std::mutex> lk(mutex_);
  if (m.verts.empty()) {
    meshes_.erase(k);  // mesh removed
    return;
  }
  auto cm = std::make_shared<CachedMesh>();
  cm->cx = m.cx;
  cm->sy = m.sy;
  cm->cz = m.cz;
  // Winding: none needed. The mesher winds quads CCW seen from outside (what
  // GL's glFrontFace(GL_CCW) keeps); the 2026-10-11 isBackface fix made the
  // software renderer cull the same faces, so the old v1<->v3 reversal
  // workaround was removed (it would now double-invert and hide every front
  // face from the agent). Verbatim copy.
  cm->verts = m.verts;  // no reversal: isBackface now matches GL's CCW convention
  meshes_[k] = std::move(cm);
}

void McpSensesAdapter::clearMeshes() {
  std::lock_guard<std::mutex> lk(mutex_);
  meshes_.clear();
  spriteVerts_.clear();
}

void McpSensesAdapter::setSpriteVerts(std::vector<litholattice::PackedVertex> verts,
                                       litholattice::Vec3 origin) {
  std::lock_guard<std::mutex> lk(mutex_);
  spriteVerts_ = std::move(verts);
  spriteOrigin_ = origin;
}

void McpSensesAdapter::syncCvars(const CvarState& s) {
  std::lock_guard<std::mutex> lk(mutex_);
  cvars_ = s;
}

McpSensesAdapter::CvarState McpSensesAdapter::cvars() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return cvars_;
}

std::vector<std::string> McpSensesAdapter::pilotIds() const { return {"player"}; }

bool McpSensesAdapter::captureVisionFrame(std::string_view pilot, int width, int height,
                                          VisionFrame& out) {
  // The pilot's engine camera (PilotCameraRig) resolves the capture pose on
  // the game thread; an unknown pilot — or one whose camera has not
  // resolved yet (no frame ticked) — fails, as an unknown pilot did before.
  CameraViewPose res;
  if (!rig_ || !rig_->resolvedPose(pilot, res)) return false;

  // Snapshot the shared state under one lock: cvars and a shallow
  // (shared_ptr) copy of the mesh cache. The entries are immutable, so the
  // render below sees a consistent world without blocking the game thread.
  CvarState cv;
  std::unordered_map<int64_t, std::shared_ptr<const CachedMesh>> meshes;
  std::vector<litholattice::PackedVertex> spriteVerts;
  litholattice::Vec3 spriteOrigin;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    cv = cvars_;
    meshes = meshes_;
    spriteVerts = spriteVerts_;
    spriteOrigin = spriteOrigin_;
  }

  const int w = std::clamp(width > 0 ? width : cv.visionWidth, kMinDim, kMaxDim);
  const int h = std::clamp(height > 0 ? height : cv.visionHeight, kMinDim, kMaxDim);

  // Eye / orientation / FOV from the engine camera's resolved pose
  // (radians internally; VisionFrame reports degrees, as before).
  const double ex = res.position.x, ey = res.position.y, ez = res.position.z;
  const float yawDeg = float(res.yaw * kRadToDeg), pitchDeg = float(res.pitch * kRadToDeg);
  const float fovDeg = float(res.fovRad * kRadToDeg);
  const float yawR = res.yaw, pitchR = res.pitch;
  // The engine camera's forward (Player::forward convention, radians).
  const litholattice::Vec3 eye{float(ex), float(ey), float(ez)};
  const litholattice::Vec3 fwd = res.forward;
  const litholattice::Mat4 vp =
      litholattice::mul(litholattice::perspective(float(std::clamp(fovDeg, 10.0f, 120.0f) * kDegToRad),
                                                 float(w) / float(h), kNear, kFar),
                        litholattice::lookDir(eye, fwd));

  {
    std::lock_guard<std::mutex> rlk(renderMutex_);
    sw_.setPerPixelShading(false);  // flat = the downgraded path
    sw_.resize(uint32_t(w), uint32_t(h));
    sw_.clear(kSkyClear);  // beyond loaded chunks: sky, not a crash
    // Culling-cache prototypes (JD 2026-10-11): pass 1 classifies every mesh
    // under the prototype pipeline (per-pilot state, its own mutex — the game
    // thread never blocks on this); pass 2 renders the survivors. Base FOV
    // margin is the foundation; the prototypes only add keep-behavior.
    const double nowSec = std::chrono::duration<double>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    std::vector<std::shared_ptr<const CachedMesh>> drawList;
    drawList.reserve(meshes.size());
    {
      std::lock_guard<std::mutex> plk(protoMutex_);
      auto& st = protoStates_[std::string(pilot)];  // per-pilot camera state
      litholattice::CullProtoConfig cfg;
      cfg.cometTailFrames = cv.cometTailFrames;
      cfg.velocityMargin = cv.velocityMargin;
      cfg.velocityMarginGain = cv.velocityMarginGain;
      cfg.twoTier = cv.twoTier;
      cfg.twoTierOuterDeg = cv.twoTierOuterDeg;
      st.configure(cfg);  // no-op when unchanged (history/velocity survive)
      st.beginFrame(vp, yawR, pitchR, nowSec);
      for (const auto& [k, cm] : meshes) {
        // Sub-chunk world origin (16^3), the same offset the GL backend
        // uploads as its per-mesh uniform; baked into the MVP here.
        const litholattice::Vec3 mn{float(cm->cx * 16), float(cm->sy * 16), float(cm->cz * 16)};
        if (st.decide(vp, mn, {mn.x + 16.0f, mn.y + 16.0f, mn.z + 16.0f}, eye,
                      cv.frustumMarginDeg) == litholattice::CullDecision::Full)
          drawList.push_back(cm);
      }
    }
    for (const auto& cm : drawList) {
      const litholattice::Vec3 mn{float(cm->cx * 16), float(cm->sy * 16), float(cm->cz * 16)};
      const litholattice::Mat4 mvp = litholattice::mul(vp, translation(mn.x, mn.y, mn.z));
      sw_.renderMesh(cm->verts.data(), cm->verts.size(), mvp.m);
    }
    // Sprites (flat form fed by Game's SpriteHost): drawn after the world,
    // world-anchored via the same translation trick with their own origin.
    if (!spriteVerts.empty()) {
      const litholattice::Mat4 mvp =
          litholattice::mul(vp, translation(spriteOrigin.x, spriteOrigin.y, spriteOrigin.z));
      sw_.renderMesh(spriteVerts.data(), spriteVerts.size(), mvp.m);
    }
    // Framebuffer pixels are 0xAARRGGBB; PNG wants RGB bytes.
    const auto& fb = sw_.framebuffer();
    std::vector<uint8_t> rgb(size_t(w) * size_t(h) * 3);
    for (size_t i = 0, n = size_t(w) * size_t(h); i < n; ++i) {
      const uint32_t p = fb.pixels[i];
      rgb[3 * i] = uint8_t((p >> 16) & 0xFF);
      rgb[3 * i + 1] = uint8_t((p >> 8) & 0xFF);
      rgb[3 * i + 2] = uint8_t(p & 0xFF);
    }
    out.width = w;
    out.height = h;
    if (!litholattice::mcp::encodePngRgb(w, h, rgb.data(), out.png)) return false;
  }

  out.tick = tick_ ? tick_() : 0;
  out.camX = ex;
  out.camY = ey;
  out.camZ = ez;
  out.yaw = yawDeg;
  out.pitch = pitchDeg;
  return true;
}

bool McpSensesAdapter::setCameraPose(std::string_view pilot, const CameraPose& pose) {
  if (pose.pitch < -90.0f || pose.pitch > 90.0f) return false;
  if (pose.fov < 10.0f || pose.fov > 120.0f) return false;
  // Fix 2026-10-11 (regression): unbounded coords reach the software rasterizer
  // as inf/NaN (1e308 parses; float() overflows to inf; NaN poisons lookDir /
  // perspective and int(floor(NaN)) is UB). Reject non-finite and absurd
  // magnitudes before they can get there.
  if (!std::isfinite(pose.x) || !std::isfinite(pose.y) || !std::isfinite(pose.z)) return false;
  constexpr double kMaxCameraCoord = 1e6;
  if (std::fabs(pose.x) > kMaxCameraCoord || std::fabs(pose.y) > kMaxCameraCoord ||
      std::fabs(pose.z) > kMaxCameraCoord)
    return false;
  return rig_ && rig_->setFreePose(pilot, pose);  // false for an unknown pilot
}

bool McpSensesAdapter::trackPlayer(std::string_view pilot) {
  return rig_ && rig_->trackPlayer(pilot);
}

bool McpSensesAdapter::cameraPose(std::string_view pilot, CameraPose& out) const {
  return rig_ && rig_->mirrorPose(pilot, out);
}

bool McpSensesAdapter::captureAudioChunk(std::string_view pilot, int tier, AudioChunk& out) {
  if (!rig_ || !rig_->hasPilot(pilot)) return false;  // unknown pilot
  const int t = std::clamp(tier, 0, litholattice::mcp::kAudioTierCount - 1);
  const int rate = litholattice::mcp::kAudioTierRates[t];
  const size_t step = size_t(kTapRate) / size_t(rate);  // 6, 3, 2, 1
  const size_t need = litholattice::mcp::kAudioChunkFrames * step;

  // drainTap() is the tap's single reader; serialize captures.
  std::lock_guard<std::mutex> alk(audioMutex_);
  const size_t got = audio_.drainTap(tapScratch_.data(), need);
  if (got < need) return false;  // tap has no data yet (or underran)

  // Box-downsample the 48 kHz tap to the tier rate; clamp float -> int16.
  out.sampleRate = rate;
  out.samples.resize(litholattice::mcp::kAudioChunkFrames);
  for (size_t i = 0; i < out.samples.size(); ++i) {
    float sum = 0.0f;
    for (size_t s = 0; s < step; ++s) sum += tapScratch_[i * step + s];
    const float c = std::clamp(sum / float(step), -1.0f, 1.0f);
    out.samples[i] = int16_t(std::lround(c * 32767.0f));
  }
  out.tick = tick_ ? tick_() : 0;
  return true;
}

}  // namespace lithokraft
