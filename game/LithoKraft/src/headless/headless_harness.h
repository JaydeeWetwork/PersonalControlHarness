// Headless game harness: constructs the full LithoKraft Game without a window, GPU, or
// audio device, so the game is testable in CI (this VM has no GPU).
//
// This is the general game-testing fixture, not a one-off runner: future gameplay tests
// (inventory, combat, dimensions, lithography) live here. Tests drive the REAL Game and the
// REAL retained UI (GameUi) — clicks go through UiRoot exactly like the app's event loop —
// so a passing harness test means the wiring works, not just the widgets.
//
// Note (G-L3, corrected 2026-10-13): earlier versions of this comment claimed
// the shipped content registry was desynced and the shipped game could not
// start (a row-count guard in blocks.cpp calling std::abort()). That guard was
// removed by the resolveId registry refactor and the claim is FALSE today:
// the registry resolves all its constants in order, and the shipped game
// starts. The harness links a verbatim scratch copy of blocks.cpp (see
// src/headless/CMakeLists.txt + patch_blocks.cmake) purely so its binaries
// get the registry from an object in their own target. The tree is untouched.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "litholattice/audio/audio_engine.h"
#include "litholattice/core/threading.h"
#include "litholattice/input/input_system.h"
#include "litholattice/render/renderer.h"
#include "litholattice/render/texture_array.h"
#include "lithokraft/game/actions.h"
#include "lithokraft/game/resource_manager.h"

namespace lithokraft {

class Game;

/// No-op IRenderer for headless testing. Records uploaded chunk-mesh bytes by sub-chunk key
/// (what a real backend would keep alive) so tests can assert the mesh pipeline ran;
/// renderFrame() is a no-op. Mirrors the NullRenderer in app/perf_headless.cpp.
class NullRenderer final : public litholattice::IRenderer {
 public:
  struct NullTextures final : public litholattice::ITextureArray {
    bool upload(const litholattice::TextureRegistry&) override { return true; }
    int layerCount() const override { return 0; }
    int tileSize() const override { return 0; }
  };

  const char* name() const override { return "null"; }
  std::string deviceInfo() const override { return "null (headless harness)"; }
  litholattice::ITextureArray& textureArray() override { return tex_; }
  void setMaterials(const litholattice::BlockMaterials&) override {}
  int loadShaders(const litholattice::ShaderLibrary&) override { return 0; }
  void uploadChunkMesh(const litholattice::ChunkMesh& mesh) override;
  void uploadEntityMesh(const std::vector<litholattice::PackedVertex>&) override {}
  void clearChunkMeshes() override { meshes_.clear(); }
  void renderFrame(const litholattice::FrameParams&) override {}
  litholattice::RenderStats stats() const override;
  /// Total mesh bytes currently held.
  size_t bytes() const;

 private:
  NullTextures tex_;
  std::unordered_map<int64_t, size_t> meshes_;
};

/// Owns everything a Game needs and constructs it headless, in main.cpp's order:
/// JobPool, NullRenderer, ResourceManager (no packs; built-in worldgen fallbacks),
/// AudioEngine (no output device; commands queue harmlessly), InputSystem (default action
/// map, no backend). Members declare in construction order; destruction runs in reverse,
/// so the Game dies before the things that must outlive it.
class HeadlessGame {
 public:
  struct Config {
    int worldChunks = 4;    ///< small world: fast load, plenty for state-machine tests
    uint64_t seed = 0x1e55; ///< fixed seed: repeatable worlds
    std::string version = "headless-test";
    int threads = 2;
    std::string saveDir;    ///< empty = no saving (default); set for persistence tests
  };

  HeadlessGame();  // default Config{}
  explicit HeadlessGame(const Config& cfg);
  ~HeadlessGame();
  HeadlessGame(const HeadlessGame&) = delete;
  HeadlessGame& operator=(const HeadlessGame&) = delete;

  Game& game() { return *game_; }
  const Game& game() const { return *game_; }
  NullRenderer& renderer() { return renderer_; }
  /// Advance the game one frame on a dummy 1280x720 viewport (no window).
  void frame(float dt = 1.f / 60.f, int w = 1280, int h = 720);

 private:
  litholattice::JobPool pool_;
  NullRenderer renderer_;
  ResourceManager resources_;
  litholattice::AudioEngine audio_;
  litholattice::InputSystem input_;
  std::unique_ptr<Game> game_;
};

/// Runs every headless harness self-test. Returns the failure count (0 = all pass).
/// Prints "[PASS]/[FAIL] headless: ..." lines in the repo's self-test style.
int runHeadlessHarnessSelfTest();

}  // namespace lithokraft
