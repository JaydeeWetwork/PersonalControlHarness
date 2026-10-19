// GameCameraController: LithoKraft's integration of the engine camera system
// (litholattice::camera). Owns the game's CameraManager and its cameras:
//
//   - the player camera, attached to the local player through a
//     PlayerCameraTarget adapter. The target's ANCHOR is the player's
//     FEET (player_.pos is the locomotion body's feet position); the eye
//     rides in the target's eyeOffset at the locomotion controller's
//     current eye height (Stand 1.62 / Sneak 1.27 / Crawl 0.4, via the
//     playerEyeHeight hook), and first person renders from that eye —
//     playerEyePos(player_, eyeHeight) — exactly where Game's
//     interaction ray starts and where MCP pilots look from. (P2b-2,
//     review CAM-1: an earlier version of this comment claimed the
//     anchor WAS the eye; that was true only before the locomotion
//     wiring made pos the feet.) Default mode FirstPerson; the console
//     `camera` command switches first/third/follow/free (each mode is
//     its own engine camera in the manager).
//   - a third-person orbit camera: mouse Look orbits, the wheel zooms,
//     pitch is clamped by the engine, and the spring arm collides against
//     the world through a GameCameraWorld adapter fed by Game's race-free
//     raycast hook (SimThread::runBetweenTicks + the engine voxel raycast,
//     one ray per frame, third person only). A follow camera (velocity-
//     driven chase, wheel sets its distance) binds the same target.
//   - a free camera for spectate/debug (Free mode): Look turns it, the
//     movement actions fly it; the player is untouched while it is active.
//   - pilot cameras for MCP agent vision: one engine camera per pilot in
//     the PilotCameraRig, created lazily, shared with McpSensesAdapter.
//
// Presentation only: nothing here feeds the sim thread, and camera state is
// never an input to deterministic simulation.
//
// FP-BYPASS SCOPE (CAM-4, review 2026-10-13): "the camera system drives the
// view" is true for the NON-first-person modes and for MCP pilots — those
// render from the CameraManager's resolved poses. FIRST PERSON is a
// deliberate bypass (for pre-system exactness): viewPose() composes the FP
// pose directly from the player (eye position via playerEyePos at the live
// eye height, forward/yaw/pitch, the fixed 1.2 rad FOV) and does NOT read
// the manager's FP camera for the rendered view. Consequence, by design:
// engine FP features — head-bob, FOV kick, trauma shake, damped FOV — have
// NO effect on the first-person view. They still apply to pilot cameras and
// to the manager-driven modes (third/follow/free). The manager's FP camera
// is still glued to the eye pose each frame so blends OUT of first person
// start from the true view.
//
// Threading: the controller and the engine cameras live on the game thread
// (update() runs inside Game::update). The PilotCameraRig is the exception:
// MCP server threads read/write pilot state through it, so it guards its
// mirror state with a mutex and the engine cameras behind it are only ever
// touched from the game thread (the controller publishes resolved poses
// into the rig at the end of update()).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "litholattice/core/math.h"  // Vec3
#include "litholattice/mcp/mcp_senses.h"  // mcp::CameraPose (pilot boundary type)
#include "lithokraft/game/mcp_player_control.h"  // McpPlayerView::GameState

namespace litholattice {
class InputSystem;
namespace console {
class ConsoleRegistry;
}  // namespace console
namespace camera {
struct CameraCvarValues;  // camera/camera.h — full definition in the .cpp
}  // namespace camera
}  // namespace litholattice

namespace lithokraft {

struct Player;

/// The view Game::frameParams renders from. Angles are radians in the
/// Player::forward convention; fovRad is the vertical FOV handed to
/// litholattice::perspective (1.2 rad = the value frameParams hardcoded
/// before the camera system, so FirstPerson renders numerically identical).
struct CameraViewPose {
  litholattice::Vec3 position{20.f, 92.f, 20.f};
  litholattice::Vec3 forward{1.f, 0.f, 0.f};  ///< normalized look direction
  float yaw = 0.785f;
  float pitch = -0.35f;
  float fovRad = 1.2f;
};

/// The game's view modes (console `camera` command values). These select
/// engine cameras in the manager (first person / orbit / follow / free);
/// the engine's Fixed mode stays unbound in the game for now.
enum class GameCameraMode { FirstPerson, ThirdPerson, Follow, Free };

const char* gameCameraModeName(GameCameraMode m);
std::optional<GameCameraMode> parseGameCameraMode(std::string_view name);

/// World + player access the controller needs from Game, as hooks so the
/// controller never touches SimThread / Dimension directly (the same
/// pattern GameWorldQuery and the entity behaviors use).
struct GameCameraHooks {
  /// MCP pilot snapshot (the piloted player's pose; yaw/pitch radians).
  /// The pilot target adds the standard eye height on top of it, exactly
  /// like McpSensesAdapter's tracking path always has.
  std::function<McpPlayerView::GameState()> pilotSnapshot;
  /// Race-free raycast against the active world (Game implements it with
  /// SimThread::runBetweenTicks + litholattice::raycast). Returns true and
  /// sets `hitDist` (a conservative distance from `origin` that is safe to
  /// place a camera at) when a solid cell blocks the ray within maxDist.
  /// Called at most once per frame, and only in third person.
  std::function<bool(litholattice::Vec3 origin, litholattice::Vec3 dir, float maxDist,
                     float& hitDist)>
      raycastSolid;
  /// The local player's current locomotion eye height (feet-relative):
  /// Stand 1.62 / Sneak 1.27 / Crawl 0.4 from the locomotion controller.
  /// PlayerCameraTarget reads it on every pose resolution, so the
  /// first-person view dips when sneaking and drops inside a crawl
  /// tunnel. May be empty (the target falls back to the standing height).
  std::function<float()> playerEyeHeight;
};

/// Per-pilot engine cameras for MCP agent vision. Owned by the
/// GameCameraController; McpSensesAdapter holds a shared_ptr and talks to
/// pilots through this rig instead of keeping raw CameraPose structs.
///
/// Semantics mirror the adapter's old per-pilot CameraPose map exactly:
/// a pilot is either tracking the player (the default) or holding a free
/// pose set through setFreePose(); cameraPose() reports the MCP-side pose
/// (degrees, CameraPose convention); capture resolves the eye from the
/// engine camera's game-thread-computed pose.
class PilotCameraRig {
 public:
  explicit PilotCameraRig(GameCameraHooks hooks);
  ~PilotCameraRig();

  /// Pilots the rig serves. v1 registers exactly "player" (the local
  /// player), matching McpSensesAdapter::pilotIds().
  bool hasPilot(std::string_view pilot) const;
  /// MCP thread: hold a free pose (already validated by the caller; stored
  /// verbatim for cameraPose()). False for an unknown pilot.
  bool setFreePose(std::string_view pilot, const litholattice::mcp::CameraPose& pose);
  /// MCP thread: return the pilot to player tracking. False if unknown.
  bool trackPlayer(std::string_view pilot);
  /// MCP thread: the pilot's MCP-side pose (degrees). False if unknown.
  bool mirrorPose(std::string_view pilot, litholattice::mcp::CameraPose& out) const;
  /// MCP thread: the resolved capture pose (eye position, yaw/pitch/fov in
  /// radians) from the engine camera's last game-thread update. False if
  /// unknown or never resolved.
  bool resolvedPose(std::string_view pilot, CameraViewPose& out) const;

  // -- game thread (GameCameraController mediates; not for the adapter) ----
  /// A pilot's pending state, applied to its engine camera by the
  /// controller: tracking the player, or holding `freePose` (degrees, the
  /// MCP-side mirror, converted by the controller).
  struct Request {
    bool tracking = true;
    litholattice::mcp::CameraPose freePose;
  };
  std::vector<std::string> pilotNames() const;
  Request request(std::string_view pilot) const;
  void publishResolved(std::string_view pilot, const CameraViewPose& pose);

 private:
  struct State;
  std::shared_ptr<State> state_;
};

/// The game's camera-system front end. Game owns one, constructs it with
/// the local player + hooks, calls update() once per frame after
/// Player::update, and renders from viewPose().
class GameCameraController {
 public:
  GameCameraController(Player& player, GameCameraHooks hooks);
  ~GameCameraController();

  void setMode(GameCameraMode m);
  GameCameraMode mode() const { return mode_; }
  /// True when the active mode consumes the Look axis itself (orbit /
  /// follow / free): Game must not also turn the player with it.
  bool consumesLook() const { return mode_ != GameCameraMode::FirstPerson; }
  /// True in third person / follow: the wheel zooms the camera instead of
  /// cycling the hotbar, so Game skips its wheel steps for the frame.
  bool consumesWheel() const {
    return mode_ == GameCameraMode::ThirdPerson || mode_ == GameCameraMode::Follow;
  }

  /// Cvar-driven camera config (camera.*), refreshed by Game once per
  /// frame from the console registry (main-thread-only) — the same
  /// pattern McpSensesAdapter uses. Applied to camera configs on the
  /// next update().
  void syncCvars(const litholattice::camera::CameraCvarValues& v);

  /// One frame of camera work (game thread, after Player::update).
  /// `lookEnabled` is the same gate Game passes to Player::update (cursor
  /// captured, playing, no modal UI): orbit/free input only applies while
  /// it holds. `playing` is MenuState::Playing (menu/bench drive the
  /// player pose directly; the attached camera just follows).
  void update(const litholattice::InputSystem& in, float dt, bool lookEnabled, bool playing,
              int frameWidth, int frameHeight);

  /// The pose Game::frameParams renders from. FirstPerson resolves live
  /// through the player target, so menu-dolly and bench writes to the
  /// player pose (which happen between update() and frameParams()) are
  /// reflected exactly, as before the camera system. Third/Free return
  /// the pose update() last computed.
  CameraViewPose viewPose() const;

  std::shared_ptr<PilotCameraRig> pilotRig() { return pilotRig_; }

  /// One non-player camera's chunk-load request (CAM-2, P2c 2026-10-13):
  /// chunk column (cx, cz) + radius in chunks, from that camera's
  /// chunkLoadCenter()/chunkLoadRadiusChunks().
  struct ChunkLoadRequest {
    int cx = 0, cz = 0, radiusChunks = 0;
  };
  /// Chunk-load requests for every camera that is NOT the first-person
  /// player view: the active view camera when the mode is third / follow /
  /// free (its centre is wherever it is looking from, which can be far
  /// from the player), plus every MCP pilot camera. The player camera is
  /// deliberately absent — Game's player-centred streaming already covers
  /// it. Game feeds these to the ChunkStreamer AFTER the player's own
  /// update, so they spend only leftover in-flight capacity. Radii are
  /// the cameras' configured chunk budgets (camera.chunk_radius, default
  /// 4): there is NO hardware scaling of the radius (the earlier claim
  /// said there was; there still is not — the radius is a cvar).
  std::vector<ChunkLoadRequest> chunkLoadRequests() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  Player& player_;
  GameCameraMode mode_ = GameCameraMode::FirstPerson;
  CameraViewPose cachedPose_{};  ///< third/follow/free view pose from the last update()
  // Free-camera flight state (the engine Free camera holds a pose; the
  // controller integrates movement input into it and setPose()s the result).
  litholattice::Vec3 freePos_{};
  float freeYaw_ = 0.f, freePitch_ = 0.f;
  std::shared_ptr<PilotCameraRig> pilotRig_;
};

/// Console surface (Decision-9 pattern, mirroring GameCommandBindings):
///   camera            - print the current mode
///   camera <mode>     - switch: first | third | follow | free
///   camera mode <m>   - same
struct CameraCommandBindings {
  std::function<std::string(std::string_view modeName)> setModeByName;
  std::function<std::string()> status;
};
void registerCameraCommands(litholattice::console::ConsoleRegistry& r, CameraCommandBindings b);

}  // namespace lithokraft
