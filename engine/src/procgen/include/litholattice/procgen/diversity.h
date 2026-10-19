// Ecosystem & environment diversity stages (2026-10-12): ground cover, rock formations,
// biome transition bands, Worley ore veins, fauna spawn hooks, and weather-shaped surface
// rules (snow caps, sand drifts). All deterministic: every decision is a pure function of
// (dimensionSeed, world position), following the StructureFeature discipline, so chunks and
// --seed runs agree exactly. Everything is data-driven from generator JSON (see
// generator_loader.cpp); no hardcoded biomes or materials here.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "litholattice/procgen/pipeline.h"
#include "litholattice/procgen/stages.h"

namespace litholattice::procgen {

// ---- 1. ground cover ---------------------------------------------------------------

/// One ground-cover kind: grass tuft, flower, pebble, fallen leaves...
struct GroundCoverEntry {
  MaterialId material = kAirMaterial;  ///< REQUIRED
  MaterialId surface = kAirMaterial;   ///< required top block; kAirMaterial = any solid
  int minH = 1, maxH = 1;              ///< cover height in blocks (1 = tuft, 2 = tall flower)
  bool tintByBiome = false;            ///< state byte = dominant biome index (biome tinting)
  BiomeDensity biomes;                 ///< per-biome placement density
};

struct GroundCoverSettings {
  std::string seedName;                ///< salts the position RNG (defaults to feature name)
  std::vector<GroundCoverEntry> entries;
};

/// Cheap single-column decoration: at most one cover per column, picked by a single roll
/// against the entries' cumulative density. Position-keyed (no chunk RNG), vertical only,
/// writes into air above the surface. The biggest visual-diversity win per line of code.
class GroundCoverFeature final : public IFeature {
 public:
  explicit GroundCoverFeature(GroundCoverSettings s) : s_(std::move(s)) {}
  void place(StageContext& ctx, Rng& rng) const override;
  const GroundCoverSettings& settings() const { return s_; }

 private:
  GroundCoverSettings s_;
};

// ---- 2. rock formations ------------------------------------------------------------

struct RockFormationSettings {
  std::string seedName;          ///< salts the position RNG (defaults to feature name)
  MaterialId material = kAirMaterial;  ///< REQUIRED
  MaterialId surface = kAirMaterial;   ///< required top block; kAirMaterial = any solid
  int minR = 2, maxR = 4;        ///< horizontal blob radius in blocks
  int minH = 2, maxH = 8;        ///< formation height in blocks (h >> r = spire, h ~= r = boulder)
  float wobble = 0.35f;          ///< noise wobble on the silhouette, [0, 1)
  float density = 0.02f;         ///< per-column base probability, times the biome density
  BiomeDensity biomes;
};

/// Boulders, arches and spires: noise-wobbled ellipsoid blobs grown upward from the surface.
/// Position-keyed with margin replay (reach = maxR), writes into air only, never overwrites
/// terrain. One feature covers the whole boulder->spire spectrum via the radius/height ratio.
class RockFormationFeature final : public IFeature {
 public:
  explicit RockFormationFeature(RockFormationSettings s) : s_(std::move(s)) {
    s_.minR = std::max(1, s_.minR); s_.maxR = std::max(s_.minR, s_.maxR);
    s_.minH = std::max(1, s_.minH); s_.maxH = std::max(s_.minH, s_.maxH);
    s_.wobble = std::clamp(s_.wobble, 0.f, 0.95f);
    s_.density = std::clamp(s_.density, 0.f, 1.f);
  }
  void place(StageContext& ctx, Rng& rng) const override;
  /// Horizontal blocks a formation can reach past its base.
  int reach() const { return s_.maxR; }
  const RockFormationSettings& settings() const { return s_; }

 private:
  RockFormationSettings s_;
};

// ---- 3. biome transition bands -----------------------------------------------------

/// One transition rule: where biome `a` meets biome `b` (within `width` columns), columns of
/// either side become `result` -- the beach between ocean and land, scree at mountain feet.
/// Symmetric in a/b. Narrow by design (2-4 columns); the band is a distinct biome, not a
/// gradient (gradients are BlendedBiomeSelector's job).
struct BiomeTransition {
  uint8_t a = 0, b = 0;      ///< biome indices, resolved from names by the loader
  uint8_t result = 0;        ///< transition biome index
  int width = 3;             ///< neighbor search radius in columns
};

/// Post-process on the biome map: runs inside generate() after biome selection, before
/// terrain. In-chunk neighbours read ColumnMaps::biome directly; out-of-chunk neighbours
/// use the pipeline's biomeAt probe (bound by onPipelineBuilt) -- the probe returns the
/// un-transitioned dominant biome, so there is no recursion and every chunk agrees.
class BiomeTransitionStage final : public IBiomePostStage {
 public:
  using Probe = BiomeProbe;  ///< dominant un-transitioned biome at a world column
  explicit BiomeTransitionStage(std::vector<BiomeTransition> rules) : rules_(std::move(rules)) {}
  void process(StageContext& ctx) const override;
  void onPipelineBuilt(const GeneratorPipeline* pipeline) override;
  void setProbe(Probe probe) { probe_ = std::move(probe); }
  const std::vector<BiomeTransition>& rules() const { return rules_; }

 private:
  std::vector<BiomeTransition> rules_;
  Probe probe_;
};

// ---- 4. Worley ore veins -----------------------------------------------------------

struct WorleyOreVeinSettings {
  std::string seedName;                ///< salts the noise (defaults to feature name)
  MaterialId material = kAirMaterial;  ///< REQUIRED: the ore block
  MaterialId replace = kAirMaterial;   ///< REQUIRED: only this material is replaced (never air)
  int minY = 4, maxY = 64;             ///< vein band
  float frequency = 0.08f;             ///< Worley cell frequency (cells per block)
  float threshold = 0.32f;             ///< F1 (normalized ~[0,1]) below this -> ore
  float clusterFreq = 0.f;             ///< low-freq deposit mask frequency; 0 = no clustering
  float clusterThreshold = 0.f;        ///< mask value gate in [-1, 1]
};

/// 3D ore veins as Worley-noise blobs: where the F1 distance field dips below `threshold`
/// inside the y band, `replace` becomes ore. The field is a pure function of position, so
/// the veins are chunk-pure with no neighbour replay at all. An optional low-frequency
/// value-noise mask clusters veins into deposits instead of a uniform sprinkle.
class WorleyOreVeinFeature final : public IFeature {
 public:
  explicit WorleyOreVeinFeature(WorleyOreVeinSettings s) : s_(std::move(s)) {}
  void place(StageContext& ctx, Rng& rng) const override;
  const WorleyOreVeinSettings& settings() const { return s_; }
  /// Normalized Worley F1 at a world position: exposed for tests. Pure function of
  /// (seed, position); ~[0, 1] for the frequencies worldgen uses.
  static float worleyF1(uint64_t seed, float x, float y, float z);

 private:
  WorleyOreVeinSettings s_;
};

// ---- 6. fauna spawn hooks ----------------------------------------------------------
// (5. dimension palettes are JSON-only: the loader already builds a full pipeline per
// dimension from its JSON; see the cosmic/underwafer example profiles in the selftest.)
//
// FaunaEntry lives in pipeline.h (pipeline-owned data). The query API is
// GeneratorPipeline::faunaForBiome / faunaAt -- the entity system's spawn hook.

// ---- 7. weather-shaped generation --------------------------------------------------

struct SnowCapSettings {
  MaterialId snow = kAirMaterial;  ///< REQUIRED: snow block
  int baseLine = 180;             ///< snow line at |z| == 0 (equator)
  float latitudeFalloff = 0.0008f;  ///< snow line drops this many blocks per block of |z|
  float noiseAmp = 12.f;          ///< wobble amplitude on the snow line, in blocks
  float noiseFreq = 0.01f;        ///< wobble frequency (per block)
  int minHeight = 0;              ///< snow never below this y, even at the poles
  std::string seedName;
};

/// Static baking of snowfall: columns at/above the latitude-adjusted snow line get a snow
/// top. Runs as a surface rule after LayeredSurface so it caps the biome's own top block.
class SnowCapRule final : public ISurfaceRule {
 public:
  explicit SnowCapRule(SnowCapSettings s) : s_(std::move(s)) {}
  void apply(StageContext& ctx) const override;
  const SnowCapSettings& settings() const { return s_; }

 private:
  SnowCapSettings s_;
};

struct SandDriftSettings {
  MaterialId sand = kAirMaterial;  ///< REQUIRED
  float frequency = 0.02f;         ///< drift-noise frequency (per block)
  float threshold = 0.25f;         ///< value noise in [-1,1] above this -> drift
  int depth = 2;                   ///< top N solid blocks rewritten to sand
  BiomeDensity biomes;             ///< usually desert-only
  std::string seedName;
};

/// Static baking of wind: where the drift noise runs high, the top of the column becomes
/// sand (dunes read as sand patches over the biome's own surface). A surface rule.
class SandDriftRule final : public ISurfaceRule {
 public:
  explicit SandDriftRule(SandDriftSettings s) : s_(std::move(s)) {
    s_.depth = std::max(1, s_.depth);
  }
  void apply(StageContext& ctx) const override;
  const SandDriftSettings& settings() const { return s_; }

 private:
  SandDriftSettings s_;
};

}  // namespace litholattice::procgen
