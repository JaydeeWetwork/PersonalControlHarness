// McpSensesAdapter: game-side adapters for the MCP pilot-senses seams
// (litholattice::mcp::IVisionCapture / IAudioCapture).
//
// VISION: a standalone SoftwareRenderer (flat shading, the JD-locked
// downgraded path) renders a COPY of the classic chunk-mesh cache from the
// pilot's camera. The cache is fed on the game thread by noteMesh(), called
// from Game::uploadMeshes() for every mesh that lands in the GL renderer
// (copy, before the move; empty verts erase the entry). captureVisionFrame()
// runs on the MCP server thread: it takes a cheap shared_ptr snapshot of
// the cache under the mutex, then renders without holding the game thread.
//
// Per-mesh world offset: classic ChunkMesh verts are sub-chunk-local
// (1/16-block units); the sub-chunk origin (cx*16, sy*16, cz*16 — the same
// 16^3 origin the GL backend uploads as uOffset_) is baked into a per-mesh
// MVP (mul(viewProj, translation(origin))). Meshes are frustum-culled
// against the pilot's viewProj with aabbInFrustum, like the GL path.
//
// WINDING (workaround REMOVED 2026-10-11): the mesher winds quads CCW seen
// from outside, which is what the GL renderer's glFrontFace(GL_CCW) keeps.
// The software renderer's isBackface used to cull exactly those faces (screen-
// space cross <= 0 in y-down coords), so noteMesh() stored every quad
// reversed (v1 <-> v3). The 2026-10-11 engine fix corrected isBackface to
// cross >= 0, matching GL — the reversal was removed the same day (it would
// otherwise double-invert and hide every front face from the agent).
// noteMesh() now stores the verts verbatim.
//
// CAMERAS (engine camera system, 2026-10-12): pilot cameras are engine
// cameras now, owned by GameCameraController and shared through
// PilotCameraRig (lithokraft/game/camera_controller.h). This adapter no
// longer keeps raw CameraPose structs: setCameraPose()/trackPlayer() write
// rig requests, cameraPose() reads the rig's MCP-side mirror, and
// captureVisionFrame() renders from the rig's resolved pose (the engine
// camera's game-thread-computed eye/yaw/pitch/fov, at most one frame
// stale — the mesh cache it renders is a frame snapshot too).
//
// AUDIO: drains the AudioEngine's mixed-output tap (post-shed,
// pre-master-gain, positional — the pilot's ears, not a global mix) and
// box-downsamples to the requested tier rate. The tap is armed once at
// construction and disarmed at destruction. captureAudioChunk() needs
// 1024 * (48000/rate) tap frames; fewer available -> false (no data yet).
//
// Threading: noteMesh()/clearMeshes()/syncCvars() run on the game thread;
// pilotIds()/capture*/setCameraPose()/trackPlayer()/cameraPose() run on the
// MCP server thread. All shared state is mutex-guarded. The mesh cache
// holds immutable shared_ptrs so a capture sees a consistent world without
// blocking uploads. drainTap() is the tap's single reader, serialized by
// its own mutex.
//
// Cvars (registered by registerMcpSensesCvars in settings_cvars.cpp):
// mcp.vision_width / mcp.vision_height (capture defaults, 0 = cvar),
// mcp.audio_tier (the host's upgrade ramp 0..3), camera.chunk_radius. The
// console registry is main-thread-only, so the game thread refreshes them
// into the adapter once per frame via syncCvars(); the MCP thread never
// touches the registry.
//
// Chunk loading (JD-locked 2026-10-11): cameras CAN load chunks, but in a
// LIMITED radius only — never the player's view/sim distances. Wired in
// P2c (2026-10-13, CAM-2): GameCameraController::chunkLoadRequests()
// exposes every non-player camera's centre + camera.chunk_radius budget
// (default 4) and Game::pumpStreaming() feeds them to the ChunkStreamer
// after the player's own requests, inside the same in-flight cap. This
// adapter itself still only RENDERS loaded chunks (sky beyond them); it
// issues no requests — the controller/streamer path does.
//
// MULTI-PILOT (locked 2026-10-11): every method takes a `pilot` id and each
// seam declares the pilots it serves via pilotIds(). v1 wires exactly one
// pilot ("player" = the local player); the camera map is already per-pilot.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "litholattice/audio/audio_engine.h"
#include "litholattice/mcp/mcp_senses.h"
#include "litholattice/render/renderer.h"  // ChunkMesh
#include "litholattice/render/software_renderer.h"
#include "lithokraft/game/camera_controller.h"  // PilotCameraRig (engine pilot cameras)
#include "lithokraft/game/mcp_player_control.h"  // McpPlayerView::GameState

namespace lithokraft {

class McpSensesAdapter : public litholattice::mcp::IVisionCapture,
                         public litholattice::mcp::IAudioCapture {
 public:
  /// Live cvar values, refreshed by the game thread once per frame (the
  /// console registry is main-thread-only; the MCP thread never reads it).
  struct CvarState {
    int visionWidth = 160;   ///< mcp.vision_width
    int visionHeight = 90;   ///< mcp.vision_height
    int audioTier = 0;       ///< mcp.audio_tier (the upgrade ramp 0..3)
    int chunkRadius = 4;     ///< camera.chunk_radius (pilot cameras' streaming budget; consumed via GameCameraController::chunkLoadRequests)
    float frustumMarginDeg = 5.f;  ///< render.frustum_margin_deg (FOV culling margin)
    // Culling-cache prototypes (JD 2026-10-11; all default OFF — see
    // render/cull_prototypes.h). Per-pilot state lives in protoStates_.
    int cometTailFrames = 0;       ///< render.comet_tail_frames
    bool velocityMargin = false;   ///< render.velocity_margin
    float velocityMarginGain = 1.f;  ///< render.velocity_margin_gain
    bool twoTier = false;          ///< render.two_tier
    float twoTierOuterDeg = 10.f;  ///< render.two_tier_outer_deg
  };

  /// `snapshot` returns the piloted player's pose (McpPlayerView::GameState:
  /// x/y/z feet, yaw/pitch in RADIANS like Player::forward); `tick` returns
  /// the sim world tick for frame/chunk stamping (0 when the sim is down).
  /// `rig` carries the per-pilot engine cameras (owned by the game's
  /// GameCameraController, which is constructed before this adapter).
  McpSensesAdapter(litholattice::AudioEngine& audio,
                   std::function<McpPlayerView::GameState()> snapshot,
                   std::function<uint64_t()> tick, std::shared_ptr<PilotCameraRig> rig);
  ~McpSensesAdapter();

  // -- game thread ---------------------------------------------------------
  /// Feed one landed classic mesh into the vision cache (copies the verts).
  /// Empty verts erase the entry (mesh removed). Called from
  /// Game::uploadMeshes() before the move into the GL renderer.
  void noteMesh(const litholattice::ChunkMesh& m);
  /// Drop the whole vision cache (dimension switch / unload).
  void clearMeshes();
  /// This frame's sprite quads in software form (PackedVertex quads,
  /// positions relative to `origin`), converted by the game's SpriteHost.
  /// Captures render them after the chunk meshes, flat-shaded (the sprite's
  /// average color as brightness). Called once per frame from Game::update.
  void setSpriteVerts(std::vector<litholattice::PackedVertex> verts, litholattice::Vec3 origin);
  /// Refresh the cvar-driven knobs (mcp.vision_width, mcp.vision_height,
  /// mcp.audio_tier, camera.chunk_radius). Called once per frame from
  /// Game::update().
  void syncCvars(const CvarState& s);
  /// The last-synced cvar knobs (mutex-guarded copy). The screenshot command
  /// reads mcp.vision_width / mcp.vision_height through this.
  CvarState cvars() const;

  // -- IVisionCapture (MCP thread) ------------------------------------------
  std::vector<std::string> pilotIds() const override;
  /// Renders one PNG frame from the pilot's camera. width/height of 0 take
  /// the mcp.vision_width / mcp.vision_height cvars; nonzero values clamp to
  /// 16..512. VisionFrame.yaw/pitch are DEGREES (CameraPose convention).
  bool captureVisionFrame(std::string_view pilot, int width, int height,
                          litholattice::mcp::VisionFrame& out) override;
  bool setCameraPose(std::string_view pilot,
                     const litholattice::mcp::CameraPose& pose) override;
  bool trackPlayer(std::string_view pilot) override;
  bool cameraPose(std::string_view pilot,
                  litholattice::mcp::CameraPose& out) const override;

  // -- IAudioCapture (MCP thread) -------------------------------------------
  /// One 1024-frame chunk at the tier rate (tier clamped 0..3). Returns
  /// false for an unknown pilot or when the tap has no data yet.
  bool captureAudioChunk(std::string_view pilot, int tier,
                         litholattice::mcp::AudioChunk& out) override;

 private:
  struct CachedMesh {
    int cx = 0, sy = 0, cz = 0;  ///< sub-chunk coords; world origin = *16
    std::vector<litholattice::PackedVertex> verts;  ///< quads, sub-chunk-local
  };

  litholattice::AudioEngine& audio_;
  std::function<McpPlayerView::GameState()> snapshot_;
  std::function<uint64_t()> tick_;
  std::shared_ptr<PilotCameraRig> rig_;  ///< per-pilot engine cameras (own mutex inside)

  mutable std::mutex mutex_;  ///< guards meshes_, cvars_, spriteVerts_
  std::unordered_map<int64_t, std::shared_ptr<const CachedMesh>> meshes_;
  CvarState cvars_;
  std::vector<litholattice::PackedVertex> spriteVerts_;  ///< latest sprite frame (software form)
  litholattice::Vec3 spriteOrigin_{};                    ///< world origin spriteVerts_ are relative to

  std::mutex renderMutex_;  ///< serializes the SoftwareRenderer (MCP thread)
  litholattice::SoftwareRenderer sw_;

  // Culling-cache prototype state, one per pilot camera (JD 2026-10-11).
  // Guarded by its own mutex: only the MCP thread touches it, so the game
  // thread is never blocked by culling decisions.
  std::mutex protoMutex_;
  std::unordered_map<std::string, litholattice::CullProtoState> protoStates_;

  std::mutex audioMutex_;  ///< drainTap() is the tap's single reader
  std::vector<float> tapScratch_;  ///< 6144 floats: 1024 frames @ 48kHz for tier 0
};

}  // namespace lithokraft
