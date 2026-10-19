// Headless game harness implementation.
#include "headless_harness.h"
#include "litholattice/world/chunk.h"  // packPos for the mesh key
#include "lithokraft/game/game.h"

namespace lithokraft {

void NullRenderer::uploadChunkMesh(const litholattice::ChunkMesh& mesh) {
  const int64_t k = litholattice::packPos(mesh.cx, mesh.sy, mesh.cz);
  if (mesh.verts.empty())
    meshes_.erase(k);
  else
    meshes_[k] = mesh.verts.size() * sizeof(litholattice::PackedVertex);
}

litholattice::RenderStats NullRenderer::stats() const {
  return {uint32_t(meshes_.size()), 0};
}

size_t NullRenderer::bytes() const {
  size_t b = 0;
  for (const auto& [k, v] : meshes_) b += v;
  return b;
}

HeadlessGame::HeadlessGame() : HeadlessGame(Config{}) {}

HeadlessGame::HeadlessGame(const Config& cfg)
    : pool_(cfg.threads > 0 ? cfg.threads : 1),
      resources_({}),  // no packs: built-in worldgen fallbacks, like the demo
      input_(defaultActionMap(), inputContexts()) {
  GameConfig gc;
  gc.dimension = "wafer";
  gc.seed = cfg.seed;
  gc.worldChunks = cfg.worldChunks;
  gc.version = cfg.version;
  gc.saveDir = cfg.saveDir;  // empty unless a persistence test asks for a save dir
  // scriptsDir / settingsPath stay empty: no scripts, no settings persistence.
  game_ = std::make_unique<Game>(gc, renderer_, pool_, resources_, audio_, input_);
}

HeadlessGame::~HeadlessGame() = default;

void HeadlessGame::frame(float dt, int w, int h) { game_->update(dt, w, h); }

}  // namespace lithokraft
