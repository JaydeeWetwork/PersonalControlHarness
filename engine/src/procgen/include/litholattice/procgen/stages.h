// Built-in pipeline stages. Each is small and configured by value; generator_loader.h builds
// them from JSON. Graph-driven stages hold the pipeline's NoiseGraph and an output index.
#pragma once
#include <algorithm>
#include <array>
#include <climits>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "litholattice/procgen/litho_formula.h"
#include "litholattice/procgen/pipeline.h"

namespace litholattice::procgen {

// ---- climate

/// Climate parameters read from graph outputs; parameters without an output use `fallback`.
class GraphClimate final : public IClimateStage {
 public:
  /// outputs[p] = graph output index for parameter p, or -1.
  GraphClimate(std::shared_ptr<const NoiseGraph> graph, std::array<int, kClimateParamCount> outputs, float fallback = 0.5f);
  void sample(StageContext& ctx) const override;

 private:
  std::shared_ptr<const NoiseGraph> graph_;
  std::array<int, kClimateParamCount> outputs_;
  float fallback_;
};

// ---- biomes

/// A biome's position in climate space. Parameters with weight 0 are ignored.
struct BiomePoint {
  std::string name;
  ClimatePoint point{0.5f, 0.5f, 0.5f, 0.5f};
  ClimatePoint weight{1.f, 1.f, 1.f, 1.f};
};

/// Multi-noise selection: the biome whose point is nearest (weighted squared distance) to the
/// column's climate. Ties go to the earlier biome.
class MultiNoiseBiomeSelector final : public IBiomeSelector {
 public:
  explicit MultiNoiseBiomeSelector(std::vector<BiomePoint> biomes);
  uint8_t select(const ClimatePoint& climate) const override;
  size_t biomeCount() const override { return biomes_.size(); }
  const std::vector<BiomePoint>& biomes() const { return biomes_; }

 private:
  std::vector<BiomePoint> biomes_;
};

/// Minecraft-style biome blending: per column, the `blendCount` nearest biomes in climate
/// space get smooth weights -- a softmax over the distance gap to the nearest kept biome,
/// renormalized -- so biome edges read as gradients, not walls. Smooth everywhere:
/// the nearest biome always leads, the rest fade with their gap, so there is no
/// "past the radius" regime and no hard fallback. `blendRadius` is the softness scale
/// for the gap (smaller = purer biome interiors, narrower borders). Fills
/// ColumnMaps::biomeWeight; the dominant (nearest) biome still goes into ColumnMaps::biome.
/// Deterministic: a pure function of the climate point. General system: any dimension opts
/// in through its generator JSON ("biomes": {"selector": {"type": "blended", ...}}).
class BlendedBiomeSelector final : public IBiomeSelector {
 public:
  /// blendRadius: climate-space distance with nonzero weight (> 0). blendCount: biomes kept
  /// per column, clamped to 2..4 by the loader.
  BlendedBiomeSelector(std::vector<BiomePoint> biomes, float blendRadius, int blendCount);
  uint8_t select(const ClimatePoint& climate) const override;
  size_t biomeCount() const override { return biomes_.size(); }
  void blendWeights(const ClimatePoint& climate, float* out) const override;
  const std::vector<BiomePoint>& biomes() const { return biomes_; }

 private:
  std::vector<BiomePoint> biomes_;
  float blendRadius_;
  int blendCount_;
};

// ---- terrain

/// Solid from floor(graph output) down `depth` blocks (0 = down to y 0).
class HeightmapTerrain final : public ITerrainStage {
 public:
  HeightmapTerrain(std::shared_ptr<const NoiseGraph> graph, int output, MaterialId material, int depth = 0);
  void generate(StageContext& ctx) const override;

 private:
  std::shared_ptr<const NoiseGraph> graph_;
  int output_;
  MaterialId material_;
  int depth_;
};

/// 3D density: solid where the graph output is > 0, evaluated for y in [minY, maxY]; solid
/// below minY, air above maxY. Allows overhangs and floating terrain.
class DensityTerrain final : public ITerrainStage {
 public:
  DensityTerrain(std::shared_ptr<const NoiseGraph> graph, int output, MaterialId material, int minY, int maxY);
  void generate(StageContext& ctx) const override;

 private:
  std::shared_ptr<const NoiseGraph> graph_;
  int output_;
  MaterialId material_;
  int minY_, maxY_;
};

/// Per-biome heightmap: each biome selects its own height output and base material; the
/// column's biome (ColumnMaps::biome, filled by the biome selector before terrain runs) picks
/// the entry. Entries must cover every biome index the selector can produce; the generator
/// loader enforces one entry per biome. Solid from floor(graph output) down `depth` blocks
/// (0 = down to y 0), exactly like HeightmapTerrain.
///
/// Mountain support: an entry may name an extra `uplift` graph output (e.g. a `mountain_range`
/// or `peak` node) scaled by `upliftAmplitude` blocks and added to the biome's base height
/// *before* biome blending. The uplift therefore rides the existing weight-averaged height
/// path: biome borders stay smooth gradients (no cliffs) unless the biome weights are one-hot
/// there. Omit `uplift` (the default) for flat biomes.
class BiomeHeightmapTerrain final : public ITerrainStage {
 public:
  struct Entry {
    int output;                 ///< graph output index for this biome's height
    MaterialId material;        ///< base material for this biome's crust
    int upliftOutput = -1;      ///< optional extra graph output added to the height; -1 = none
    float upliftAmplitude = 1.f;  ///< scales the uplift output (blocks)
  };
  BiomeHeightmapTerrain(std::shared_ptr<const NoiseGraph> graph, std::vector<Entry> perBiome, int depth = 0);
  void generate(StageContext& ctx) const override;
  size_t biomeCount() const { return perBiome_.size(); }

 private:
  std::shared_ptr<const NoiseGraph> graph_;
  std::vector<Entry> perBiome_;
  int depth_;
};

// ---- surface

/// Top block and the filler beneath it, for one biome.
struct SurfaceLayers {
  MaterialId top = kAirMaterial;     ///< kAirMaterial: leave the terrain material
  MaterialId filler = kAirMaterial;
  int fillerDepth = 0;
};

/// Rewrites the top of each column with its biome's layers. With `topStateIsBiome`, the top
/// block's state byte is the biome index (tints grass per biome).
class LayeredSurface final : public ISurfaceRule {
 public:
  /// ditherSeed salts the weight-proportioned palette dither (hash of column position).
  /// With unblended (one-hot) weights the dither always picks the dominant biome, so this
  /// is exactly the old behaviour.
  LayeredSurface(std::vector<SurfaceLayers> perBiome, bool topStateIsBiome, uint64_t ditherSeed = 0);
  void apply(StageContext& ctx) const override;

 private:
  std::vector<SurfaceLayers> perBiome_;
  bool topStateIsBiome_;
  uint64_t ditherSeed_;
};

// ---- carvers

struct WormCaveSettings {
  float chance = 0.25f;          ///< probability that a chunk starts a worm
  int minLength = 40, maxLength = 90;
  float minRadius = 1.5f, maxRadius = 3.f;
  int minY = 8, maxY = 48;       ///< start height range
  int surfaceMargin = 6;         ///< keep this many blocks below the surface intact
  std::vector<float> density;    ///< per-biome start density multiplier; missing entries = 1
  float branchChance = 0.f;      ///< per-worm fork probability; 0 = legacy unbranched worms
  int maxBranches = 2;           ///< fork attempts per worm (geometric count, capped)
  float branchLengthFrac = 0.5f; ///< branch walk length as a fraction of the parent's
  float branchRadiusFrac = 0.75f; ///< branch radius as a fraction of the parent's
};

/// Perlin-worm caves: random walks that wander across chunk borders. Each chunk replays the
/// worms of every chunk within reach (seeded from that chunk), so tunnels line up seamlessly.
/// Per-biome density needs the pipeline's biome table: it is bound by onPipelineBuilt (every
/// Builder::build pipeline does this); without a bound pipeline the density table is ignored.
class WormCaveCarver final : public ICarver {
 public:
  /// Dominant biome-table index at a world column; bound to the pipeline by onPipelineBuilt.
  using BiomeProbe = std::function<uint8_t(int32_t wx, int32_t wz)>;
  explicit WormCaveCarver(WormCaveSettings s);
  void carve(StageContext& ctx) const override;
  void onPipelineBuilt(const GeneratorPipeline* pipeline) override;
  void setBiomeProbe(BiomeProbe probe) { probe_ = std::move(probe); }

 private:
  WormCaveSettings s_;
  BiomeProbe probe_;
};

/// Long surface gashes that cut down from the terrain surface: a meandering path pinched at
/// both ends, deepest and widest in the middle, with a V-shaped cross-section (sloped walls).
/// Cross-chunk discipline mirrors WormCaveCarver: each chunk replays the ravines of every
/// chunk within reach, seeded from that chunk, so gashes line up seamlessly. Ravines open at
/// the surface by design, so surfaceMargin does not apply; bottomMargin floors the cut.
struct RavineSettings {
  float chance = 0.0625f;        ///< per-chunk ravine-start probability (1/16)
  int minLength = 64, maxLength = 128;
  float minWidth = 3.f, maxWidth = 8.f;   ///< half-width at the surface
  float minDepth = 16.f, maxDepth = 32.f;  ///< centerline cut depth
  float meander = 0.06f;         ///< max heading change per step (radians)
  int bottomMargin = 4;          ///< never carve below this y
  std::vector<float> density;    ///< per-biome density multiplier; missing entries = 1
};

class RavineCarver final : public ICarver {
 public:
  /// Dominant biome-table index at a world column; bound to the pipeline by onPipelineBuilt.
  using BiomeProbe = std::function<uint8_t(int32_t wx, int32_t wz)>;
  RavineCarver();  // defaults, so {"type": "ravine"} keeps working
  explicit RavineCarver(RavineSettings s);
  void carve(StageContext& ctx) const override;
  void onPipelineBuilt(const GeneratorPipeline* pipeline) override;
  void setBiomeProbe(BiomeProbe probe) { probe_ = std::move(probe); }

 private:
  RavineSettings s_;
  BiomeProbe probe_;
};

/// 3D-noise pocket caverns: large chambers on a coarse cell lattice (at most one candidate
/// per cell), each an ellipsoid with noise-wobbled walls -- a guaranteed solid core plus a rim
/// where a smooth value-noise field decides the boundary. Cross-chunk discipline mirrors
/// WormCaveCarver: each chunk replays the cells within reach, seeded per cell, so chambers
/// line up seamlessly. Per-biome density is bound by onPipelineBuilt like the worm carver.
struct CavernSettings {
  int cellSize = 64;             ///< coarse lattice spacing in blocks
  float chance = 0.25f;          ///< per-cell chamber probability
  int minRadius = 8, maxRadius = 16;
  float squashY = 0.5f;          ///< vertical squash of the chamber ellipsoid
  int minY = 8, maxY = 48;       ///< chamber center height range
  int surfaceMargin = 6;         ///< keep this many blocks below the surface intact
  float noiseFreq = 0.125f;      ///< 3D wall-wobble noise frequency (1/8 block)
  float coreFrac = 0.75f;        ///< inner ellipsoid fraction always carved; noise shapes only the rim
  std::vector<float> density;    ///< per-biome density multiplier; missing entries = 1
};

class CavernCarver final : public ICarver {
 public:
  /// Dominant biome-table index at a world column; bound to the pipeline by onPipelineBuilt.
  using BiomeProbe = std::function<uint8_t(int32_t wx, int32_t wz)>;
  explicit CavernCarver(CavernSettings s);
  void carve(StageContext& ctx) const override;
  void onPipelineBuilt(const GeneratorPipeline* pipeline) override;
  void setBiomeProbe(BiomeProbe probe) { probe_ = std::move(probe); }

 private:
  CavernSettings s_;
  BiomeProbe probe_;
};

/// "Swiss-cheese" caves (JD's name for this family): a 3D noise density-field carver
/// (worldgen completeness, 2026-10-13; clean-room). There is no path and no chamber
/// lattice: a seeded FBm density field is evaluated at every block, and a block is
/// carved where the field crosses `threshold`. Where the field stays below the
/// threshold the rock remains -- those uncarved pillars/columns are what make the
/// caverns read as cheese holes rather than a hollowed shell. The field is a pure
/// function of (dimensionSeed, world position), so the carve is position-stable and
/// chunk-border seamless by construction, like the water rules. Per-biome density
/// gating uses the same column-weight discipline as the other carvers: the field is
/// only evaluated for columns whose dominant biome passes the density table (empty
/// table = no gating).
struct CheeseSettings {
  float threshold = 0.30f;       ///< carve where the density field exceeds this, [-0.9, 0.9]
  float frequency = 1.f / 24.f;  ///< density-field scale (~24-block cavern features)
  int minY = 6;                  ///< never carve below this y
  int surfaceMargin = 8;         ///< keep this many blocks below the surface intact
  std::vector<float> density;    ///< per-biome carve gate multiplier; missing entries = 1
};

class CheeseCarver final : public ICarver {
 public:
  using BiomeProbe = std::function<uint8_t(int32_t wx, int32_t wz)>;
  explicit CheeseCarver(CheeseSettings s);
  void carve(StageContext& ctx) const override;
  void onPipelineBuilt(const GeneratorPipeline* pipeline) override;
  void setBiomeProbe(BiomeProbe probe) { probe_ = std::move(probe); }
  const CheeseSettings& settings() const { return s_; }

 private:
  CheeseSettings s_;
  BiomeProbe probe_;
};

/// Noise-tube caves (worldgen completeness, 2026-10-13; clean-room): the
/// spaghetti/noodle family. Two independent 3D noise fields A and B are evaluated at
/// every block; a block is carved where BOTH fields are near zero -- the intersection
/// of the two zero-isosurfaces is a thin tube that wanders in 3D:
/// carve iff A*A + B*B < tubeRadius*tubeRadius (noise-space units). Spaghetti and
/// noodle are one family with two parameter tiers (noodle = tighter radius, higher
/// frequency, squigglier), exposed as two real, separately selectable carvers with
/// their own seeds/salts, exactly like the codebase's other parameter-tier siblings.
/// Position-stable pure fields: no paths, no chunk replay, seamless borders.
struct TubeCaveSettings {
  float tubeRadius = 0.14f;      ///< noise-space tube radius (spaghetti tier)
  float frequency = 1.f / 20.f;  ///< field scale of the wander
  int minY = 6;                  ///< never carve below this y
  int surfaceMargin = 8;         ///< keep this many blocks below the surface intact
  std::vector<float> density;    ///< per-biome carve gate multiplier; missing entries = 1
};

class SpaghettiCarver final : public ICarver {
 public:
  using BiomeProbe = std::function<uint8_t(int32_t wx, int32_t wz)>;
  SpaghettiCarver();  // spaghetti tier defaults, so {"type": "spaghetti"} works bare
  explicit SpaghettiCarver(TubeCaveSettings s);
  void carve(StageContext& ctx) const override;
  void onPipelineBuilt(const GeneratorPipeline* pipeline) override;
  void setBiomeProbe(BiomeProbe probe) { probe_ = std::move(probe); }
  const TubeCaveSettings& settings() const { return s_; }

 private:
  TubeCaveSettings s_;
  BiomeProbe probe_;
};

/// Noodle tier: tubeRadius 0.125, frequency 1/13 in NoodleCarver's default settings.
class NoodleCarver final : public ICarver {
 public:
  using BiomeProbe = std::function<uint8_t(int32_t wx, int32_t wz)>;
  NoodleCarver();  // noodle tier defaults (tighter, higher-frequency than spaghetti)
  explicit NoodleCarver(TubeCaveSettings s);
  void carve(StageContext& ctx) const override;
  void onPipelineBuilt(const GeneratorPipeline* pipeline) override;
  void setBiomeProbe(BiomeProbe probe) { probe_ = std::move(probe); }
  const TubeCaveSettings& settings() const { return s_; }

 private:
  TubeCaveSettings s_;
  BiomeProbe probe_;
};

/// Underground ruins of the old machine: fossilized circuit traces, broken via stubs, and
/// delaminated crust layers, buried in the crust below the surface. Traces route orthogonally
/// (circuits route at right angles) with missing segments; via stubs are capped columns that
/// never reach the surface; delamination alternates crust materials in thin buried bands.
///
/// Everything replaces crust blocks 1:1 (no air pockets except tiny, rare collapse voids of at
/// most 2x1x1), and every placement is clamped inside the generating chunk: the result is a
/// pure function of the chunk seed, independent of neighbours and thread order. The optional
/// open mode (openMode, default off) additionally carves open ruined volumes -- see the
/// settings block; it is the only mode in which this carver creates real air volume.
struct RuinsSettings {
  MaterialId trace = kAirMaterial;   ///< fossil trace block (e.g. fossil_trace)
  MaterialId via = kAirMaterial;     ///< broken via block (e.g. broken_via)
  std::vector<MaterialId> crust;     ///< crust materials eligible for replacement
  MaterialId delamA = kAirMaterial;  ///< delaminated band alternate A (e.g. stone)
  MaterialId delamB = kAirMaterial;  ///< delaminated band alternate B (e.g. dirt)
  std::vector<float> density;        ///< per-biome ruin density multiplier; missing entries = 1
  int depthMin = 10, depthMax = 40;  ///< ruin band: blocks below the surface
  int surfaceMargin = 6;             ///< nothing is placed closer than this to the surface
  int traceMinLen = 8, traceMaxLen = 24;  ///< Manhattan trace length in blocks
  int tracesPerChunk = 6;            ///< trace start attempts per chunk (times biome density)
  float gapChance = 0.15f;           ///< per-block chance a trace segment is missing (broken)
  float voidChance = 0.06f;          ///< per-trace chance of a tiny (<= 2x1x1) collapse void
  int viasPerChunk = 3;              ///< via stub attempts per chunk (times biome density)
  int viaMinH = 2, viaMaxH = 5;      ///< via stub column height (capped, buried)
  float viaTraceChance = 0.35f;      ///< chance a via stub grows a trace from its base
  float delamChance = 0.35f;         ///< per-chunk chance of delaminated bands
  int delamMinY = 2;                 ///< delamination never reaches below this y
  // ---- open mode (worldgen completeness, 2026-10-13; config-gated, OFF by default)
  // With openMode set, the carver ADDITIONALLY carves open ruined volumes: a buried
  // rectangular template volume is eroded by a noise mask (edges crumble), carved to
  // air, and rubble is scattered on the new floor. With openMode unset (the default,
  // and every shipped config) the carver is bit-identical to the inlay-only carver:
  // the open pass is skipped before any RNG is drawn for it.
  bool openMode = false;             ///< gate for the open-volume pass
  MaterialId rubble = kAirMaterial;  ///< floor scatter material; kAirMaterial = use `trace`
  int openRoomsPerChunk = 2;         ///< open-volume attempts per chunk (times biome density)
  int openMinSize = 3, openMaxSize = 6;  ///< open-volume footprint per axis, blocks
  int openHeight = 3;                ///< open-volume interior height, blocks
  float rubbleChance = 0.35f;        ///< per-floor-cell rubble scatter probability
};

class RuinsCarver final : public ICarver {
 public:
  explicit RuinsCarver(RuinsSettings s);
  void carve(StageContext& ctx) const override;

 private:
  RuinsSettings s_;
};

// ---- rivers

/// One sampled point of a river's downhill walk: world position and the routing height there.
struct RiverPoint {
  float x = 0.f, z = 0.f, h = 0.f;
};

/// Fluid-emission convention (the worldgen -> fluid-sim seam):
///
/// Rivers write water as the `water` material id with the fluid level in the block state
/// byte's value bits (stValue(state) == level, 1..16 per fluid.h). Worldgen emits level 16
/// (source) for every water cell in the carved channel; the IFluidSim settles and tapers
/// levels at runtime. The world layer maps the `water` material id to its water BlockDef
/// (material names resolve through the generator's MaterialResolver, e.g. the "water"
/// texture key) and reads the level back out of the state byte into FluidState{type, level}.
struct RiverSettings {
  MaterialId water = kAirMaterial;  ///< REQUIRED: water material id (level 16 emitted)
  MaterialId bed = kAirMaterial;    ///< channel floor material; kAirMaterial = keep terrain
  int cellSize = 256;               ///< river-origin lattice spacing, in blocks (one roll per cell)
  float chance = 0.5f;              ///< per-cell spawn probability (times the biome density)
  int maxSteps = 64;                ///< longest walk, in steps of stepLen blocks
  int stepLen = 4;                  ///< blocks advanced per walk step
  float width = 4.f;                ///< channel half-width in blocks (cosine bank profile)
  float depth = 4.f;                ///< channel depth below the routing height at the centerline
  float freeboard = 1.f;            ///< water surface sits this far below the routing height
  float meander = 2.f;              ///< perpendicular meander amplitude in blocks (0 = none)
  float meanderFreq = 0.19635f;     ///< meander radians per step (~2*pi/32)
  float momentum = 0.3f;            ///< walk-direction smoothing, [0, 1)
  float minSlope = 0.02f;           ///< the walk stops when |gradient| falls below this
  float uphillTol = 0.5f;           ///< accepted rise per step before the walk deflects, then stops
  float waterTable = 0.f;           ///< the walk stops when the routing height drops below this
  float minStartHeight = 8.f;       ///< origins spawn only at/above this routing height
  float maxStartHeight = 1e9f;      ///< origins spawn only at/below this routing height
  std::array<float, 256> density{}; ///< per-biome spawn multiplier; the generator loader fills
                                     ///< 1.0 for every biome, then applies "biomes" overrides
                                     ///< (direct construction must fill 1.0 itself)
};

/// Noise-driven rivers carved into the heightfield.
///
/// Routing: each river starts at a seeded point in a coarse origin cell (the highest of 4
/// candidates, so rivers begin in the hills) and walks the descending gradient of a routing
/// height sampled from the graph (`routingOutput`, default "height"). The walk is a pure
/// function of (dimensionSeed, origin cell): every chunk replays the origin cells within
/// reach (like WormCaveCarver replays neighbouring worms) and only ever writes its own
/// columns, so rivers are deterministic, seeded, and chunk-pure. The walk never flows
/// uphill: each step is checked against `uphillTol` with a few deflected retries, and the
/// river stops at flat ground, the water table, or a peak it cannot get around.
///
/// Carving: the channel is cut with a cosine cross-section (`width`, `depth`), giving sloped
/// banks, never vertical slots. The channel floor is optionally re-materialed (`bed`).
/// Water fills the channel up to `freeboard` below the routing height, every cell at level
/// 16 (source) per the fluid-emission convention above.
///
/// Per-biome/dimension configurability: spawn density is per-biome (`density`, probed from
/// the finished pipeline, overridable at construction); every other parameter is per
/// carver instance, so each dimension's generator JSON tunes its own rivers.
///
/// Caveat: the routing height is a single graph output, while the real terrain may blend
/// several (BiomeHeightmapTerrain). The generator JSON should expose a representative
/// routing output (usually the same "height" the terrain uses, or a smoothed variant);
/// where the routing height and the terrain disagree, the carve takes min(actual, channel)
/// so rivers never raise terrain or float water in the air.
class RiverCarver final : public ICarver {
 public:
  /// `heightOutput`: graph output index used for downhill routing (see caveat above).
  RiverCarver(std::shared_ptr<const NoiseGraph> graph, int heightOutput, RiverSettings s);
  void carve(StageContext& ctx) const override;
  void onPipelineBuilt(const GeneratorPipeline* pipeline) override;
  /// Override the biome probe (used for the per-biome spawn density). Bound to the
  /// pipeline automatically by onPipelineBuilt when left null; null = no biome gate.
  void setBiomeProbe(BiomeProbe probe) { probe_ = std::move(probe); }
  const RiverSettings& settings() const { return s_; }
  /// Origin cells (in cell units) a chunk at (cx, cz) must replay: the reach of the walk.
  int reachCells() const { return reach_; }
  /// The river (possibly empty) spawned by the origin cell (cellX, cellZ): seeded start,
  /// downhill walk, water-table stop. A pure function of its inputs -- this is what the
  /// no-uphill-flow and determinism tests exercise directly. Thread-safe.
  static std::vector<RiverPoint> traceRiver(const NoiseGraph& graph, int heightOutput,
                                           uint64_t dimensionSeed, int32_t cellX, int32_t cellZ,
                                           const RiverSettings& s, const BiomeProbe& probe);

 private:
  std::shared_ptr<const NoiseGraph> graph_;
  int heightOutput_;
  RiverSettings s_;
  BiomeProbe probe_;
  int reach_ = 1;
};

// ---- features

struct OreVeinSettings {
  MaterialId material = kAirMaterial;
  MaterialId replace = kAirMaterial;  ///< only this material is replaced
  int veinsPerChunk = 8;
  int size = 10;                      ///< blocks per vein (random walk steps)
  int minY = 4, maxY = 64;
};

/// Blob-shaped veins of `material` inside `replace`, kept within the chunk.
class OreVeinFeature final : public IFeature {
 public:
  explicit OreVeinFeature(OreVeinSettings s) : s_(s) {}
  void place(StageContext& ctx, Rng& rng) const override;

 private:
  OreVeinSettings s_;
};

/// Structure slot for dungeons. Placeholder: places nothing yet.
class DungeonSlot final : public IFeature {
 public:
  void place(StageContext&, Rng&) const override {}
};


// ---- silicon forest features (Wafer surface flora)

/// Per-biome placement density: a per-column probability for every biome index. The generator
/// loader fills this from a default density plus per-biome overrides (biome names resolved to
/// indices); features read it with the column's biome id from ColumnMaps::biome.
struct BiomeDensity {
  std::array<float, 256> density{};  ///< per-column placement probability per biome index
  float at(uint8_t biome) const { return density[biome]; }
};

/// Effective placement density at one column under biome blending: the density table weighted
/// by the column's biome weight vector (sum_b density[b] * weight_b). A feature eligible in
/// one biome at weight 0.3 gets ~30% of its full density there. With no blending data
/// (ColumnMaps::biomeCount == 0, or a short vector) this is exactly density[dominantBiome].
/// Deterministic: weights are fixed per column, so the seeded per-position density dither
/// needs no changes.
float blendedDensity(const BiomeDensity& densities, const ColumnMaps& maps, size_t column);

/// --- Superformula (Litho Formula) trace-tree canopies -------------------------
/// Review item M11: every trace tree used to be the identical silhouette. The
/// canopy is now a small Litho Formula volume (spherical product of two 2D
/// superformula evaluators, litho_formula.h) stamped around the trunk crown:
/// branch-material cells fill the volume, the top cell of each canopy column
/// takes the tree's tip material (the red/blue glow behaviour is unchanged).
///
/// DETERMINISM (world permanence): the canopy for a tree is a pure function of
/// (feature seed, tree anchor world position). traceTreeCanopyFor derives its
/// own position-keyed Rng via posSeed(featSeed ^ canopy salt, wx, wz) — the
/// same discipline TraceTreeFeature uses for trunk/branches — so canopies are
/// identical regardless of chunk generation order, and the canopy stream never
/// perturbs the trunk/branch stream. No rand(), no time, no unordered iteration.
inline constexpr int kTraceTreeCanopyRadiusHMax = 3;  ///< max horizontal canopy radius (blocks)
inline constexpr int kTraceTreeCanopyRadiusVMax = 4;  ///< max vertical canopy radius (blocks)

/// Canopy silhouette families (traceTreeCanopyArchetypeName gives the names).
enum class TraceTreeCanopyArchetype : uint8_t {
  Round = 0,   ///< classic blob crown: theta/phi near-sphere, rH 2..3, rV 2..3
  Spire = 1,   ///< tall tapered crown: phi profile pod-like (n2/n3 high), rH 2, rV 3..4
  Parasol = 2, ///< broad flat crown: rH 3, rV 1 (wider than tall)
  Lobed = 3,   ///< multi-lobe crown: theta m = 3..5 lobes, rH 3, rV 2..3
};

struct TraceTreeCanopySpec {
  TraceTreeCanopyArchetype archetype = TraceTreeCanopyArchetype::Round;
  litho_formula::LithoParams theta;  ///< longitude evaluator (lobes live here)
  litho_formula::LithoParams phi;    ///< latitude evaluator (vertical profile)
  int radiusH = 2;                   ///< horizontal extent after normalization, 1..3
  int radiusV = 2;                   ///< vertical extent after normalization, 1..4
};

/// One canopy cell relative to the crown centre (trunk top + 1). `tip` cells
/// take the tree's tip material; the rest take the branch material.
struct TraceTreeCanopyCell {
  int dx = 0, dy = 0, dz = 0;
  bool tip = false;
};

const char* traceTreeCanopyArchetypeName(TraceTreeCanopyArchetype a);

/// Per-instance canopy parameters for the tree anchored at world (wx, wz)
/// under feature seed `featSeed` (deriveSeed(dimensionSeed, nameHash("trace_tree"))).
/// Pure function of (featSeed, wx, wz): the M11 variance source. Jitter covers
/// archetype, theta m/n1/n2/n3/a/b, phi n1/n2/n3, and radiusH/radiusV.
TraceTreeCanopySpec traceTreeCanopyFor(uint64_t featSeed, int32_t wx, int32_t wz);

/// Voxelizes a canopy spec into a filled local stamp (surface sampled densely
/// via lithoPoint, per-column min/max span filled — the litho_formula_voxelize
/// approach at tree scale). Deterministic cell order (dz, dx, dy ascending).
/// Cost: 72x36 surface samples per call, factored into 72+36 lithoR
/// evaluations (~0.05 ms/tree measured; see the self-test timing line).
std::vector<TraceTreeCanopyCell> traceTreeCanopyCells(const TraceTreeCanopySpec& spec);

struct TraceTreeSettings {
  MaterialId trunk = kAirMaterial;
  MaterialId branch = kAirMaterial;
  MaterialId tipRed = kAirMaterial;
  MaterialId tipBlue = kAirMaterial;
  MaterialId surface = kAirMaterial;  ///< required top block under the trunk; kAirMaterial = any solid
  int minTrunkH = 4, maxTrunkH = 7;
  int minBranches = 2, maxBranches = 4;
  int minBranchLen = 2, maxBranchLen = 3;
  float redTipChance = 0.15f;  ///< per tree, not per tip: each tree glows red or blue
  BiomeDensity biomes;
};

/// Circuit-grown tree: a via-like trunk, 2-4 trace branches forking outward/up, one glowing tip
/// colour per tree (mostly blue, occasionally red). Every decision is a pure function of the
/// world block position (seeded hash per position keyed by the feature type), so neighbouring
/// chunks agree on trees that cross the border without reading each other: each chunk replays
/// the margin of its neighbours and only ever writes its own columns.
class TraceTreeFeature final : public IFeature {
 public:
  explicit TraceTreeFeature(TraceTreeSettings s) : s_(std::move(s)) {}
  void place(StageContext& ctx, Rng& rng) const override;
  /// Horizontal blocks a tree can reach past its base: the margin replayed from neighbours.
  /// Canopy radius participates: a border tree's canopy must replay identically
  /// in the neighbouring chunk, so reach covers the widest canopy.
  int reach() const { return std::max(s_.maxBranchLen, kTraceTreeCanopyRadiusHMax); }
  const TraceTreeSettings& settings() const { return s_; }

 private:
  TraceTreeSettings s_;
};

struct CapacitorCanSettings {
  MaterialId can = kAirMaterial;
  MaterialId surface = kAirMaterial;  ///< required top block under a can; kAirMaterial = any solid
  int minCans = 3, maxCans = 7;
  int minCanH = 2, maxCanH = 3;
  BiomeDensity biomes;
};

/// Cluster of squat capacitor-can "trees" (3-7 cans, 2-3 tall, in a tight radius-2 cluster).
/// Same position-keyed deterministic placement as TraceTreeFeature.
class CapacitorCanFeature final : public IFeature {
 public:
  explicit CapacitorCanFeature(CapacitorCanSettings s) : s_(std::move(s)) {}
  void place(StageContext& ctx, Rng& rng) const override;
  int reach() const { return 2; }
  const CapacitorCanSettings& settings() const { return s_; }

 private:
  CapacitorCanSettings s_;
};

struct ResonatorSpireSettings {
  MaterialId crystal = kAirMaterial;
  MaterialId surface = kAirMaterial;  ///< required top block under the spire; kAirMaterial = any solid
  int minH = 8, maxH = 16;
  BiomeDensity biomes;
};

/// Tall thin crystal spike (8-16 tall, 1 wide, 2x2 for the first 3 blocks half the time).
/// Same position-keyed deterministic placement as TraceTreeFeature.
class ResonatorSpireFeature final : public IFeature {
 public:
  explicit ResonatorSpireFeature(ResonatorSpireSettings s) : s_(std::move(s)) {}
  void place(StageContext& ctx, Rng& rng) const override;
  int reach() const { return 1; }
  const ResonatorSpireSettings& settings() const { return s_; }

 private:
  ResonatorSpireSettings s_;
};

// ---- structures (above-ground ruins, monuments, ...)

/// One block of a structure blueprint: relative to the base (base sits at surfY + 1).
struct BlueprintCell {
  int dx = 0, dy = 0, dz = 0;  ///< offset from the base block position
  MaterialId material = kAirMaterial;
  float decay = 0.f;  ///< per-cell skip probability (ruin damage), position-hashed, [0, 1]
};

/// How a structure's base Y is determined.
enum class AnchorMode : uint8_t {
  Surface,   ///< base sits on the terrain surface (surfY + 1); classic behavior
  FixedY,    ///< base sits at a fixed Y (buried cities, sky ruins)
  Floating,  ///< base hovers at surfY + 1 + hoverHeight
};

/// Placement parameters for one structure type. The Wafer configures these from its
/// generator JSON ("features": {"type": "structure", ...}); other dimensions reuse the
/// same framework with their own blueprints.
struct StructureSettings {
  std::string seedName;      ///< salts the cell RNG (defaults to the feature name)
  std::string structureName; ///< registry name ("jd:watchtower"); when set, variants come
                             ///< from the StructureRegistry instead of an inline blueprint
  int cellSize = 64;         ///< candidate lattice spacing in blocks; at most one base per cell
  float density = 0.05f;     ///< per-cell placement probability, before the biome gate
  BiomeDensity biomes;       ///< per-biome multiplier, gated on the dominant biome at the base
  MaterialId surface = kAirMaterial;  ///< required top block under the base; kAirMaterial = any solid
  int snap = 0;              ///< > 0: base x/z snapped to multiples of this (die-grid alignment)
  bool rotate = true;        ///< rotate the blueprint 0/90/180/270 by the cell seed
  // ---- placement solver upgrades (2026-10-12) ----
  float spacing = 0.f;       ///< min distance between placed bases; 0 = cell lattice only
  float minSlope = 0.f;      ///< reject when terrain gradient under the footprint < minSlope
  float maxSlope = 1e9f;     ///< reject when terrain gradient under the footprint > maxSlope
  int minHeight = INT_MIN;   ///< reject when the surface Y at the base < minHeight
  int maxHeight = INT_MAX;   ///< reject when the surface Y at the base > maxHeight
  bool nearWater = false;    ///< require water (height < seaLevel) within waterRadius
  int waterRadius = 8;       ///< search radius in blocks for nearWater
  int seaLevel = INT_MIN;    ///< water surface Y; INT_MIN disables the water check
  AnchorMode anchor = AnchorMode::Surface;  ///< how the base Y is determined
  int fixedY = 0;            ///< base Y when anchor == FixedY
  int hoverHeight = 0;       ///< extra lift above the surface when anchor == Floating
};

/// General above-ground structure framework: seeded, chunk-local-safe, biome-gated.
///
/// Candidate bases live on a coarse cell lattice (at most one per cell, so structures keep
/// their spacing); the base position inside the cell comes from a cell-keyed RNG, and every
/// decision is a pure function of (dimensionSeed, cell, position). Each chunk replays the
/// cells overlapping its margin (blueprint reach + snap drift) and only ever writes its own
/// columns through setIntoAir -- neighbours agree without reading each other EXCEPT at
/// simultaneous chunk+biome borders (see the forBases comment in stages.cpp: a replaying
/// chunk clamps out-of-chunk bases to its nearest in-chunk column for biome weights and
/// surface height, so at a chunk+biome border it can resolve a different dominant biome
/// than the owning chunk and replay a border structure partially — rare, bounded to
/// `reach` columns of the border, and fully deterministic). Terrain is never overwritten
/// (structures sit on the surface, into air only).
///
/// The biome gate uses the dominant biome at the exact base position, probed from the finished
/// pipeline (bound automatically by Builder::build via onPipelineBuilt). basesInRect() replays
/// the same cell/biome-gate computation (subject to the same chunk+biome-border caveat, and
/// to the extra place()-only gates listed there), so overview maps mark the candidate
/// structures chunk gen may place. The long-term fix — probing biome/height by WORLD
/// position instead of the clamped in-chunk column — is deferred (P3-2 B-M5).
class StructureFeature final : public IFeature {
 public:
  /// Dominant biome-table index at a world column; bound to the pipeline by onPipelineBuilt.
  using BiomeProbe = std::function<uint8_t(int32_t wx, int32_t wz)>;
  /// A weighted blueprint variant (registry entries carry a pool; inline JSON is one variant).
  struct Variant {
    std::vector<BlueprintCell> cells;
    float weight = 1.f;
  };
  StructureFeature(StructureSettings s, std::vector<BlueprintCell> blueprint,
                   BiomeProbe probe = nullptr);
  /// Registry-backed: variants + rules come from a StructureEntry (resolved by name
  /// in the loader). Explicit JSON fields in `s` override the entry's rules.
  StructureFeature(StructureSettings s, std::vector<Variant> variants, BiomeProbe probe = nullptr);
  void place(StageContext& ctx, Rng& rng) const override;
  void onPipelineBuilt(const GeneratorPipeline* pipeline) override;
  /// CANDIDATE base (x, z) positions with any part inside [x0, x1) x [z0, z1),
  /// for `dimensionSeed` (P3-2 B-L6: renamed semantics — this returns
  /// candidates, not confirmed placements). It shares the cell resolution
  /// (baseForCell: density roll, position, spacing) and the biome gate with
  /// place(), but place() ADDITIONALLY applies terrain gates this query
  /// cannot evaluate (it has no height/surface probe): surface height band,
  /// footprint slope, near-water, anchor-mode fit and the required surface
  /// material. So the result OVER-REPORTS: every structure place() puts in
  /// the world is in here, but some candidates returned here are rejected
  /// by place()'s terrain gates. Consumers (overview/scout maps) must
  /// treat entries as "may generate here", never as confirmed structures.
  /// Needs the biome probe (empty when the pipeline never bound one --
  /// pipelines built by Builder::build always do).
  std::vector<std::array<int32_t, 2>> basesInRect(uint64_t dimensionSeed, int32_t x0, int32_t z0,
                                                  int32_t x1, int32_t z1) const;
  /// Horizontal blocks the structure can reach past its base (blueprint extent + snap drift).
  int reach() const { return reach_; }
  const StructureSettings& settings() const { return s_; }
  const std::vector<BlueprintCell>& blueprint() const {
    return variants_.empty() ? kEmptyCells : variants_.front().cells;
  }
  const std::vector<Variant>& variants() const { return variants_; }
  void setBiomeProbe(BiomeProbe probe) { probe_ = std::move(probe); }

 private:
  struct Base;  // resolved candidate base (position, rotation, biome, variant)
  /// Resolves the candidate base for one cell; nullopt when the cell rolls empty or the
  /// biome gate rejects it. Pure function of (featSeed, ccx, ccz).
  std::optional<Base> baseForCell(uint64_t featSeed, int32_t ccx, int32_t ccz) const;
  uint64_t featSeed(uint64_t dimensionSeed) const;

  StructureSettings s_;
  std::vector<Variant> variants_;  ///< variant pool; single-variant for inline blueprints
  BiomeProbe probe_;
  int reach_ = 0;   ///< max |dx|,|dz| over all variants, plus snap drift
  int maxDy_ = 0;   ///< max dy over all variants (air-room check)
  int minDy_ = 0;   ///< min dy over all variants (buried check)
  uint64_t decaySalt_ = 0;
  inline static const std::vector<BlueprintCell> kEmptyCells{};
};

// ---- lithoform stamping (baked .lthm models scattered as world structures) ----

/// One stamped voxel: offset from the feature base, with its resolved material.
struct LithoStampVoxel {
  int dx = 0, dy = 0, dz = 0;
  MaterialId material = kAirMaterial;
};

/// Placement parameters for one lithoform stamp type, from generator JSON
/// ("features": {"type": "lithoform_stamp", ...}).
struct LithoStampSettings {
  std::string seedName;   ///< salts the cell RNG (defaults to the feature name)
  int cellSize = 128;     ///< candidate lattice spacing in blocks; at most one base per cell
  float density = 0.02f;  ///< per-cell placement probability, before the biome gate
  BiomeDensity biomes;    ///< per-biome multiplier, gated on the dominant biome at the base
  bool rotate = true;     ///< rotate the model 0/90/180/270 by the cell seed
  int yOffset = 0;        ///< added to the surface base y
  bool anchorCenter = false;  ///< true: model vertical center at base y; false: model bottom at base y
};

/// Stamps a baked Litho Formula model (unit cells from a .lthm, via ModelResolver) into
/// the world on a coarse cell lattice: at most one base per cell, density-gated, biome-gated,
/// rotated in 90-degree steps. Like StructureFeature but driven by model cells instead of
/// a JSON blueprint. Cells write into air only, never overwriting terrain.
class LithoStampFeature final : public IFeature {
 public:
  /// Dominant biome-table index at a world column; bound to the pipeline by onPipelineBuilt.
  using BiomeProbe = std::function<uint8_t(int32_t wx, int32_t wz)>;
  LithoStampFeature(LithoStampSettings s, std::vector<LithoStampVoxel> cells,
                    BiomeProbe probe = nullptr);
  void place(StageContext& ctx, Rng& rng) const override;
  void onPipelineBuilt(const GeneratorPipeline* pipeline) override;
  /// Horizontal blocks the model can reach past its base.
  int reach() const { return reach_; }
  const LithoStampSettings& settings() const { return s_; }
  const std::vector<LithoStampVoxel>& cells() const { return cells_; }

 private:
  struct Base;  // resolved candidate base (position, rotation, biome gate roll)
  /// Resolves the candidate base for one cell; nullopt when the cell rolls empty.
  /// Pure function of (featSeed, ccx, ccz).
  std::optional<Base> baseForCell(uint64_t featSeed, int32_t ccx, int32_t ccz) const;
  uint64_t featSeed(uint64_t dimensionSeed) const;
  LithoStampSettings s_;
  std::vector<LithoStampVoxel> cells_;
  BiomeProbe probe_;
  int reach_ = 0;  ///< max |dx|,|dz| over the cells
  int minDy_ = 0;  ///< min dy over the cells (for the anchor)
  int maxDy_ = 0;  ///< max dy over the cells (air-room check)
};

}  // namespace litholattice::procgen
