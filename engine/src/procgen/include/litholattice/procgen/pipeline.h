// Generator pipeline: the stage interfaces and the pipeline that runs them for one chunk column.
// procgen knows nothing about blocks; stages write numeric material ids (0 = air) that the
// world maps to its block ids. Order per column:
//
//   climate -> biome selection -> terrain (density / heightmap) -> surface rules -> carvers -> features
//
// generate() is const and thread-safe: every call owns its buffers, graph context and RNGs, and
// all randomness derives from ChunkInfo::chunkSeed, so results do not depend on thread or order.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "litholattice/procgen/graph.h"
#include "litholattice/procgen/seed.h"

namespace litholattice::procgen {

using MaterialId = uint16_t;
inline constexpr MaterialId kAirMaterial = 0;

/// Where and what to generate.
struct ChunkInfo {
  int cx = 0, cz = 0;           ///< chunk coordinate
  int size = 16;                ///< columns per side
  int height = 256;             ///< world height in blocks
  uint64_t dimensionSeed = 0;   ///< dimensionSeed(worldSeed, dimension)
  uint64_t chunkSeed = 0;       ///< chunkSeed(dimensionSeed, cx, cz)

  int worldX(int x) const { return cx * size + x; }
  int worldZ(int z) const { return cz * size + z; }
};

/// Materials and per-block state for one chunk column, index (y * size + z) * size + x.
class ColumnBuffer {
 public:
  ColumnBuffer(int size, int height)
      : size_(size), height_(height), material_(size_t(size) * size * height, kAirMaterial),
        state_(material_.size(), 0) {}

  int size() const { return size_; }
  int height() const { return height_; }
  bool contains(int x, int y, int z) const { return x >= 0 && z >= 0 && y >= 0 && x < size_ && z < size_ && y < height_; }
  size_t index(int x, int y, int z) const { return (size_t(y) * size_t(size_) + size_t(z)) * size_t(size_) + size_t(x); }

  MaterialId get(int x, int y, int z) const { return material_[index(x, y, z)]; }
  void set(int x, int y, int z, MaterialId m) { material_[index(x, y, z)] = m; }
  uint8_t state(int x, int y, int z) const { return state_[index(x, y, z)]; }
  void setState(int x, int y, int z, uint8_t s) { state_[index(x, y, z)] = s; }
  /// True when layer y holds no solid material (lets callers skip empty sections).
  bool layerEmpty(int y) const;

 private:
  int size_, height_;
  std::vector<MaterialId> material_;
  std::vector<uint8_t> state_;
};

/// Climate parameters, each nominally in [0, 1].
enum class ClimateParam { Temperature, Humidity, Continentalness, Erosion };
inline constexpr size_t kClimateParamCount = 4;
using ClimatePoint = std::array<float, kClimateParamCount>;

/// Per-column (x + z * size) maps shared between stages.
struct ColumnMaps {
  std::vector<int> height;                 ///< highest solid y, or -1 (filled by terrain)
  std::vector<ClimatePoint> climate;       ///< filled by the climate stage
  std::vector<uint8_t> biome;              ///< dominant biome index (filled by the biome selector)
  /// Biome blending weights (filled by the biome selector when it blends): row-major
  /// `columns * biomeCount` floats; the weight of biome b at column i is
  /// biomeWeight[i * biomeCount + b]. Weights are >= 0 and usually sum to ~1 per column.
  /// Empty with biomeCount == 0: no blending data; the dominant biome then has weight 1.
  /// (Integration contract for the biome blender: fill these alongside `biome`.)
  std::vector<float> biomeWeight;
  size_t biomeCount = 0;                   ///< biomes in the weight vector; 0 = no blending data
};

/// Everything a stage may read or write for the current chunk.
struct StageContext {
  const ChunkInfo& chunk;
  ColumnBuffer& blocks;
  ColumnMaps& maps;
  GraphContext& graph;     ///< bound to `columns` for the whole run; stages must not rebind it
  const PointsView& columns;  ///< column positions (x, 0, z), index x + z * size
};

/// Fills ColumnMaps::climate.
class IClimateStage {
 public:
  virtual ~IClimateStage() = default;
  virtual void sample(StageContext& ctx) const = 0;
};

class GeneratorPipeline;  // defined below; IFeature::onPipelineBuilt takes it

/// Picks a biome per column from its climate; also owns the biome table.
class IBiomeSelector {
 public:
  virtual ~IBiomeSelector() = default;
  virtual uint8_t select(const ClimatePoint& climate) const = 0;
  /// Biomes in the table (0 if unknown). Sizes ColumnMaps::biomeWeight.
  virtual size_t biomeCount() const { return 0; }
  /// Blending weights for one column, into `out` (sized to biomeCount()). The default puts
  /// weight 1 on select() — no blending. A blending selector overrides this; decoration
  /// density scales with these weights (effective density = sum_b density[b] * weight_b),
  /// so a feature eligible in one biome fades out smoothly across the blend boundary.
  virtual void blendWeights(const ClimatePoint& climate, float* out) const {
    const uint8_t dom = select(climate);
    for (size_t b = 0, n = biomeCount(); b < n; ++b) out[b] = (b == dom) ? 1.f : 0.f;
  }
};

/// Post-processes the biome map after selection, before terrain. Transition bands are the
/// canonical use: rewrite ColumnMaps::biome (and biomeWeight) for columns near biome
/// borders. Runs inside generate(); bound to pipeline services via onPipelineBuilt like
/// IFeature/ICarver.
class IBiomePostStage {
 public:
  virtual ~IBiomePostStage() = default;
  virtual void process(StageContext& ctx) const = 0;
  /// Called once the pipeline is fully built (by GeneratorPipeline::Builder::build()).
  /// Default: no-op.
  virtual void onPipelineBuilt(const GeneratorPipeline* pipeline) { (void)pipeline; }
};

/// One mob's spawn entry in a biome's fauna table. Data for the entity system to read
/// later -- procgen never spawns anything itself. Tables come from generator JSON
/// ("fauna": {"<biome>": [{"mob": ..., "weight": ..., "pack": [min, max]}]}).
struct FaunaEntry {
  std::string mob;          ///< mob id, e.g. "lithokraft:skitter"
  float weight = 1.f;       ///< relative spawn weight within the biome
  int minPack = 1, maxPack = 4;  ///< pack size range
};

/// Places the base terrain and fills ColumnMaps::height.
class ITerrainStage {
 public:
  virtual ~ITerrainStage() = default;
  virtual void generate(StageContext& ctx) const = 0;
};

/// Replaces the top of the terrain (grass, sand, ...) per biome.
class ISurfaceRule {
 public:
  virtual ~ISurfaceRule() = default;
  virtual void apply(StageContext& ctx) const = 0;
};

/// Removes material (caves, ravines). May reach in from neighbouring chunks.
class ICarver {
 public:
  virtual ~ICarver() = default;
  virtual void carve(StageContext& ctx) const = 0;
  /// Called once the pipeline is fully built (by GeneratorPipeline::Builder::build(), so
  /// every pipeline gets it automatically), letting a carver bind pipeline services it
  /// cannot take at construction time. RiverCarver uses it to bind its biome probe.
  /// Default: no-op.
  virtual void onPipelineBuilt(const GeneratorPipeline* pipeline) { (void)pipeline; }
};

/// Dominant biome-table index at a world column; bound to the finished pipeline by
/// onPipelineBuilt. Same signature as StructureFeature::BiomeProbe.
using BiomeProbe = std::function<uint8_t(int32_t wx, int32_t wz)>;

/// Places a feature (ores, structures) using an RNG seeded for this chunk and feature.
class IFeature {
 public:
  virtual ~IFeature() = default;
  virtual void place(StageContext& ctx, Rng& rng) const = 0;
  /// Called once the pipeline is fully built (by GeneratorPipeline::Builder::build(), so
  /// every pipeline gets it automatically), letting a feature bind pipeline services it
  /// cannot take at construction time. StructureFeature uses it to bind its dominant-biome
  /// probe to the finished pipeline. Default: no-op.
  virtual void onPipelineBuilt(const GeneratorPipeline* pipeline) { (void)pipeline; }
};

/// Named features in placement order. Each feature's RNG seed is
/// deriveSeed(chunkSeed, nameHash(name)), so adding a feature does not shift the others.
class FeatureRegistry {
 public:
  /// Appends a feature; returns false if the name is already taken.
  bool add(std::string name, std::unique_ptr<IFeature> feature);
  bool contains(std::string_view name) const;
  size_t size() const { return features_.size(); }
  void placeAll(StageContext& ctx) const;
  /// Visits every feature in placement order.
  void forEach(const std::function<void(std::string_view name, IFeature& feature)>& fn);
  void forEach(const std::function<void(std::string_view name, const IFeature& feature)>& fn) const;

 private:
  struct Entry {
    std::string name;
    std::unique_ptr<IFeature> feature;
  };
  std::vector<Entry> features_;
};

/// A configured generator: one graph plus stages. Build with GeneratorPipeline::Builder or from
/// JSON (generator_loader.h). Immutable once built.
class GeneratorPipeline {
 public:
  class Builder;
  /// Construction token: only GeneratorPipeline's own factory can make one, so it can use make_unique / make_shared.
  class Passkey {
    friend class GeneratorPipeline;
    friend class Builder;
    Passkey() = default;
  };
  explicit GeneratorPipeline(Passkey) {}

  /// Generates one chunk column. Thread-safe.
  ColumnBuffer generate(const ChunkInfo& chunk, ColumnMaps* mapsOut = nullptr) const;
  const NoiseGraph& graph() const { return *graph_; }
  /// Dominant biome-table index at one world column, without generating a chunk: samples
  /// the climate stage at (wx, wz) and runs the biome selector. A pure function of the
  /// pipeline seed and the position; agrees with ColumnMaps::biome for generated columns.
  /// 0 when the pipeline has no biome selector. Thread-safe.
  uint8_t biomeAt(int32_t wx, int32_t wz) const;
  /// Batched biomeAt: fills `out[i]` with the dominant biome at (xs[i], zs[i]). One graph
  /// binding for all points (far cheaper per point than biomeAt in a loop). Thread-safe.
  void sampleBiomes(const int32_t* xs, const int32_t* zs, size_t n, uint8_t* out) const;
  /// Visits every named feature in placement order. Thread-safe.
  void visitFeatures(const std::function<void(std::string_view name, const IFeature& feature)>& fn) const;
  /// Fauna spawn table for one biome index; empty when the biome has no table. Thread-safe.
  const std::vector<FaunaEntry>& faunaForBiome(uint8_t biome) const;
  /// Fauna entries for the dominant biome at a world column (biomeAt + table lookup).
  /// Thread-safe. The entity system's spawn hook: pure function of (seed, position).
  std::vector<FaunaEntry> faunaAt(int32_t wx, int32_t wz) const;
  /// Biome post-stages in run order (transition bands, ...). Thread-safe to read.
  const std::vector<std::unique_ptr<IBiomePostStage>>& biomePostStages() const { return biomePost_; }

 private:
  std::shared_ptr<const NoiseGraph> graph_;
  std::unique_ptr<IClimateStage> climate_;
  std::unique_ptr<IBiomeSelector> biomes_;
  std::unique_ptr<ITerrainStage> terrain_;
  std::vector<std::unique_ptr<IBiomePostStage>> biomePost_;
  std::vector<std::unique_ptr<ISurfaceRule>> surface_;
  std::vector<std::unique_ptr<ICarver>> carvers_;
  FeatureRegistry features_;
  std::vector<std::vector<FaunaEntry>> fauna_;  ///< per-biome spawn tables; empty = none
};

/// Assembles a pipeline; graph and terrain are required, every other stage is optional.
class GeneratorPipeline::Builder {
 public:
  explicit Builder(std::shared_ptr<const NoiseGraph> graph);
  Builder& climate(std::unique_ptr<IClimateStage> s);
  Builder& biomes(std::unique_ptr<IBiomeSelector> s);
  Builder& terrain(std::unique_ptr<ITerrainStage> s);
  /// Biome post-stage (transition bands, ...): runs after biome selection, before terrain.
  Builder& biomePost(std::unique_ptr<IBiomePostStage> s);
  Builder& surface(std::unique_ptr<ISurfaceRule> s);
  Builder& carver(std::unique_ptr<ICarver> s);
  /// Per-biome fauna spawn tables, indexed by biome-table index. Replaces any previous.
  Builder& fauna(std::vector<std::vector<FaunaEntry>> tables);
  /// Returns false (and leaves the registry unchanged) on a duplicate name.
  bool feature(std::string name, std::unique_ptr<IFeature> f);
  /// nullptr if the terrain stage is missing.
  std::unique_ptr<const GeneratorPipeline> build();

 private:
  std::unique_ptr<GeneratorPipeline> p_;
};

}  // namespace litholattice::procgen
