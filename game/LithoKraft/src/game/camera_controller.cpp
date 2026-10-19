// GameCameraController implementation (see camera_controller.h for the
// architecture, threading model, and mode semantics).
#include "lithokraft/game/camera_controller.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "litholattice/camera/camera.h"
#include "litholattice/console/concmd.h"
#include "litholattice/input/input_system.h"
#include "litholattice/mcp/mcp_senses.h"  // mcp::CameraPose (pilot rig mirror)
#include "lithokraft/game/actions.h"
#include "lithokraft/game/player.h"
#include "lithokraft/game/player_adapter.h"  // playerEyePos (CAM-1 FP eye)

namespace lithokraft {
namespace {

namespace cam = litholattice::camera;
using litholattice::Vec3;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.f;
constexpr float kRadToDeg = 180.f / kPi;
/// The game's render FOV: Game::frameParams hardcoded perspective(1.2f)
/// before the camera system; the view cameras carry it (in degrees, the
/// engine convention) so first person renders numerically identical.
constexpr float kGameFovRad = 1.2f;
constexpr float kGameFovDeg = kGameFovRad * kRadToDeg;  // 68.7549...
/// Pilot tracking eye height above the snapshot position — the exact value
/// McpSensesAdapter's tracking path has always added (kEyeHeight there).
constexpr float kPilotEyeHeight = 1.62f;
/// True-velocity clamp: sprint is 60 blocks/s; anything past 4x that in a
/// single frame is a teleport/respawn snap, not locomotion, and would
/// slam the FOV kick / follow look-ahead with a nonsense spike.
constexpr float kMaxTrueSpeed = 240.f;
constexpr float kBlendSeconds = 0.35f;  ///< mode-switch blend (manager ease)

Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float length(Vec3 a) { return std::sqrt(litholattice::dot(a, a)); }
/// Player::forward convention: forward from yaw/pitch (radians).
Vec3 dirFromYawPitch(float yaw, float pitch) {
  return {std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)};
}
float yawOf(Vec3 f) { return std::atan2(f.z, f.x); }
float pitchOf(Vec3 f) { return std::asin(std::clamp(f.y, -1.f, 1.f)); }

CameraViewPose toViewPose(const cam::CameraPose& p) {
  CameraViewPose v;
  v.position = p.position;
  v.forward = p.forward;
  v.yaw = yawOf(p.forward);
  v.pitch = pitchOf(p.forward);
  v.fovRad = p.fovDeg * kDegToRad;
  return v;
}

/// Shared per-frame velocity sampling for the camera targets: true
/// velocity = the anchor movement actually applied since the last frame,
/// divided by dt (blocks/second). Sampled once per frame by the
/// controller, before the camera updates read it.
struct VelocitySampler {
  Vec3 prev{};
  bool havePrev = false;
  Vec3 velocity{};
  void sample(Vec3 now, float dt) {
    if (!havePrev || dt <= 0.f) {
      velocity = {};
      prev = now;
      havePrev = true;
      return;
    }
    Vec3 v = sub(now, prev) * (1.f / dt);
    if (const float l = length(v); l > kMaxTrueSpeed) v = v * (kMaxTrueSpeed / l);
    velocity = v;
    prev = now;
  }
};

/// ICameraTarget over the game Player. Anchor = the body's FEET position
/// (Player.pos is feet since the locomotion wiring); the eye rides in
/// eyeOffset at the locomotion controller's current eye height (via the
/// GameCameraHooks::playerEyeHeight hook), so first person dips when
/// sneaking and drops inside a crawl tunnel. Third person orbits the
/// anchor (feet) and is unaffected beyond that. Velocity is the true
/// applied movement (JD directive: no stub zero — it drives the engine's
/// FOV kick and follow behaviour).
class PlayerCameraTarget final : public cam::ICameraTarget {
 public:
  PlayerCameraTarget(const Player& p, std::function<float()> eyeHeight)
      : player_(p), eyeHeight_(std::move(eyeHeight)) {}
  Vec3 anchorPosition() const override { return player_.pos; }
  Vec3 eyeOffset() const override {
    return {0.f, eyeHeight_ ? eyeHeight_() : kPilotEyeHeight, 0.f};
  }
  /// The current eye height (feet-relative) the FP view renders at:
  /// the locomotion hook's live value, standing height as fallback.
  float eyeHeight() const { return eyeHeight_ ? eyeHeight_() : kPilotEyeHeight; }
  float facingYaw() const override { return player_.yaw; }
  float facingPitch() const override { return player_.pitch; }
  Vec3 velocity() const override { return sampler_.velocity; }
  void sampleVelocity(float dt) { sampler_.sample(player_.pos, dt); }

 private:
  const Player& player_;
  std::function<float()> eyeHeight_;
  VelocitySampler sampler_;
};

/// ICameraTarget over the MCP pilot snapshot (mutex-guarded GameState,
/// refreshed by Game each frame). Anchor = snapshot position; the pilot
/// eye height rides in eyeOffset, reproducing the senses adapter's
/// tracking eye exactly. Velocity is sampled from snapshot movement the
/// same way as the player's.
class SnapshotCameraTarget final : public cam::ICameraTarget {
 public:
  explicit SnapshotCameraTarget(std::function<McpPlayerView::GameState()> snap)
      : snap_(std::move(snap)) {}
  Vec3 anchorPosition() const override {
    const McpPlayerView::GameState g = snap_();
    return {float(g.x), float(g.y), float(g.z)};
  }
  Vec3 eyeOffset() const override { return {0.f, kPilotEyeHeight, 0.f}; }
  float facingYaw() const override { return snap_().yaw; }
  float facingPitch() const override { return snap_().pitch; }
  Vec3 velocity() const override { return sampler_.velocity; }
  void sampleVelocity(float dt) { sampler_.sample(anchorPosition(), dt); }

 private:
  std::function<McpPlayerView::GameState()> snap_;
  VelocitySampler sampler_;
};

/// ICameraWorld over Game's race-free raycast hook (runBetweenTicks +
/// engine voxel raycast; see Game::cameraRaycast). Engaged only while a
/// collision mode is live (third person): disengaged casts report no hit,
/// so first/free frames never pay for a between-ticks raycast and idle
/// boom cameras simply ease out (the engine pulls the boom in instantly
/// on the first engaged cast, so nothing clips).
class GameCameraWorld final : public cam::ICameraWorld {
 public:
  explicit GameCameraWorld(GameCameraHooks hooks) : hooks_(std::move(hooks)) {}
  void setEngaged(bool e) { engaged_ = e; }
  cam::CameraRayHit castRay(Vec3 origin, Vec3 dir, float maxDist) const override {
    cam::CameraRayHit out;
    if (!engaged_ || !hooks_.raycastSolid) return out;
    float d = 0.f;
    if (hooks_.raycastSolid(origin, dir, maxDist, d)) {
      out.hit = true;
      out.distance = d;
    }
    return out;
  }

 private:
  GameCameraHooks hooks_;
  bool engaged_ = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// PilotCameraRig

struct PilotCameraRig::State {
  struct Pilot {
    litholattice::mcp::CameraPose mirror{};  ///< MCP-side pose (degrees); trackPlayer default true
    bool resolvedValid = false;
    CameraViewPose resolved{};
  };
  mutable std::mutex mutex;
  std::map<std::string, Pilot, std::less<>> pilots;
};

PilotCameraRig::PilotCameraRig(GameCameraHooks /*hooks*/) : state_(std::make_shared<State>()) {
  // v1 serves exactly the local player, like McpSensesAdapter::pilotIds().
  state_->pilots.emplace("player", State::Pilot{});
}

PilotCameraRig::~PilotCameraRig() = default;

bool PilotCameraRig::hasPilot(std::string_view pilot) const {
  std::lock_guard<std::mutex> lk(state_->mutex);
  return state_->pilots.find(pilot) != state_->pilots.end();
}

bool PilotCameraRig::setFreePose(std::string_view pilot, const litholattice::mcp::CameraPose& pose) {
  std::lock_guard<std::mutex> lk(state_->mutex);
  const auto it = state_->pilots.find(pilot);
  if (it == state_->pilots.end()) return false;
  it->second.mirror = pose;
  it->second.mirror.trackPlayer = false;  // an explicit pose is a free camera (adapter semantics)
  return true;
}

bool PilotCameraRig::trackPlayer(std::string_view pilot) {
  std::lock_guard<std::mutex> lk(state_->mutex);
  const auto it = state_->pilots.find(pilot);
  if (it == state_->pilots.end()) return false;
  it->second.mirror.trackPlayer = true;
  return true;
}

bool PilotCameraRig::mirrorPose(std::string_view pilot, litholattice::mcp::CameraPose& out) const {
  std::lock_guard<std::mutex> lk(state_->mutex);
  const auto it = state_->pilots.find(pilot);
  if (it == state_->pilots.end()) return false;
  out = it->second.mirror;
  return true;
}

bool PilotCameraRig::resolvedPose(std::string_view pilot, CameraViewPose& out) const {
  std::lock_guard<std::mutex> lk(state_->mutex);
  const auto it = state_->pilots.find(pilot);
  if (it == state_->pilots.end() || !it->second.resolvedValid) return false;
  out = it->second.resolved;
  return true;
}

std::vector<std::string> PilotCameraRig::pilotNames() const {
  std::lock_guard<std::mutex> lk(state_->mutex);
  std::vector<std::string> names;
  names.reserve(state_->pilots.size());
  for (const auto& [name, p] : state_->pilots) names.push_back(name);
  return names;
}

PilotCameraRig::Request PilotCameraRig::request(std::string_view pilot) const {
  std::lock_guard<std::mutex> lk(state_->mutex);
  Request r;
  const auto it = state_->pilots.find(pilot);
  if (it == state_->pilots.end()) return r;
  r.tracking = it->second.mirror.trackPlayer;
  r.freePose = it->second.mirror;
  return r;
}

void PilotCameraRig::publishResolved(std::string_view pilot, const CameraViewPose& pose) {
  std::lock_guard<std::mutex> lk(state_->mutex);
  const auto it = state_->pilots.find(pilot);
  if (it == state_->pilots.end()) return;
  it->second.resolved = pose;
  it->second.resolvedValid = true;
}

// ---------------------------------------------------------------------------
// GameCameraController

struct GameCameraController::Impl {
  explicit Impl(GameCameraHooks hooks) : world(std::move(hooks)) {}

  cam::CameraManager manager;
  GameCameraWorld world;
  std::function<McpPlayerView::GameState()> pilotSnapshot;
  std::shared_ptr<PlayerCameraTarget> playerTarget;
  std::shared_ptr<SnapshotCameraTarget> pilotTarget;
  std::shared_ptr<cam::ICamera> playerCam, orbitCam, followCam, freeCam;
  std::map<std::string, std::shared_ptr<cam::ICamera>, std::less<>> pilotCams;
  std::map<std::string, bool, std::less<>> pilotAppliedTracking;
  std::map<std::string, litholattice::mcp::CameraPose, std::less<>> pilotAppliedFree;
  cam::CameraCvarValues cvars;
  bool cvarsDirty = true;
};

GameCameraController::GameCameraController(Player& player, GameCameraHooks hooks)
    : impl_(std::make_unique<Impl>(hooks)), player_(player) {
  impl_->pilotSnapshot = hooks.pilotSnapshot;
  impl_->playerTarget = std::make_shared<PlayerCameraTarget>(player, hooks.playerEyeHeight);
  if (hooks.pilotSnapshot)
    impl_->pilotTarget = std::make_shared<SnapshotCameraTarget>(hooks.pilotSnapshot);
  pilotRig_ = std::make_shared<PilotCameraRig>(hooks);

  cam::CameraConfig cfg;
  cfg.fovDeg = kGameFovDeg;
  auto makeCam = [&](cam::CameraMode mode, bool attachPlayer) {
    auto c = std::make_shared<cam::Camera>(cfg);
    c->setMode(mode);
    if (attachPlayer) c->attach(impl_->playerTarget);
    return c;
  };
  impl_->playerCam = makeCam(cam::CameraMode::FirstPerson, true);
  impl_->orbitCam = makeCam(cam::CameraMode::ThirdPerson, true);
  impl_->followCam = makeCam(cam::CameraMode::Follow, true);
  impl_->freeCam = makeCam(cam::CameraMode::Free, false);
  impl_->freeCam->setPose({player.pos, player.forward(), {0.f, 1.f, 0.f}, kGameFovDeg});
  impl_->manager.addCamera("player", impl_->playerCam);
  impl_->manager.addCamera("orbit", impl_->orbitCam);
  impl_->manager.addCamera("follow", impl_->followCam);
  impl_->manager.addCamera("free", impl_->freeCam);
  impl_->manager.setActive("player");  // cut: the game boots in first person
  freePos_ = player.pos;
  freeYaw_ = player.yaw;
  freePitch_ = player.pitch;
  cachedPose_ = {playerEyePos(player, impl_->playerTarget->eyeHeight()), player.forward(), player.yaw,
                 player.pitch, kGameFovRad};
}

GameCameraController::~GameCameraController() = default;

void GameCameraController::setMode(GameCameraMode m) {
  if (m == mode_) return;
  mode_ = m;
  const char* name = "player";
  switch (m) {
    case GameCameraMode::FirstPerson: name = "player"; break;
    case GameCameraMode::ThirdPerson: name = "orbit"; break;
    case GameCameraMode::Follow: name = "follow"; break;
    case GameCameraMode::Free: {
      name = "free";
      // Start where the player is looking, then fly independently.
      const cam::CameraPose cur = impl_->manager.pose();
      freePos_ = cur.position;
      freeYaw_ = yawOf(cur.forward);
      freePitch_ = pitchOf(cur.forward);
      impl_->freeCam->setPose(cur);
      break;
    }
  }
  impl_->world.setEngaged(m == GameCameraMode::ThirdPerson);
  impl_->manager.setActive(name, kBlendSeconds, cam::CameraBlendEase::EaseInOut);
}

void GameCameraController::syncCvars(const cam::CameraCvarValues& v) {
  impl_->cvars = v;
  impl_->cvarsDirty = true;
}

void GameCameraController::update(const litholattice::InputSystem& in, float dt, bool lookEnabled,
                                  bool playing, int frameWidth, int frameHeight) {
  Impl& im = *impl_;

  // True velocity first: sampled from the movement applied since last
  // frame, before any camera reads it this frame.
  im.playerTarget->sampleVelocity(dt);
  if (im.pilotTarget) im.pilotTarget->sampleVelocity(dt);

  if (im.cvarsDirty) {
    im.cvarsDirty = false;
    // The view cameras keep the game's render FOV (1.2 rad) — the
    // camera.default_fov cvar drives pilot cameras, not the game view.
    auto applyView = [&](cam::ICamera* c) {
      if (!c) return;
      cam::applyCameraCvars(im.cvars, c->config());
      c->config().fovDeg = kGameFovDeg;
    };
    applyView(im.playerCam.get());
    applyView(im.orbitCam.get());
    applyView(im.followCam.get());
    applyView(im.freeCam.get());
    for (auto& [name, c] : im.pilotCams) cam::applyCameraCvars(im.cvars, c->config());
  }
  if (frameWidth > 0 && frameHeight > 0) {
    const float aspect = float(frameWidth) / float(frameHeight);
    im.playerCam->config().aspect = aspect;
    im.orbitCam->config().aspect = aspect;
    im.followCam->config().aspect = aspect;
    im.freeCam->config().aspect = aspect;
  }

  // Input feeding (presentation input only; the sim never sees this).
  if (playing && lookEnabled) {
    const litholattice::AxisValue look = in.axis(Action::Look, dt);
    const float wheel = in.devices().wheel();
    switch (mode_) {
      case GameCameraMode::ThirdPerson:
        // Pitch sign matches Player::update (mouse up = look up).
        im.orbitCam->addLookInput(look.x, -look.y);
        if (wheel != 0.f) im.orbitCam->addZoomInput(wheel);  // up = closer
        break;
      case GameCameraMode::Follow:
        if (wheel != 0.f) im.followCam->addZoomInput(wheel);
        break;
      case GameCameraMode::Free: {
        freeYaw_ += look.x;
        freePitch_ = std::clamp(freePitch_ - look.y, -1.55f, 1.55f);
        float fwd = in.value(Action::MoveForward) - in.value(Action::MoveBack);
        float side = in.value(Action::MoveRight) - in.value(Action::MoveLeft);
        if (const float len = std::hypot(fwd, side); len > 1.f) { fwd /= len; side /= len; }
        const Vec3 f = dirFromYawPitch(freeYaw_, freePitch_);
        const Vec3 right = litholattice::normalize(litholattice::cross(f, {0.f, 1.f, 0.f}));
        const float speed = in.held(Action::Sprint) ? 60.f : 24.f;
        const float up = in.value(Action::Jump) - in.value(Action::Sneak);
        freePos_ = freePos_ + (f * fwd + right * side + Vec3{0.f, 1.f, 0.f} * up) * (speed * dt);
        break;
      }
      case GameCameraMode::FirstPerson:
        break;  // Player::update owns the look; the FP camera is synced below
    }
  }
  if (mode_ == GameCameraMode::Free)
    im.freeCam->setPose({freePos_, dirFromYawPitch(freeYaw_, freePitch_), {0.f, 1.f, 0.f}, kGameFovDeg});

  // First person: keep the engine camera's own state glued to the player
  // (its orientation is camera-owned state, seeded on attach; the game
  // turns the player, so the camera pose is re-issued each frame — this
  // is what the render seam reads through viewPose(), and it keeps blend
  // sources honest when switching away). The glued pose is the EYE pose
  // (CAM-1): the same playerEyePos viewPose() resolves, so the manager's
  // blend source and the live FP view agree to the centimetre.
  if (mode_ == GameCameraMode::FirstPerson)
    im.playerCam->setPose({playerEyePos(player_, im.playerTarget->eyeHeight()), player_.forward(),
                           {0.f, 1.f, 0.f}, kGameFovDeg});

  // Pilot cameras: create lazily, apply MCP rig requests on change, and
  // (for tracking pilots) glue the engine camera to the live snapshot —
  // same follow trick as the main first-person camera.
  if (im.pilotTarget) {
    for (const std::string& name : pilotRig_->pilotNames()) {
      auto& slot = im.pilotCams[name];
      if (!slot) {
        auto cfg = cam::CameraConfig{};
        cam::applyCameraCvars(im.cvars, cfg);
        slot = std::make_shared<cam::Camera>(cfg);
        im.manager.addCamera("pilot:" + name, slot);
        im.pilotAppliedTracking[name] = false;
      }
      const PilotCameraRig::Request req = pilotRig_->request(name);
      if (req.tracking) {
        if (!im.pilotAppliedTracking[name]) {
          slot->attach(im.pilotTarget);
          slot->setMode(cam::CameraMode::FirstPerson);
          im.pilotAppliedTracking[name] = true;
        }
        // Glue the engine camera's orientation to the live snapshot (its
        // FP orientation is camera-owned state, seeded on attach — the
        // snapshot facing moves every frame). Position is target-driven
        // (snapshot anchor + eye height), so this pose's position is the
        // same eye, for consistency.
        if (im.pilotSnapshot) {
          const McpPlayerView::GameState g = im.pilotSnapshot();
          const Vec3 eye{float(g.x), float(g.y) + kPilotEyeHeight, float(g.z)};
          slot->setPose({eye, dirFromYawPitch(g.yaw, g.pitch), {0.f, 1.f, 0.f},
                         slot->config().fovDeg});
        }
      } else {
        const litholattice::mcp::CameraPose& fp = req.freePose;
        const auto applied = im.pilotAppliedFree.find(name);
        const bool changed = im.pilotAppliedTracking[name] || applied == im.pilotAppliedFree.end() ||
                             applied->second.x != fp.x || applied->second.y != fp.y ||
                             applied->second.z != fp.z || applied->second.yaw != fp.yaw ||
                             applied->second.pitch != fp.pitch || applied->second.fov != fp.fov;
        if (changed) {
          slot->detach();
          slot->setMode(cam::CameraMode::Free);
          slot->config().fovDeg = fp.fov;
          slot->setPose({{float(fp.x), float(fp.y), float(fp.z)},
                         dirFromYawPitch(fp.yaw * kDegToRad, fp.pitch * kDegToRad),
                         {0.f, 1.f, 0.f}, fp.fov});
          im.pilotAppliedFree[name] = fp;
          im.pilotAppliedTracking[name] = false;
        }
      }
    }
  }

  im.manager.update(dt, im.world);

  // Publish every pilot's resolved pose for the MCP capture path.
  for (auto& [name, c] : im.pilotCams) pilotRig_->publishResolved(name, toViewPose(c->pose()));

  if (mode_ != GameCameraMode::FirstPerson) {
    cachedPose_ = toViewPose(im.manager.pose());
    // Third person: the character faces the camera's view direction
    // (horizontally), so movement + the interaction ray follow the orbit.
    if (mode_ == GameCameraMode::ThirdPerson && playing) {
      const Vec3 f = cachedPose_.forward;
      if (f.x != 0.f || f.z != 0.f) player_.yaw = std::atan2(f.z, f.x);
    }
  }
}

std::vector<GameCameraController::ChunkLoadRequest> GameCameraController::chunkLoadRequests() const {
  std::vector<ChunkLoadRequest> out;
  const auto add = [&out](const cam::ICamera* c) {
    if (!c) return;
    const Vec3 centre = c->chunkLoadCenter();
    const int r = c->chunkLoadRadiusChunks();
    if (r <= 0) return;
    out.push_back({int(std::floor(centre.x / 16.f)), int(std::floor(centre.z / 16.f)), r});
  };
  // The active view camera, unless it is the first-person player camera
  // (player-centred streaming covers that centre already).
  if (mode_ != GameCameraMode::FirstPerson) add(impl_->manager.camera(impl_->manager.activeName()));
  // MCP pilot cameras (tracking pilots sit at the player, where their
  // request dedupes against the player's own; free pilots can be far away).
  for (const auto& [name, c] : impl_->pilotCams) add(c.get());
  return out;
}

CameraViewPose GameCameraController::viewPose() const {
  if (mode_ == GameCameraMode::FirstPerson) {
    // Live through the player target: menu-dolly and bench writes to the
    // player pose land between update() and frameParams() and must be
    // reflected exactly. The position is the EYE (CAM-1, P2b-2):
    // player_.pos is the locomotion body's FEET, so the view sits at
    // playerEyePos with the controller's live eye height (Stand 1.62 /
    // Sneak 1.27 / Crawl 0.4) — the same point the interaction ray starts
    // from and MCP pilots see from. Forward/yaw/pitch/FOV are unchanged
    // from the pre-camera-system expressions.
    return {playerEyePos(player_, impl_->playerTarget->eyeHeight()), player_.forward(), player_.yaw,
            player_.pitch, kGameFovRad};
  }
  return cachedPose_;
}

// ---------------------------------------------------------------------------
// Mode names + console surface

const char* gameCameraModeName(GameCameraMode m) {
  switch (m) {
    case GameCameraMode::FirstPerson: return "first";
    case GameCameraMode::ThirdPerson: return "third";
    case GameCameraMode::Follow: return "follow";
    case GameCameraMode::Free: return "free";
  }
  return "first";
}

std::optional<GameCameraMode> parseGameCameraMode(std::string_view name) {
  if (name == "first" || name == "firstperson" || name == "fp" || name == "1")
    return GameCameraMode::FirstPerson;
  if (name == "third" || name == "thirdperson" || name == "tp" || name == "3")
    return GameCameraMode::ThirdPerson;
  if (name == "follow" || name == "chase") return GameCameraMode::Follow;
  if (name == "free" || name == "spectate" || name == "noclip") return GameCameraMode::Free;
  return std::nullopt;
}

void registerCameraCommands(litholattice::console::ConsoleRegistry& r, CameraCommandBindings b) {
  using litholattice::console::ConsoleArgs;
  using litholattice::console::ConsoleRegistry;
  std::string err;
  r.registerCommand(
      "camera",
      "camera [first|third|follow|free] - view camera mode (no arg = current mode)",
      [b](ConsoleRegistry& r, const ConsoleArgs& a) {
        if (a.size() == 1) {
          r.print(b.status());
          return;
        }
        if (a.size() == 2) {
          r.print(b.setModeByName(a[1]));
          return;
        }
        if (a.size() == 3 && a[1] == "mode") {
          r.print(b.setModeByName(a[2]));
          return;
        }
        r.print("usage: camera [first|third|follow|free]");
      },
      &err);
}

}  // namespace lithokraft
