// Lakes and oceans: still-water fill stages for the generator pipeline.
//
// Two ISurfaceRule stages (they run after terrain, so ColumnMaps::height is known):
//   OceanFillRule  - vast water for ocean-designated biomes: noise-driven floor relief
//                    (capped), water filled flat to sea level.
//   LakeFillRule   - still-water bodies in terrain depressions below a water table,
//                    inside smooth noise-mask regions, filled flat to the spill level.
//   ShorelineRule  - sand/gravel-ish rim around both, applied after the game's surface
//                    rules so it wins at the waterline.
//
// Everything is a pure function of (dimensionSeed, world x, z): same seed -> same water,
// always, independent of chunk borders, thread scheduling and generation order.
//
// Worldgen <-> fluid-sim seam (no channel existed; defined here): water is an ordinary
// material id resolved from the "water" material name against the game's BlockRegistry,
// exactly like every other worldgen material. Each water block's ColumnBuffer state byte
// carries the FluidState level (1..kMaxFluidLevel); worldgen always writes
// kStillWaterLevel (16 = source, still, never falling). ColumnGenerator copies the byte
// into the chunk's CellState, so the sim reads the level with stValue(state) (block.h)
// and identifies water by block id (game-defined: BlockDef behavior/flags).
#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include "litholattice/procgen/modifiers.h"
#include "litholattice/procgen/noise.h"
#include "litholattice/procgen/pipeline.h"
#include "litholattice/procgen/stages.h"  // BiomeDensity, SurfaceLayers, blendedDensity

namespace litholattice::procgen {

// ---- vertical datum and depth defaults (JD 2026-10-11; parameterized 2026-10-13)
//
// The world holds 256 solid layers below the reference ground plane (y 0..255).
// Depth limits are SETTINGS, not engine caps: the values below are only the
// defaults a settings struct starts from. A generator (game or mod) sets its own
// maxDepth / minDepth / minSolidBelow freely; the loader's only checks are sanity
// (positive depths, and seaLevel - maxDepth >= minSolidBelow, itself a settings
// parameter whose default is 60). LithoKraft's shipped values are 196 (ocean) and
// 98 (lake), set as DATA in its generator JSON -- with the default sea level 256
// and the default minSolidBelow 60 they satisfy 256 - 196 = 60, so the deepest
// default ocean floor keeps 60 solid layers beneath it. That guarantee is now a
// consequence of the chosen data, not an engine law.

/// Reference ground plane: 256 solid layers below it. 2^8.
inline constexpr int kGroundLevel = 256;
/// Solid layers below kGroundLevel (world bottom is y 0).
inline constexpr int kSolidLayersBelowGround = 256;
/// DEFAULT ocean liquid depth (a settings default, NOT a cap). LithoKraft sets
/// 196 in its generator JSON (JD 2026-10-11).
inline constexpr int kDefaultOceanMaxDepth = 196;
/// DEFAULT lake liquid depth (a settings default, NOT a cap, and no longer
/// derived from the ocean value: the two are independent fields). LithoKraft
/// sets 98 in its generator JSON (JD 2026-10-11).
inline constexpr int kDefaultLakeMaxDepth = 98;
/// DEFAULT solid layers guaranteed below the deepest ocean floor; an explicit
/// settings parameter (OceanSettings::minSolidBelow, JSON "min_solid_below").
inline constexpr int kDefaultMinSolidBelowOcean = 60;
/// Default ocean water surface: the reference ground plane.
inline constexpr int kDefaultSeaLevel = kGroundLevel;
/// FluidState.level written for worldgen water: 16 = source, still (fluid.h).
inline constexpr uint8_t kStillWaterLevel = 16;

/// Settings for OceanFillRule. maxDepth is a plain settings value: construction
/// only enforces sanity (maxDepth >= 1, 0 <= minDepth <= maxDepth when set);
/// there is no engine cap. The generator loader additionally checks
/// seaLevel - maxDepth >= minSolidBelow (both data).
struct OceanSettings {
  MaterialId water = kAirMaterial;   ///< resolved "water" material; kAirMaterial = rule no-ops
  int seaLevel = kDefaultSeaLevel;   ///< flat water surface Y
  int maxDepth = kDefaultOceanMaxDepth;  ///< liquid depth at the deepest relief (settings value)
  int minDepth = -1;  ///< shallowest relief depth; -1 = derive maxDepth/4 (the pre-2026-10-13
                      ///  behaviour, preserved exactly when the JSON omits "min_depth")
  int minSolidBelow = kDefaultMinSolidBelowOcean;  ///< loader sanity floor: the deepest
                      ///  ocean floor (seaLevel - maxDepth) must stay >= this many layers
                      ///  above y 0; JSON "min_solid_below", default 60
  MaterialId floorMaterial = kAirMaterial;  ///< raised-floor fill; kAirMaterial = reuse the
                                            ///  column's own surface block
  float depthFrequency = 1.f / 256.f;  ///< ocean-floor relief scale (large-scale: vast water)
  BiomeDensity biomes;              ///< per-biome eligibility 0..1 (dominant-biome gate when
                                    ///  unblended, weight-blended otherwise); default 0 = opt-in
  uint64_t seed = 0;                ///< dimension seed; the relief noise derives from it
};

/// Settings for LakeFillRule. maxDepth is a plain settings value (no engine cap);
/// construction only enforces sanity (maxDepth >= 1).
struct LakeSettings {
  MaterialId water = kAirMaterial;  ///< resolved "water" material; kAirMaterial = rule no-ops
  int waterTable = kDefaultSeaLevel;  ///< still-water surface Y (the spill level)
  int maxDepth = kDefaultLakeMaxDepth;  ///< liquid depth limit (settings value)
  float threshold = 0.62f;  ///< lake-mask threshold in [0, 1]; higher = fewer, smaller lakes
  float frequency = 1.f / 64.f;  ///< lake-mask blob scale (~tens of blocks across)
  BiomeDensity biomes;           ///< per-biome eligibility 0..1; default 0 = opt-in
  uint64_t seed = 0;             ///< dimension seed; the mask noise derives from it
};

/// Vast water for ocean-designated biomes. Per column: the floor target is
/// seaLevel - oceanDepth(x, z) with oceanDepth noise-driven in [minDepth, maxDepth]
/// (minDepth defaults to maxDepth/4 when unset); the floor is the blend of the existing
/// terrain height and the target by the column's ocean eligibility weight, so shores grade
/// smoothly into the existing weight-averaged height field -- no cliffs at biome borders.
/// Where the terrain lies deeper than the configured depth allows, the floor is raised
/// with floorMaterial (or the column's own surface block) so the water surface stays flat
/// at seaLevel. Columns whose floor reaches seaLevel stay dry (islands).
/// Water is written with state kStillWaterLevel. Thread-safe.
class OceanFillRule final : public ISurfaceRule {
 public:
  explicit OceanFillRule(OceanSettings s);
  void apply(StageContext& ctx) const override;
  const OceanSettings& settings() const { return s_; }
  /// Ocean liquid depth for unit noise u in [0, 1]: maps to [minDepth, maxDepth],
  /// monotone non-decreasing, maxDepth at u = 1. minDepth < 0 derives maxDepth/4.
  /// Public test seam.
  static int depthForUnit(float u, int maxDepth, int minDepth);
  /// Noise-driven floor target Y at a world column (test seam; apply() clamps it into the
  /// chunk). Never below seaLevel - maxDepth.
  int floorTarget(int wx, int wz, int seaLevel) const;

 private:
  OceanSettings s_;
  std::shared_ptr<const INoise> relief_;  ///< fbm in [-1, 1], seeded from s_.seed
};

/// Still-water bodies in terrain depressions. A column holds lake water when it is
/// biome-eligible, its terrain lies below the water table, and the smooth lake mask
/// ((fbm + 1) / 2 > threshold, eligibility-blended) covers it -- i.e. water appears in
/// depressions of the heightfield inside lake-prone regions, filled flat to the water
/// table (the spill level). Liquid depth is limited to the configured maxDepth: deeper
/// pits hold exactly maxDepth of water with air above, never more. Solid terrain is never
/// modified, only air becomes water, so lakes introduce no terrain steps. Water is written
/// with state kStillWaterLevel. Thread-safe.
class LakeFillRule final : public ISurfaceRule {
 public:
  explicit LakeFillRule(LakeSettings s);
  void apply(StageContext& ctx) const override;
  const LakeSettings& settings() const { return s_; }
  /// Lake mask in [0, 1] at a world column (test seam).
  float maskAt(int wx, int wz) const;

 private:
  LakeSettings s_;
  std::shared_ptr<const INoise> mask_;  ///< fbm in [-1, 1], seeded from s_.seed
};

/// Sand/gravel-ish rim around lake and ocean water, applied after the game's surface rules
/// so it wins at the waterline. Per column, with water surface ws (seaLevel / waterTable):
/// columns whose solid top lies in [ws - depth, ws + band] inside a water region get their
/// top (and filler below) rewritten with the dominant biome's shore layers. Pure
/// per-column function of (height, biome, region masks): continuous across chunk borders,
/// never touches water cells. Thread-safe.
class ShorelineRule final : public ISurfaceRule {
 public:
  struct Settings {
    int band = 2;    ///< dry-side blocks above the water surface rewritten
    int depth = 2;   ///< wet-side blocks below the surface rewritten
    std::vector<SurfaceLayers> perBiome;  ///< indexed by biome; kAirMaterial top = leave alone
  };
  /// Copies of the fill settings are taken (the rule must outlive the loader's stack).
  /// hasOcean/hasLake say which bodies exist; null bodies are skipped.
  ShorelineRule(Settings s, OceanSettings ocean, bool hasOcean, LakeSettings lake, bool hasLake);
  void apply(StageContext& ctx) const override;
  const Settings& settings() const { return s_; }

 private:
  Settings s_;
  OceanSettings ocean_;
  bool hasOcean_;
  LakeSettings lake_;
  bool hasLake_;
  std::shared_ptr<const INoise> lakeMask_;  ///< null when !hasLake_
};

/// Headless checks: depth defaults as settings (no caps), the configured
/// min-solid-below loader check, flat still water, determinism, border continuity,
/// shoreline placement, loader accept/reject. Returns failure count.
int runWaterSelfTest();

}  // namespace litholattice::procgen
