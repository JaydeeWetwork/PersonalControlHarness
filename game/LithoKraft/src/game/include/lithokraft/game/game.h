// Game layer: wires world + sim + render together for one player. Owns the dimension
// registry, the active dimension's sim thread, the mesh pipeline, creative placement,
// hotbar and debug commands.
#pragma once
#include "lithokraft/content/circuit_scene.h"
#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <unordered_set>
#include "lithokraft/game/engine.h"
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "litholattice/core/threading.h"
#include "lithokraft/game/sound_director.h"
#include "lithokraft/game/player.h"
#include "lithokraft/game/player_adapter.h"
#include "lithokraft/game/player_locomotion.h"
#include "lithokraft/game/entities.h"
#include "lithokraft/game/resource_manager.h"
#include "litholattice/input/input_system.h"
#include "lithokraft/game/actions.h"
#include "lithokraft/game/world_save.h"
#include "lithokraft/game/camera_controller.h"  // GameCameraController (engine camera system)
#include "lithokraft/game/sprite_host.h"        // SpriteHost (engine sprite system)
#include "lithokraft/game/ai_host.h"            // AiHost (engine AiWorld production owner, B6)
#include "lithokraft/game/deceitor.h"           // Deceitor mob director (brains live in the AiWorld)
#include "lithokraft/game/mcp_player_control.h"
#include "lithokraft/game/mcp_senses_adapter.h"
#include "lithokraft/game/settings_cvars.h"
#include "lithokraft/game/game_commands.h"
#include "lithokraft/game/fragment_gallery.h"
#include "lithokraft/game/fragment_markdown.h"
#include "lithokraft/game/fragment_state.h"
#include "lithokraft/game/world_query.h"
#include "lithokraft/game/bench_stream.h"
#include "lithokraft/game/chunk_streamer.h"
#include "litholattice/core/time.h"
#include "litholattice/platform/window.h"
#include "litholattice/render/renderer.h"
#include "litholattice/render/binary_greedy_mesher.h"   // BinaryQuad path (mesher option)
#include "litholattice/render/pbr_quad_mesh.h"          // PbrChunkMesh + expansion
#include "litholattice/render/pbr_emissive_provider.h"  // ChunkEmissiveProvider (PBR lights)
#include "litholattice/script/script_engine.h"
#include "litholattice/console/concmd.h"
#include "litholattice/console/debug_console.h"
#include "litholattice/console/script_console.h"
#include "litholattice/mcp/server.h"  // McpServer / McpBackends (P2c MCP wiring)
#include "litholattice/projectile/projectile.h"  // ProjectileManager (P2c, ENT-1)
#include "litholattice/sim/sim_thread.h"
#include "litholattice/world/dimension.h"
#include "lithokraft/game/game_ui.h"
#include "lithokraft/game/script_editor.h"
#include "lithokraft/game/settings.h"
#include "lithokraft/game/ui_script_bindings.h"

namespace litholattice { class MaterialRegistry; }  // pbr_materials.h; main.cpp owns the instance

namespace lithokraft {

/// The game's clock domains (SimConfig::domains order): lithographed EUV-tier chips.
constexpr int kEuvDomain = 0;
inline const SimStats::Domain& euvStats(const SimStats& s) { return s.domains[kEuvDomain]; }

struct GameConfig {
  std::string dimension = "wafer";
  uint64_t seed = 0;        // world seed (--seed): worldgen noise, per-chunk features
  int worldChunks = 16, tps = kDefaultTps, tickRadius = 8;
  bool euv = true;
  bool timingHandshake = true;   // SimConfig::timingHandshake at world load (standard; default ON since 2026-10-14)
  int handshakePhases = 4;       // phases per handshake cycle
  std::string scriptsDir;   // empty = no scripts
  bool pauseSim = true;     // the pause menu stops the sim (single-player); bench ignores it
  std::string benchScene = "default";  // "circuits" / "display": stress scenes built next to the spawn
  int sceneSize = 120;
  int sceneRows = 16;
  std::string pattern = "checkers";  // pattern-wall: checkers | circles  // wall-pixel: rows (1-16)
  bool screenVertical = false;          // --screen-orient vertical: framebuffer as an upright wall                  // bench scene footprint (blocks per side)
  std::string saveDir;      // empty = no saving / loading (tests, --bench, --no-save)
  double autosaveSeconds = 300.0;
  std::string version = "dev";  // LITHOKRAFT_VERSION, plumbed from main.cpp for the menu footer
  GameSettings settings;        // player preferences (main.cpp loads config/settings.json)
  std::string settingsPath;     // where the settings persist (empty = do not save)
  const litholattice::MaterialRegistry* materials = nullptr;  // PBR registry (main.cpp loads the
      // data file and owns it; must outlive the game). Null = chunk saves skip the MATS payload.
};

class Game {
 public:
  /// `resources` (block materials for meshing and HUD swatches; F9 reloads them), `audio` and
  /// `input` must outlive the game. The game pushes / pops input contexts (inventory, keyboard
  /// typing) on `input`.
  Game(const GameConfig& cfg, IRenderer& renderer, JobPool& pool, ResourceManager& resources, AudioEngine& audio,
       InputSystem& input);
  ~Game();
  /// Saves the active dimension and the player (F5, autosave, exit, dimension switch). `wait`
  /// blocks until it is on disk. No-op without a save directory.
  bool saveWorld(const char* reason, bool wait);  // true when a save was actually written
  bool ok() const { return active_ != nullptr; }

  /// Reads this frame's actions (the app updated the InputSystem first), turns them into sim
  /// commands (applied by the sim thread at its next tick boundary), uploads meshes, plays sounds.
  void update(float dt, int width, int height);
  FrameParams frameParams(int width, int height) const;
  /// The view pose the render path uses: the camera system's active camera
  /// (first person resolves live from the player — the identical view the
  /// game rendered before the camera system).
  CameraViewPose activeCameraPose() const { return camera_.viewPose(); }
  void uploadEntities();  // builds the item billboard mesh
  std::string statusLine(double fps, double worstMs) const;
  // Frame timing measured by the app loop (fps, avg/max frame ms over the last ~0.5 s); shown on the HUD.
  void setFrameStats(float fps, float avgMs, float maxMs) { fps_ = fps; frameAvgMs_ = avgMs; frameMaxMs_ = maxMs; }
  /// --bench: put the camera on a fixed, repeatable path (a slow circle over the spawn, looking
  /// across the world) at time `t` seconds. Same path for every backend and build.
  /// --bench: put the camera on a fixed, repeatable path at time `t` seconds. "orbit": a slow
  /// circle over the spawn. "stream": a straight line (+x, 32 blocks/s) out of the fixed world,
  /// with a BenchStreamer generating / meshing / dropping columns around the camera.
  void setBenchPath(const std::string& path);
  void setBenchCamera(float t);
  BenchStreamer* benchStreamer() { return stream_.get(); }
  /// Latest sim stats snapshot. Returns nullopt before the sim thread exists.
  /// NOTE: the returned reference aliases the triple-buffer's read slot — copy
  /// what you need immediately; the next stats_.publish() may overwrite it.
  std::optional<std::reference_wrapper<const SimStats>> simStats() {
    if (!sim_) return std::nullopt;
    return std::cref(sim_->stats());
  }
  /// Main-thread cost of the last update(): mesh uploads (ms, bytes).
  double lastUploadMs() const { return uploadMs_; }
  size_t lastUploadBytes() const { return uploadBytes_; }
  // One-line perf summary for the log (called once per second by the app).
  std::string perfLine(double fps, double frameAvgMs, double frameMaxMs) const;
  bool wantsCursorCaptured() const { return captured_ && !inventoryOpen_ && !editorOpen() && !paused_; }
  /// Window focus changed (engine focus events): losing it opens the pause menu (not in bench).
  void onFocusChanged(bool focused);
  void setPaused(bool p);
  bool paused() const { return paused_; }
  /// The pause state machine's current state (P2b-2): Playing, MenuPaused
  /// (Esc / launch menu), TickFrozen (tick_freeze / tick_step), HsFrozen
  /// (timing-handshake watchdog freeze, mirrored from the sim).
  enum class PauseState { Playing, MenuPaused, TickFrozen, HsFrozen };
  PauseState pauseState() const { return pauseState_; }
  bool quitRequested() const { return quit_; }
  /// Launch menu state machine: boot opens the menu (world loaded, sim paused, cinematic
  /// camera); Play hands control to the player instantly, no reload.
  void enterMenu();
  void enterPlay();
  bool inMenu() const { return menuState_ == MenuState::Menu; }
  const GameSettings& settings() const { return settings_; }
  /// Applies a display-mode change from the settings UI (main.cpp wires the window).
  void setDisplayModeApplier(std::function<void(DisplayMode)> f) { displayApplier_ = std::move(f); }
  /// OS clipboard for UI text fields (optional; a process-local one otherwise). Must outlive the game.
  void setClipboard(ui::IClipboard* clipboard) { ui_.root().setClipboard(clipboard); }
  /// The game's UI (HUD, hotbar, inventory, script editor); for tools and tests.
  GameUi& gameUi() { return ui_; }
  /// MCP player-control adapters (used by mcpBackends() below; also
  /// directly accessible for tests).
  McpPlayerController& mcpPlayerController() { return mcpController_; }
  McpPlayerView& mcpPlayerView() { return mcpView_; }
  /// MCP pilot-senses adapter (one object serves both the vision and
  /// audio seams).
  McpSensesAdapter& mcpSenses() { return mcpSenses_; }
  /// The production AiWorld host (B6): bound to the active dimension in
  /// loadActive(), gated by the ai.enabled cvar (default off — nothing
  /// is constructed), advanced from elapsed sim world ticks in update().
  AiHost& aiHost() { return aiHost_; }
  /// The Deceitor director: owns the mob brains' game-side registry +
  /// hooks (filled in the ctor); mobs themselves live in aiHost_'s
  /// AiWorld. Spawn/interact/attack are today's explicit test paths
  /// (the `deceitor` console command, cheat) for the future
  /// click-picking / mob-combat hooks.
  DeceitorDirector& deceitorDirector() { return deceitorDirector_; }
  std::string spawnDeceitorCmd();
  std::string deceitorInteractCmd();
  std::string deceitorAttackCmd();
  /// The MCP backend bundle for the engine McpServer (P2c 2026-10-13,
  /// review F-M9): the game's existing adapters — player control, player
  /// view, pilot vision, pilot audio. Every other seam stays null, so
  /// tools backed by an unwired seam answer "not wired" honestly (the
  /// engine server's documented null-seam behaviour). The shared_ptrs use
  /// no-op deleters: the adapters are Game members, and the wiring site
  /// (main.cpp, --mcp-stdio) stops the server BEFORE the Game is
  /// destroyed, so the pointers never outlive the Game.
  litholattice::mcp::McpBackends mcpBackends();
  /// Hand the running MCP server to the game so the `mcp grant|revoke|
  /// agent` console commands (registered on first call) can drive its
  /// permission flow — the commands the engine headers assume. Call on
  /// the game thread before the frame loop; pass nullptr on shutdown.
  /// Without a server (no --mcp-stdio) the commands do not exist.
  void setMcpServer(litholattice::mcp::McpServer* server);
  /// The engine projectile manager (P2c 2026-10-13, ENT-1): ticked once
  /// per world tick from updateEntities(), with its voxel seam over the
  /// sim's block reads and its chunk seam over the ChunkStreamer. No
  /// production code spawns projectiles yet (Ping's item-path projectiles
  /// are explicitly out of scope); spawners use this accessor.
  litholattice::ProjectileManager& projectiles() { return projectiles_; }

  /// Completes a pending `screenshot` capture: grabs the armed frame from the
  /// renderer, PNG-encodes it (downscaled for lowres), and writes the file or
  /// files it in the Fragment gallery. The app calls this after
  /// renderer->renderFrame(), before present. No-op without a pending shot.
  void capturePendingScreenshot();
  /// The Fragment's screenshots gallery (Fragment lore page 5). The future
  /// Fragment UI binds to this; `screenshot fragment` captures into it.
  FragmentGallery& fragmentGallery() { return fragmentGallery_; }
  /// The Fragment book's upgrade state (A3 wiring, 2026-10-13, no-decision
  /// subset of brief B1): Game owns the ONE production instance, next to
  /// the gallery it conceptually belongs to. Game thread only (see
  /// fragment_state.h). NOT persisted yet and with NO upgrade trigger —
  /// both are JD decision C3, deliberately not chosen here.
  FragmentState& fragmentState() { return fragmentState_; }
  const FragmentState& fragmentState() const { return fragmentState_; }
  /// The production Fragment text-render path (A3/B1 subset): the markdown
  /// renderer's enable flag is fed from the owned FragmentState's
  /// markdown_unlocked() — never a literal — so when a future JD-approved
  /// trigger (C3) upgrades the Fragment, every render through here follows
  /// with no further wiring. No dialog binds to it yet (also C3).
  FragmentRenderResult renderFragmentText(const std::string& text, FragmentView view) const {
    return renderFragment(text, fragmentState_.markdown_unlocked(), view);
  }

  // Unload the current dimension (sim, chunks, meshes) and load another. Debug: F6.
  bool switchDimension(const std::string& id);
  const std::string& dimensionId() const { return active_->id; }

  // --- Tools / tests surface (headless harness, MCP): the player transform and
  // streaming state. Gameplay code uses player_ / streamer_ directly. ---
  Vec3 playerPosition() const { return player_.pos; }
  void setPlayerPosition(const Vec3& p) { player_.pos = p; }
  /// Locomotion state of the local player body (steps on the sim tick stream).
  litholattice::LocomotionPose playerPose() const { return locomotion_.pose(); }
  bool playerOnGround() const { return locomotion_.onGround(); }
  Vec3 playerVelocity() const { return locomotion_.velocity(); }
  bool playerFlying() const { return locomotion_.flying(); }
  /// The player's canonical inventory (P2b-1): the HUD hotbar, placement,
  /// pickups and player.dat all read/write THIS store — there is no second
  /// hotbar array any more.
  const Inventory& playerInventory() const { return player_.inventory; }
  /// The selected hotbar slot index (0..7) into playerInventory().hotbar().
  int selectedHotbarSlot() const { return hot_; }
  /// Columns in the active dimension's map right now (pre-bake + streamed).
  size_t loadedColumnCount() const { return active_ ? active_->chunks.columns().size() : 0; }
  /// Columns chunk streaming has brought in beyond the pre-bake (0 = none yet).
  size_t streamedColumnCount() const { return streamer_ ? streamer_->streamedCount() : 0; }
  /// Highest non-air y at world column (x, z), or -1 when that column is not
  /// loaded. Race-free: read between ticks through the sim thread.
  int surfaceYAt(int x, int z);

 private:
  void loadActive();
  void handleToggles();
  void handleHotbar();
  /// This frame's movement intent as a locomotion input: move actions
  /// rotated by the player's yaw into world space (the ILocomotion
  /// contract), jump/sneak/sprint/crawl as held states.
  litholattice::LocomotionInput sampleLocomotionInput() const;
  /// `fly` console command + double-tap Jump: toggle (or set, arg on/off)
  /// the locomotion Fly mode. Returns the console message.
  std::string toggleFly(const std::string& arg);
  void handlePause();
  /// Pause state machine (P2b-2, review CH-H7 + G-M2): the ONE owner of
  /// both Game::paused_ and the sim's paused flag. Every pause source —
  /// Esc pause menu, launch menu, tick_freeze/tick_step, handshake
  /// watchdog freeze — transitions through setPauseState(); the sim flag
  /// itself is written ONLY by applySimPause() (single computation:
  /// paused_ && cfg_.pauseSim && !benchSet_), plus the transient
  /// world-query scan pause, whose restore also routes through
  /// applySimPause(). Entering Playing from any frozen state clears a
  /// latched handshake freeze (resetHandshakeAfterFreeze) BEFORE the sim
  /// unpauses, so the authority latch can never re-freeze a sim the game
  /// believes is running.
  void setPauseState(PauseState s);
  void applySimPause();
  void handleWorldActions();
  void setInventoryOpen(bool open);
  /// Block-palette click (creative give): writes a full stack of `b` into
  /// the player inventory's hotbar (selected slot; shift = first free slot).
  void pickInventoryBlock(BlockId b, bool shift);
  /// The block the selected hotbar stack places (kAir when the selected
  /// stack is empty or places nothing — a tool, the Fragment).
  BlockId selectedPlaceBlock() const {
    return placedBlockForItem(player_.inventory.hotbar()[size_t(hot_)].itemId);
  }
  /// Slow 2D side-scrolling dolly across the wafer behind the launch menu (side view,
  /// panning laterally); reuses the player camera the renderer already reads.
  void applyMenuCamera();
  /// A settings control changed: apply the live ones, persist everything.
  void onSettingsChanged(const GameSettings& s);
  void saveSettings();
  /// Push the culling-cache prototype toggles to the renderer (JD 2026-10-11).
  void pushCullProtoConfig();
  /// Feeds this frame's input to the UI. Sets `pointerUsed` if the UI took a click and
  /// `escapeUsed` if it took Escape (so gameplay ignores them).
  void routeUiInput(bool& pointerUsed, bool& escapeUsed);
  /// Input context for UI text entry, theme reloads, HUD model -> widgets, layout.
  void updateUi(float dt, int width, int height);
  HudModel hudModel() const;
  bool editorOpen() const { return editor_ && editor_->isOpen(); }
  void uploadMeshes();
  /// PBR analytic lights (phase 2, Worker 3): marks the emissive cache dirty after a
  /// block edit so the next per-frame refresh picks it up. Called at the game's
  /// block-edit sites (Break/Use/Place); the sim applies edits asynchronously, so
  /// worst-case staleness is one refresh interval - explicit, never silent drift.
  void noteBlockEdit() { if (emissiveProvider_) emissiveProvider_->markDirty(); }
  void playSounds();
  void updateEntities();  ///< spawn from sim events, magnet, tick, pickup (worldTick-driven, ENT-3)
  void updateDeceitors(float dt);  ///< mirror AiWorld Deceitor mobs into render entities (after the AI advance); P2: interpolated legs
  uint32_t nearestDeceitorMob(double maxDist);  ///< nearest director mob to the player (0 = none)
  void reloadResources();
  /// Spring-arm collision query for the camera controller (its ICameraWorld
  /// hook): engine voxel raycast over the active dimension's ChunkMap, read
  /// race-free between sim ticks; on a hit, hitDist gets the exact entry
  /// distance along the (normalized) ray. False = no solid cell in range.
  bool cameraRaycast(Vec3 origin, Vec3 dir, float maxDist, float& hitDist);

  /// Per-frame chunk streaming: request columns around the player and insert
  /// finished ones into the active dimension (sim stopped for the insert).
  void pumpStreaming();
  /// Column eviction (A2 / B-M1): drops resident columns beyond the stream
  /// radius + hysteresis, bounded per call, in its own sim-stopped window
  /// at the end of pumpStreaming(). Dirty or signal-bearing columns are
  /// saved first (WorldSave::saveColumns, SGNL exported before unscan);
  /// a column whose save cannot complete is NOT evicted. Clean,
  /// signal-free columns evict with no save (regenerable from the seed).
  void evictFarColumns();
  /// Retires every sub-chunk mesh of a column being evicted: purges the
  /// pending classic/PBR upload queues, tombstones the upload sequence
  /// entries (cleared again if the column re-streams and re-inserts),
  /// and uploads empty meshes so the renderer and the MCP vision cache
  /// drop theirs. Game thread, sim stopped (eviction window).
  void retireColumnMeshes(int cx, int cz);
  void initScripts();
  std::string targetText() const;
  void beginTyping(int x, int y, int z);
  void endTyping();
  void handleTyping();

  enum class MenuState { Menu, Playing };

  GameConfig cfg_;
  ScriptEngine scripts_;
  GameUi ui_;                          // before uiScripts_ / editor_, which reference it
  UiScriptBindings uiScripts_;
  std::unique_ptr<ScriptEditor> editor_;  // F10; only with a scripts folder
  litholattice::console::ConsoleRegistry consoleRegistry_;  // before debugConsole_
  uint64_t lastScriptTick_ = 0;  // sim world-tick counter for the lithoscript `wait` pump
  FragmentGallery fragmentGallery_;  // the Fragment's screenshots (lore page 5)
  FragmentState fragmentState_;      // the Fragment's upgrade state (A3: production-owned, C3-gated trigger/persistence)
  // A `screenshot` command armed during update(); completed by
  // capturePendingScreenshot() after the next renderFrame(), before present.
  struct PendingShot {
    ScreenshotRequest req;
    std::string path;  // resolved at request time (auto-name or the given path)
  };
  std::optional<PendingShot> pendingShot_;
  std::string requestScreenshot(const ScreenshotRequest& req);
  // First-party lithoscript population (language/population split, JD-locked):
  // settings cvars (live, archived), Decision-9 game commands, world queries.
  std::unique_ptr<SettingsCvars> settingsCvars_;
  std::unique_ptr<GameWorldQuery> worldQuery_;
  std::unique_ptr<litholattice::console::DebugConsole> debugConsole_;
  std::shared_ptr<litholattice::console::ConsoleScriptContext> consoleScriptCtx_;
  std::vector<ui::UiEvent> uiEvents_;
  int themeRevision_ = -1;
  bool uiText_ = false;                // UiText input context pushed
  bool editorWasOpen_ = false;
  IRenderer& renderer_;
  ResourceManager& resources_;
  SoundDirector sound_;
  std::vector<SimEvent> simEvents_;
  Vec3 benchCenter_{};
  std::string benchPath_ = "orbit";
  FramebufferLayout fb_;  // --bench-scene framebuffer placement (flyover path); size 0 = none
  std::unique_ptr<BenchStreamer> stream_;
  std::unique_ptr<ChunkStreamer> streamer_;  // player-centered streaming (real game path)
  double uploadMs_ = 0;
  size_t uploadBytes_ = 0;
  std::unique_ptr<WorldSave> save_;
  Clock::time_point lastSave_ = Clock::now();
  bool benchSet_ = false;
  std::vector<SoundEvent> soundBuf_;
  JobPool& pool_;
  DimensionRegistry registry_;
  Dimension* active_ = nullptr;
  Player player_;
  // --- IPlayer integration (player_adapter.h) ---
  GamePlayerAdapter playerAdapter_;  ///< engine IPlayer view of player_: vitals + stats
  uint64_t lastVitalsTick_ = 0;      ///< sim worldTick the vitals last advanced to
  // --- Locomotion (player_locomotion.h): the local player's Entity body +
  // the engine LocomotionController stepping it, once per sim tick. ---
  PlayerLocomotion locomotion_;
  uint64_t lastLocomotionTick_ = 0;  ///< sim worldTick the body last advanced to
  litholattice::LocomotionInput pendingLocomotionInput_{};  ///< frame's intent, consumed per tick
  // --- Sprite system integration (sprite_host.h): presentation only ---
  SpriteHost spriteHost_;            ///< sprite registry + live instances (game thread)
  uint64_t lastSpriteTick_ = 0;      ///< sim worldTick the sprites last advanced to
  // --- Deceitor director: declared BEFORE aiHost_ so the AiWorld (and
  // the brains it owns) destructs first — brains unregister from the
  // director on destruction, so the director must still be alive then.
  DeceitorDirector deceitorDirector_;
  std::map<uint32_t, uint64_t> deceitorMirrors_;  ///< AiWorld mob id -> mirror entity id
  /// P2 mirror interpolation (presentation only -- the AiWorld cell stays
  /// authoritative; this state is never saved and never feeds the sim).
  /// One leg = the glide from the previous cell centre to the current
  /// one over one AI-tick period; kJump/kGap legs add a parabolic arc.
  struct DeceitorMirrorAnim {
    litholattice::ai::Cell cell{};  ///< cell the mirror is synced to
    double fx = 0, fy = 0, fz = 0;   ///< leg start (world coords)
    double tx = 0, ty = 0, tz = 0;   ///< leg end (cell centre)
    float phase = 1.f;               ///< 0..1 along the leg (1 = arrived)
    litholattice::ai::StepTag tag = litholattice::ai::StepTag::kStepStart;  ///< arriving step's tag
  };
  std::map<uint32_t, DeceitorMirrorAnim> deceitorMirrorAnim_;
  // --- AiWorld integration (ai_host.h): production owner, gated by the
  // ai.enabled cvar (default off). Bound per dimension in loadActive();
  // advanced by elapsed sim world ticks in update() (ENT-2 discipline).
  AiHost aiHost_;
  uint64_t lastAiTick_ = 0;          ///< sim worldTick the AiWorld last advanced to
  Vec3 spawnPoint_{20.f, 92.f, 20.f};  ///< dimension spawn; `kill` respawns here
  litholattice::EntityManager entities_{[this](uint32_t type) {
    return makeEntityBehavior(
        type,
        [this](int x, int y, int z) { return sim_ && blockRegistry().has(sim_->getBlock(x, y, z), kSolid); },
        [this](int x, int y, int z) {
          return sim_ ? blockRegistry().def(sim_->getBlock(x, y, z)).friction : 0.8f;
        });
  }};  ///< item drops etc. (game thread)

  HandoffQueue<ChunkMesh> meshOut_;              // workers -> render thread
  std::unique_ptr<class WaferPulseCache> pulseCache_;  // wafer ground-pulse biome weights (null if no wafer)
  std::deque<int64_t> pendingOrder_;                    // sub-chunks waiting, oldest first
  std::unordered_map<int64_t, ChunkMesh> pendingMesh_;  // newest waiting mesh per sub-chunk (older ones dropped)
  std::unordered_map<int64_t, uint64_t> meshSeq_;  // latest uploaded seq per sub-chunk
  // PBR (BinaryQuad) path queues (phase 2, Worker 3): only used when the mesher option
  // selects BinaryQuad. The classic path above keeps serving every non-cube shape even
  // then (the binary mesher's scope is opaque cubes), so both queues drain each frame.
  HandoffQueue<litholattice::PbrChunkMesh> pbrMeshOut_;              // workers -> render thread
  std::deque<int64_t> pbrPendingOrder_;                             // sub-chunks waiting, oldest first
  std::unordered_map<int64_t, litholattice::PbrChunkMesh> pbrPendingMesh_;  // newest per sub-chunk
  std::unordered_map<int64_t, uint64_t> pbrMeshSeq_;  // latest uploaded PBR seq per sub-chunk
  bool useBinaryMesher_ = false;  // mesher option, boot-applied from settings_.mesher
  // PBR analytic lights (phase 2, Worker 3): the provider scans chunk material bytes
  // around the focus column; the collector reduces to K-nearest per frame for the
  // chunk_pbr uniforms (FrameParams::pbrLights). Null when no material registry.
  std::unique_ptr<litholattice::ChunkEmissiveProvider> emissiveProvider_;
  litholattice::LightCollector lightCollector_;  // default K = 8 (JD's locked decision)
  // PBR light reduction cache (audit App.A/game-1): frameParams() reuses this
  // vector and skips the emissive-cache walk unless the camera column moved or
  // the provider's cache was rebuilt. Mutable: frameParams() is const.
  mutable std::vector<litholattice::AnalyticLight> pbrLightsCache_;
  mutable int pbrLightsCamCx_ = 0, pbrLightsCamCz_ = 0;
  mutable bool pbrLightsValid_ = false;
  SimThread::SubmitMesh submitMesh_;
  std::unique_ptr<SimThread> sim_;               // owns active_->chunks while alive
  uint32_t epoch_ = 0;
  uint64_t initSeq_ = 0;
  int totalSubs_ = 0, meshesReceived_ = 0;
  size_t chunkColumns_ = 0;

  InputSystem& input_;
  AudioEngine& audio_;
  // Camera system: owns the engine CameraManager (player/orbit/follow/free
  // view cameras + MCP pilot cameras). Presentation only — declared before
  // the MCP members because mcpSenses_ borrows its PilotCameraRig.
  GameCameraController camera_;
  // MCP player control (model pilots the local player through the real input
  // path). Wired into the McpServer by Game::mcpBackends() under
  // main.cpp's --mcp-stdio flag (P2c, F-M9).
  McpPlayerController mcpController_;
  McpPlayerView mcpView_;
  // MCP pilot senses (agent vision + agent audio). Fed the mesh cache from
  // uploadMeshes() and cvar values from update(); wired the same way
  // (McpBackends{.vision, .audio}).
  McpSensesAdapter mcpSenses_;
  // MCP server wiring (P2c, F-M9): non-owning; main.cpp owns the server
  // and its stdio thread and clears this (setMcpServer(nullptr)) before
  // destroying either. Only the `mcp` console commands read it, on the
  // game thread; the server's grant/revoke methods are internally
  // synchronized for exactly this cross-thread approval flow.
  litholattice::mcp::McpServer* mcpServer_ = nullptr;
  bool mcpCommandsRegistered_ = false;
  // Projectile system (P2c, ENT-1): engine manager, game-thread owned,
  // ticked per world tick in updateEntities(). Seams are installed in
  // the constructor (voxel over sim block reads, chunk requests over the
  // active dimension's streamer — both resolve the CURRENT sim/streamer
  // at call time, so dimension switches need no rewiring).
  litholattice::ProjectileManager projectiles_;
  std::atomic<bool> mcpControllable_{false};  ///< set per frame in update()
  mutable std::mutex mcpStateMutex_;
  McpPlayerView::GameState mcpPlayerState_;  ///< refreshed per frame in update()
  McpPlayerView::GameState snapshotMcpPlayer() const;
  MenuState menuState_ = MenuState::Menu;
  GameSettings settings_;
  std::function<void(DisplayMode)> displayApplier_;
  float menuCameraT_ = 0.f;                       // cinematic dolly clock
  float menuCamX_ = 0.f, menuCamY_ = 0.f, menuCamZ_ = 0.f;  // dolly anchor (the spawn)
  // Menu entry snapshot (audit MEDIUM): the dolly overwrites player_.pos every
  // menu frame; enterPlay() restores this so save/restore survives the menu.
  Vec3 menuReturnPos_{};
  float menuReturnYaw_ = 0.f, menuReturnPitch_ = 0.f;
  bool menuSnapshotValid_ = false;
  static constexpr int kHotbarSize = kHotbarSlots;   // == kInvHotbarSlots (8)
  // P2b-1: NO separate hotbar array. The hotbar IS player_.inventory's
  // hotbar slots; hot_ is only the selection index into them. (The old
  // array's default contents survive as the fresh-spawn creative kit,
  // seeded as real stacks in the Game constructor.)
  int hot_ = 1;
  bool captured_ = true;
  bool paused_ = false, quit_ = false;  // paused_ mirrors pauseState_ != Playing (owned by the pause machine)
  PauseState pauseState_ = PauseState::Playing;
  bool hsFreezeLogged_ = false;  ///< the current HsFrozen episode was reported once
  uint64_t lastEntitiesTick_ = 0;  ///< sim worldTick the live entities last advanced to (ENT-3)
  bool showControls_ = true;   // H: controls panel
  bool showHud_ = true;        // F1: hide all HUD (like Minecraft)
  bool showDebug_ = true;      // F3: stats overlay
  bool inventoryOpen_ = false; // E: creative block palette
  bool typing_ = false;        // keyboard block typing capture
  int typeX_ = 0, typeY_ = 0, typeZ_ = 0;
  std::string typed_;
  double lastJumpTap_ = -1.0;  // double-tap Jump detection
  float fps_ = 0, frameAvgMs_ = 0, frameMaxMs_ = 0;
};

}  // namespace lithokraft
