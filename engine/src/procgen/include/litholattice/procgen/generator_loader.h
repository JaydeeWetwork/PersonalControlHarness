// Builds a GeneratorPipeline from a generator JSON file (graph + stage configuration); see
// docs/PROCGEN.md for the format. Material names are resolved by the caller (the world knows
// its block registry), keeping procgen free of block types.
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <string_view>
#include "litholattice/core/json.h"
#include "litholattice/procgen/pipeline.h"

namespace litholattice::procgen {

/// Maps a material name ("stone") to its id; nullopt if unknown.
using MaterialResolver = std::function<std::optional<MaterialId>(std::string_view)>;

/// One stampable voxel of a baked Litho Formula model: unit-cell coords in [0, size),
/// plus the authoring-palette byte (mapped to a MaterialId by the feature's JSON
/// "materials" table).
struct LithoStampCell {
  int x = 0, y = 0, z = 0;
  uint8_t palette = 0;
};

/// A baked Litho Formula model as stampable unit cells (from a .lthm file).
struct LithoStampModel {
  int size = 0;  ///< grid edge in cells (32 for the res-32 gallery)
  std::vector<LithoStampCell> cells;
};

/// Resolves a model name ("starfish") to its stampable cells; nullopt if unknown.
/// Lets the game supply .lthm-backed models while procgen stays asset-free.
using ModelResolver = std::function<std::optional<LithoStampModel>(std::string_view name)>;

/// Parses and validates a generator. Graph noise is seeded from `dimensionSeed`. Returns nullptr
/// and sets `err` (prefixed with the failing section) on any error.
std::unique_ptr<const GeneratorPipeline> loadGenerator(const Json& json, uint64_t dimensionSeed,
                                                       const MaterialResolver& materials, std::string& err);
/// Same, from JSON text.
std::unique_ptr<const GeneratorPipeline> loadGenerator(const std::string& text, uint64_t dimensionSeed,
                                                       const MaterialResolver& materials, std::string& err);
/// Same, plus `extraSurface` rules appended after the JSON-configured surface rules. Lets
/// game code attach ISurfaceRules that JSON material names cannot express (e.g. rules that
/// place block ids directly instead of going through the material resolver).
std::unique_ptr<const GeneratorPipeline> loadGenerator(const Json& json, uint64_t dimensionSeed,
                                                       const MaterialResolver& materials,
                                                       std::vector<std::unique_ptr<ISurfaceRule>> extraSurface,
                                                       std::string& err);
/// Same, from JSON text.
std::unique_ptr<const GeneratorPipeline> loadGenerator(const std::string& text, uint64_t dimensionSeed,
                                                       const MaterialResolver& materials,
                                                       std::vector<std::unique_ptr<ISurfaceRule>> extraSurface,
                                                       std::string& err);
/// Same, plus an optional `models` resolver for "lithoform_stamp" features. When null
/// (the default), that feature type fails to load with a clear error.
std::unique_ptr<const GeneratorPipeline> loadGenerator(const Json& json, uint64_t dimensionSeed,
                                                       const MaterialResolver& materials,
                                                       std::vector<std::unique_ptr<ISurfaceRule>> extraSurface,
                                                       ModelResolver models, std::string& err);
/// Same, from JSON text.
std::unique_ptr<const GeneratorPipeline> loadGenerator(const std::string& text, uint64_t dimensionSeed,
                                                       const MaterialResolver& materials,
                                                       std::vector<std::unique_ptr<ISurfaceRule>> extraSurface,
                                                       ModelResolver models, std::string& err);

/// Climate parameter names used in JSON ("temperature", "humidity", "continentalness", "erosion").
std::string_view toString(ClimateParam p);

}  // namespace litholattice::procgen
