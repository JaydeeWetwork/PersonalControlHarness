#include "litholattice/world/column_generator.h"
#include "litholattice/procgen/generator_loader.h"
#include "litholattice/procgen/seed.h"

namespace litholattice {

std::shared_ptr<const ColumnGenerator> ColumnGenerator::create(const std::string& json, uint64_t worldSeed,
                                                               std::string_view dimensionId, const BlockRegistry& blocks,
                                                               std::string& err) {
  return create(json, worldSeed, dimensionId, blocks, err, {});
}

std::shared_ptr<const ColumnGenerator> ColumnGenerator::create(
    const std::string& json, uint64_t worldSeed, std::string_view dimensionId, const BlockRegistry& blocks,
    std::string& err, std::vector<std::unique_ptr<procgen::ISurfaceRule>> extraSurface) {
  const uint64_t dimSeed = procgen::dimensionSeed(worldSeed, dimensionId);
  auto pipeline = procgen::loadGenerator(
      json, dimSeed, [&blocks](std::string_view name) { return resolveMaterial(blocks, name); },
      std::move(extraSurface), err);
  if (!pipeline) return nullptr;
  return std::make_shared<const ColumnGenerator>(Passkey{}, std::move(pipeline), dimSeed, &blocks);
}

std::shared_ptr<const ColumnGenerator> ColumnGenerator::create(
    const std::string& json, uint64_t worldSeed, std::string_view dimensionId, const BlockRegistry& blocks,
    std::string& err, std::vector<std::unique_ptr<procgen::ISurfaceRule>> extraSurface,
    procgen::ModelResolver models) {
  const uint64_t dimSeed = procgen::dimensionSeed(worldSeed, dimensionId);
  auto pipeline = procgen::loadGenerator(
      json, dimSeed, [&blocks](std::string_view name) { return resolveMaterial(blocks, name); },
      std::move(extraSurface), std::move(models), err);
  if (!pipeline) return nullptr;
  return std::make_shared<const ColumnGenerator>(Passkey{}, std::move(pipeline), dimSeed, &blocks);
}

std::optional<procgen::MaterialId> ColumnGenerator::resolveMaterial(const BlockRegistry& blocks, std::string_view name) {
  if (name == "air") return kAir;
  // Namespaced ids ("lithokraft:fossil_trace") resolve to the exact block first: texture keys
  // are shared ("fossil_trace" reuses the "dirt" texture), so a texture-only lookup would
  // return the wrong block for the new ruin blocks.
  if (auto id = blocks.findById(name)) return procgen::MaterialId(*id);
  return blocks.findByTexture(name);
}

std::unique_ptr<ChunkColumn> ColumnGenerator::generate(int cx, int cz, procgen::ColumnMaps* mapsOut) const {
  procgen::ChunkInfo info;
  info.cx = cx;
  info.cz = cz;
  info.size = kChunkSize;
  info.height = kWorldHeight;
  info.dimensionSeed = dimensionSeed_;
  info.chunkSeed = procgen::chunkSeed(dimensionSeed_, cx, cz);
  const procgen::ColumnBuffer buf = pipeline_->generate(info, mapsOut);

  auto col = std::make_unique<ChunkColumn>();
  col->cx = cx;
  col->cz = cz;
  for (int sy = 0; sy < kSubChunks; ++sy) {
    std::unique_ptr<SubChunk> sub;
    for (int ly = 0; ly < kChunkSize; ++ly) {
      const int y = sy * kChunkSize + ly;
      if (buf.layerEmpty(y)) continue;
      for (int z = 0; z < kChunkSize; ++z)
        for (int x = 0; x < kChunkSize; ++x) {
          const procgen::MaterialId m = buf.get(x, y, z);
          if (m == kAir) continue;
          if (!sub) sub = std::make_unique<SubChunk>();
          sub->set(x, ly, z, blocks_->stableIdOf(BlockId(m)));
          sub->state[SubChunk::index(x, ly, z)] = buf.state(x, y, z);
        }
    }
    col->subs[size_t(sy)] = std::move(sub);
  }
  return col;
}

}  // namespace litholattice
