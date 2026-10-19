// Bridge from procgen to chunk columns: a dimension's GeneratorPipeline (built from generator
// JSON, materials named by block texture key) plus the conversion to ChunkColumn sub-chunks.
// The built-in generators are assets/worldgen/*.json, embedded at build time.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "litholattice/procgen/generator_loader.h"
#include "litholattice/procgen/pipeline.h"
#include "litholattice/world/chunk.h"

namespace litholattice {

/// World seed and per-dimension generator JSON overrides (from resource packs).
struct WorldgenSettings {
  uint64_t seed = 0;
  /// Dimension id -> generator JSON text. Missing or invalid entries use the built-in.
  std::map<std::string, std::string, std::less<>> generators;
};

class ColumnGenerator {
 public:
  /// Construction token: only ColumnGenerator's own factory can make one, so it can use make_unique / make_shared.
  class Passkey {
    friend class ColumnGenerator;
    Passkey() = default;
  };
  ColumnGenerator(Passkey, std::unique_ptr<const procgen::GeneratorPipeline> p, uint64_t dimensionSeed,
                  const BlockRegistry* blocks)
      : pipeline_(std::move(p)), dimensionSeed_(dimensionSeed), blocks_(blocks) {}
  /// Builds a dimension's generator from JSON. Noise is seeded with
  /// procgen::dimensionSeed(worldSeed, dimensionId). nullptr and `err` on failure.
  /// Worldgen material names are block texture keys (plus "air"), resolved against `blocks`.
  static std::shared_ptr<const ColumnGenerator> create(const std::string& json, uint64_t worldSeed,
                                                       std::string_view dimensionId, const BlockRegistry& blocks,
                                                       std::string& err);
  /// Same, plus `extraSurface` rules appended after the JSON-configured surface rules (see
  /// procgen::loadGenerator). For rules that place block ids directly, which JSON material
  /// names cannot express.
  static std::shared_ptr<const ColumnGenerator> create(
      const std::string& json, uint64_t worldSeed, std::string_view dimensionId, const BlockRegistry& blocks,
      std::string& err, std::vector<std::unique_ptr<procgen::ISurfaceRule>> extraSurface);
  /// Same, plus a `models` resolver for "lithoform_stamp" features (see procgen::loadGenerator).
  /// When null, that feature type fails to load with a clear error.
  static std::shared_ptr<const ColumnGenerator> create(
      const std::string& json, uint64_t worldSeed, std::string_view dimensionId, const BlockRegistry& blocks,
      std::string& err, std::vector<std::unique_ptr<procgen::ISurfaceRule>> extraSurface,
      procgen::ModelResolver models);
  /// Maps a worldgen material name (block texture key, or "air") to a block id of `blocks`.
  static std::optional<procgen::MaterialId> resolveMaterial(const BlockRegistry& blocks, std::string_view name);

  /// Generates column (cx, cz); all-air sub-chunks stay null. Thread-safe.
  /// With `mapsOut`, also returns the pipeline's per-column maps (height, climate, biome).
  /// `mapsOut->biome[x + z * kChunkSize]` is the biome-table index for that column (the order
  /// of the generator JSON's "biomes" array) -- the deterministic per-column biome id that
  /// sibling systems (trees, ruins, pulse) query through this overload.
  std::unique_ptr<ChunkColumn> generate(int cx, int cz, procgen::ColumnMaps* mapsOut = nullptr) const;
  uint64_t dimensionSeed() const { return dimensionSeed_; }
  const procgen::GeneratorPipeline& pipeline() const { return *pipeline_; }

 private:
  std::unique_ptr<const procgen::GeneratorPipeline> pipeline_;
  uint64_t dimensionSeed_;
  // Registry for translating worldgen material ids (BlockIds) to the StableId
  // currency of sub-chunk voxel storage. Set by create(); owned by the game.
  const BlockRegistry* blocks_ = nullptr;
};

}  // namespace litholattice
