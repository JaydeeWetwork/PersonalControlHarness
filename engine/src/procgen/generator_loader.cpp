#include "litholattice/procgen/generator_loader.h"
#include "litholattice/core/log.h"  // B-L3: loud warning for the scalar-density footgun
#include <algorithm>
#include <array>
#include <cmath>
#include "litholattice/procgen/diversity.h"
#include "litholattice/procgen/stages.h"
#include "litholattice/procgen/struct_registry.h"
#include "litholattice/procgen/water.h"
#include <algorithm>

namespace litholattice::procgen {
namespace {

constexpr std::array<std::string_view, kClimateParamCount> kClimateNames{"temperature", "humidity", "continentalness",
                                                                         "erosion"};

/// Shared state while loading one generator: graph, material lookup, first error.
class Loader {
 public:
  Loader(const MaterialResolver& materials, std::string& err) : materials_(materials), err_(err) {}

  std::shared_ptr<const NoiseGraph> graph;
  ModelResolver models;  ///< null unless the game supplied .lthm model resolution

  std::optional<MaterialId> resolveMaterial(std::string_view name) { return materials_(name); }

  bool fail(const std::string& section, const std::string& msg) {
    err_ = section + ": " + msg;
    return false;
  }

  bool material(const Json& obj, const char* key, MaterialId& out, const std::string& section, bool required = true) {
    const Json* v = obj.get(key);
    if (!v) return !required || fail(section, std::string("missing '") + key + "'");
    if (v->type != Json::Type::String) return fail(section, std::string("'") + key + "' must be a material name");
    const auto id = materials_(v->str);
    if (!id) return fail(section, "unknown material '" + v->str + "'");
    out = *id;
    return true;
  }

  bool output(const Json& obj, const char* key, int& out, const std::string& section) {
    const std::string name = obj.getStr(key);
    out = graph->outputIndex(name);
    return out >= 0 || fail(section, "graph has no output '" + name + "'");
  }

 private:
  const MaterialResolver& materials_;
  std::string& err_;
};

std::unique_ptr<ITerrainStage> loadTerrain(Loader& l, const Json& j) {
  const std::string type = j.getStr("type", "heightmap");
  if (type == "biome_heightmap") {
    const Json* list = j.get("biomes");
    if (!list || list->type != Json::Type::Array || list->arr.empty()) {
      l.fail("terrain", "'biome_heightmap' needs a non-empty 'biomes' array of {output, material}");
      return nullptr;
    }
    std::vector<BiomeHeightmapTerrain::Entry> entries;
    for (const Json& e : list->arr) {
      BiomeHeightmapTerrain::Entry en;
      if (!l.material(e, "material", en.material, "terrain") || !l.output(e, "output", en.output, "terrain"))
        return nullptr;
      // Optional mountain uplift: {"uplift": "<graph output>", "uplift_amplitude": <blocks>}.
      // The uplift output (a mountain_range/peak node, amplitude 1 there) is scaled by
      // uplift_amplitude and added to this biome's height before biome blending.
      if (const Json* u = e.get("uplift")) {
        if (u->type != Json::Type::String)
          return l.fail("terrain", "'uplift' must be a graph output name"), nullptr;
        en.upliftOutput = l.graph->outputIndex(u->str);
        if (en.upliftOutput < 0)
          return l.fail("terrain", "graph has no output '" + u->str + "'"), nullptr;
        en.upliftAmplitude = float(e.getNum("uplift_amplitude", 1.0));
        if (!std::isfinite(en.upliftAmplitude))
          return l.fail("terrain", "'uplift_amplitude' must be finite"), nullptr;
      } else if (e.get("uplift_amplitude")) {
        return l.fail("terrain", "'uplift_amplitude' needs an 'uplift' output"), nullptr;
      }
      entries.push_back(en);
    }
    return std::make_unique<BiomeHeightmapTerrain>(l.graph, std::move(entries), int(j.getNum("depth", 0)));
  }
  MaterialId m{};
  int out = -1;
  if (!l.material(j, "material", m, "terrain") || !l.output(j, "output", out, "terrain")) return nullptr;
  if (type == "heightmap") return std::make_unique<HeightmapTerrain>(l.graph, out, m, int(j.getNum("depth", 0)));
  if (type == "density")
    return std::make_unique<DensityTerrain>(l.graph, out, m, int(j.getNum("min_y", 0)), int(j.getNum("max_y", 255)));
  l.fail("terrain", "unknown type '" + type + "' (heightmap, biome_heightmap, density)");
  return nullptr;
}

std::unique_ptr<IClimateStage> loadClimate(Loader& l, const Json& j) {
  std::array<int, kClimateParamCount> outputs{-1, -1, -1, -1};
  for (size_t p = 0; p < kClimateParamCount; ++p)
    if (j.get(std::string(kClimateNames[p])) && !l.output(j, std::string(kClimateNames[p]).c_str(), outputs[p], "climate"))
      return nullptr;
  return std::make_unique<GraphClimate>(l.graph, outputs, float(j.getNum("fallback", 0.5)));
}

bool loadBiomes(Loader& l, const Json& list, std::vector<BiomePoint>& points, std::vector<SurfaceLayers>& surfaces) {
  if (list.type != Json::Type::Array || list.arr.empty() || list.arr.size() > 256)
    return l.fail("biomes", "must be an array of 1..256 biomes");
  for (const Json& b : list.arr) {
    BiomePoint p;
    p.name = b.getStr("name");
    const std::string section = "biome '" + p.name + "'";
    if (const Json* c = b.get("climate"))
      for (size_t k = 0; k < kClimateParamCount; ++k) {
        const Json* v = c->get(std::string(kClimateNames[k]));
        p.point[k] = v ? float(v->num) : 0.5f;
        p.weight[k] = v ? 1.f : 0.f;  // only the listed parameters count
      }
    SurfaceLayers s;
    if (const Json* surf = b.get("surface")) {
      if (!l.material(*surf, "top", s.top, section, false) || !l.material(*surf, "filler", s.filler, section, false))
        return false;
      s.fillerDepth = int(surf->getNum("depth", 0));
    }
    points.push_back(std::move(p));
    surfaces.push_back(s);
  }
  return true;
}

bool loadCarverDensity(Loader& l, const Json& j, const std::vector<std::string>& biomeNames,
                       const char* section, std::vector<float>& out);  // defined below

std::unique_ptr<ICarver> loadRuinsCarver(Loader& l, const Json& j, const std::vector<std::string>& biomeNames) {
  constexpr const char* section = "carver 'ruins'";
  RuinsSettings s;
  if (!l.material(j, "trace", s.trace, section) || !l.material(j, "via", s.via, section)) return nullptr;
  if (const Json* crust = j.get("crust")) {
    if (crust->type != Json::Type::Array || crust->arr.empty())
      return l.fail(section, "'crust' must be a non-empty array of material names"), nullptr;
    for (const Json& m : crust->arr) {
      if (m.type != Json::Type::String)
        return l.fail(section, "'crust' entries must be material names"), nullptr;
      MaterialId id{};
      Json synth = Json::object();
      synth.add("m", Json::string(m.str));
      if (!l.material(synth, "m", id, section)) return nullptr;
      s.crust.push_back(id);
    }
  } else {
    return l.fail(section, "missing 'crust' (replaceable crust materials)"), nullptr;
  }
  if (const Json* delam = j.get("delam")) {
    if (delam->type != Json::Type::Array || delam->arr.size() != 2)
      return l.fail(section, "'delam' must be an array of 2 material names"), nullptr;
    Json synth = Json::object();
    synth.add("a", Json::string(delam->arr[0].str));
    synth.add("b", Json::string(delam->arr[1].str));
    if (!l.material(synth, "a", s.delamA, section) || !l.material(synth, "b", s.delamB, section)) return nullptr;
  }
  if (const Json* density = j.get("density")) {
    if (density->type != Json::Type::Object)
      return l.fail(section, "'density' must be an object mapping biome names to multipliers"), nullptr;
    s.density.assign(biomeNames.size(), 1.f);
    for (const auto& [name, v] : density->obj) {
      if (v.type != Json::Type::Number)
        return l.fail(section, "'density' value for '" + name + "' must be a number"), nullptr;
      const auto it = std::find(biomeNames.begin(), biomeNames.end(), name);
      if (it == biomeNames.end())
        return l.fail(section, "'density' names unknown biome '" + name + "'"), nullptr;
      s.density[size_t(it - biomeNames.begin())] = float(v.num);
    }
  }
  s.depthMin = int(j.getNum("depth_min", s.depthMin));
  s.depthMax = int(j.getNum("depth_max", s.depthMax));
  s.surfaceMargin = int(j.getNum("surface_margin", s.surfaceMargin));
  s.traceMinLen = int(j.getNum("trace_min_len", s.traceMinLen));
  s.traceMaxLen = int(j.getNum("trace_max_len", s.traceMaxLen));
  s.tracesPerChunk = int(j.getNum("traces_per_chunk", s.tracesPerChunk));
  s.gapChance = float(j.getNum("gap_chance", s.gapChance));
  s.voidChance = float(j.getNum("void_chance", s.voidChance));
  s.viasPerChunk = int(j.getNum("vias_per_chunk", s.viasPerChunk));
  s.viaMinH = int(j.getNum("via_min_h", s.viaMinH));
  s.viaMaxH = int(j.getNum("via_max_h", s.viaMaxH));
  s.viaTraceChance = float(j.getNum("via_trace_chance", s.viaTraceChance));
  s.delamChance = float(j.getNum("delam_chance", s.delamChance));
  // Open mode (2026-10-13): config-gated, default off = today's inlay behaviour.
  s.openMode = j.getBool("open_mode", s.openMode);
  if (!l.material(j, "rubble", s.rubble, section, false)) return nullptr;
  s.openRoomsPerChunk = int(j.getNum("open_rooms_per_chunk", s.openRoomsPerChunk));
  s.openMinSize = int(j.getNum("open_min_size", s.openMinSize));
  s.openMaxSize = int(j.getNum("open_max_size", s.openMaxSize));
  s.openHeight = int(j.getNum("open_height", s.openHeight));
  s.rubbleChance = float(j.getNum("rubble_chance", s.rubbleChance));
  return std::make_unique<RuinsCarver>(std::move(s));
}

std::unique_ptr<ICarver> loadCheeseCarver(Loader& l, const Json& j, const std::vector<std::string>& biomeNames) {
  constexpr const char* section = "carver 'cheese'";
  CheeseSettings s;
  s.threshold = float(j.getNum("threshold", s.threshold));
  s.frequency = float(j.getNum("frequency", s.frequency));
  s.minY = int(j.getNum("min_y", s.minY));
  s.surfaceMargin = int(j.getNum("surface_margin", s.surfaceMargin));
  if (!loadCarverDensity(l, j, biomeNames, section, s.density)) return nullptr;
  return std::make_unique<CheeseCarver>(s);
}

template <typename CarverT>
std::unique_ptr<ICarver> loadTubeCarver(Loader& l, const Json& j, const std::vector<std::string>& biomeNames,
                                        const char* section) {
  TubeCaveSettings s = CarverT().settings();  // tier defaults (spaghetti vs noodle)
  s.tubeRadius = float(j.getNum("tube_radius", s.tubeRadius));
  s.frequency = float(j.getNum("frequency", s.frequency));
  s.minY = int(j.getNum("min_y", s.minY));
  s.surfaceMargin = int(j.getNum("surface_margin", s.surfaceMargin));
  if (!loadCarverDensity(l, j, biomeNames, section, s.density)) return nullptr;
  return std::make_unique<CarverT>(s);
}

std::unique_ptr<ICarver> loadRiverCarver(Loader& l, const Json& j, const std::vector<std::string>& biomeNames) {
  constexpr const char* section = "carver 'river'";
  RiverSettings s;
  if (!l.material(j, "water_material", s.water, section)) return nullptr;
  if (!l.material(j, "bed_material", s.bed, section, false)) return nullptr;
  const std::string routing = j.getStr("routing_output", "height");
  const int out = l.graph->outputIndex(routing);
  if (out < 0) {
    l.fail(section, "graph has no output '" + routing + "'");
    return nullptr;
  }
  s.cellSize = int(j.getNum("cell_size", s.cellSize));
  s.chance = float(j.getNum("chance", s.chance));
  s.maxSteps = int(j.getNum("max_steps", s.maxSteps));
  s.stepLen = int(j.getNum("step_len", s.stepLen));
  s.width = float(j.getNum("width", s.width));
  s.depth = float(j.getNum("depth", s.depth));
  s.freeboard = float(j.getNum("freeboard", s.freeboard));
  s.meander = float(j.getNum("meander", s.meander));
  s.meanderFreq = float(j.getNum("meander_freq", s.meanderFreq));
  s.momentum = float(j.getNum("momentum", s.momentum));
  s.minSlope = float(j.getNum("min_slope", s.minSlope));
  s.uphillTol = float(j.getNum("uphill_tol", s.uphillTol));
  s.waterTable = float(j.getNum("water_table", s.waterTable));
  s.minStartHeight = float(j.getNum("min_start_height", s.minStartHeight));
  s.maxStartHeight = float(j.getNum("max_start_height", s.maxStartHeight));
  // Per-biome spawn density: 1.0 for every biome, then "biomes" overrides.
  s.density.fill(1.f);
  if (const Json* b = j.get("biomes")) {
    if (b->type != Json::Type::Object) {
      l.fail(section, "'biomes' must be an object mapping biome names to densities");
      return nullptr;
    }
    for (const auto& [name, v] : b->obj) {
      if (v.type != Json::Type::Number) {
        l.fail(section, "density for biome '" + name + "' must be a number");
        return nullptr;
      }
      const auto it = std::find(biomeNames.begin(), biomeNames.end(), name);
      if (it == biomeNames.end()) {
        l.fail(section, "unknown biome '" + name + "'");
        return nullptr;
      }
      const float d = float(v.num);
      if (!(d >= 0.f && d <= 1.f)) {
        l.fail(section, "density for biome '" + name + "' must be in [0, 1]");
        return nullptr;
      }
      s.density[size_t(it - biomeNames.begin())] = d;
    }
  }
  return std::make_unique<RiverCarver>(l.graph, out, std::move(s));
}

/// Per-biome density multipliers for the cross-chunk carvers ("density": {"biome": m}).
/// Missing object = no per-biome gating (empty table, every entry reads as 1). Unknown biome
/// names are an error.
bool loadCarverDensity(Loader& l, const Json& j, const std::vector<std::string>& biomeNames,
                       const char* section, std::vector<float>& out) {
  const Json* density = j.get("density");
  if (!density) return true;
  if (density->type != Json::Type::Object)
    return l.fail(section, "'density' must be an object mapping biome names to multipliers");
  out.assign(biomeNames.size(), 1.f);
  for (const auto& [name, v] : density->obj) {
    if (v.type != Json::Type::Number)
      return l.fail(section, "'density' value for '" + name + "' must be a number");
    const auto it = std::find(biomeNames.begin(), biomeNames.end(), name);
    if (it == biomeNames.end()) return l.fail(section, "'density' names unknown biome '" + name + "'");
    const float m = float(v.num);
    if (m < 0.f) return l.fail(section, "'density' multiplier for '" + name + "' must be >= 0");
    out[size_t(it - biomeNames.begin())] = m;
  }
  return true;
}

std::unique_ptr<ICarver> loadRavineCarver(Loader& l, const Json& j,
                                         const std::vector<std::string>& biomeNames) {
  constexpr const char* section = "carver 'ravine'";
  RavineSettings s;
  s.chance = float(j.getNum("chance", s.chance));
  s.minLength = int(j.getNum("min_length", s.minLength));
  s.maxLength = int(j.getNum("max_length", s.maxLength));
  s.minWidth = float(j.getNum("min_width", s.minWidth));
  s.maxWidth = float(j.getNum("max_width", s.maxWidth));
  s.minDepth = float(j.getNum("min_depth", s.minDepth));
  s.maxDepth = float(j.getNum("max_depth", s.maxDepth));
  s.meander = float(j.getNum("meander", s.meander));
  s.bottomMargin = int(j.getNum("bottom_margin", s.bottomMargin));
  if (!loadCarverDensity(l, j, biomeNames, section, s.density)) return nullptr;
  return std::make_unique<RavineCarver>(s);
}

std::unique_ptr<ICarver> loadCavernCarver(Loader& l, const Json& j,
                                         const std::vector<std::string>& biomeNames) {
  constexpr const char* section = "carver 'cavern'";
  CavernSettings s;
  s.cellSize = int(j.getNum("cell_size", s.cellSize));
  s.chance = float(j.getNum("chance", s.chance));
  s.minRadius = int(j.getNum("min_radius", s.minRadius));
  s.maxRadius = int(j.getNum("max_radius", s.maxRadius));
  s.squashY = float(j.getNum("squash_y", s.squashY));
  s.minY = int(j.getNum("min_y", s.minY));
  s.maxY = int(j.getNum("max_y", s.maxY));
  s.surfaceMargin = int(j.getNum("surface_margin", s.surfaceMargin));
  s.noiseFreq = float(j.getNum("noise_freq", s.noiseFreq));
  s.coreFrac = float(j.getNum("core_frac", s.coreFrac));
  if (!loadCarverDensity(l, j, biomeNames, section, s.density)) return nullptr;
  return std::make_unique<CavernCarver>(s);
}

std::unique_ptr<ICarver> loadCarver(Loader& l, const Json& j, const std::vector<std::string>& biomeNames) {
  const std::string type = j.getStr("type");
  if (type == "ravine") return loadRavineCarver(l, j, biomeNames);
  if (type == "cavern") return loadCavernCarver(l, j, biomeNames);
  if (type == "ruins") return loadRuinsCarver(l, j, biomeNames);
  if (type == "river") return loadRiverCarver(l, j, biomeNames);
  if (type == "cheese") return loadCheeseCarver(l, j, biomeNames);
  if (type == "spaghetti") return loadTubeCarver<SpaghettiCarver>(l, j, biomeNames, "carver 'spaghetti'");
  if (type == "noodle") return loadTubeCarver<NoodleCarver>(l, j, biomeNames, "carver 'noodle'");
  if (type != "worm_cave") {
    l.fail("carvers",
           "unknown type '" + type + "' (worm_cave, ravine, cavern, ruins, river, cheese, spaghetti, noodle)");
    return nullptr;
  }
  WormCaveSettings s;
  s.chance = float(j.getNum("chance", s.chance));
  s.minLength = int(j.getNum("min_length", s.minLength));
  s.maxLength = int(j.getNum("max_length", s.maxLength));
  s.minRadius = float(j.getNum("min_radius", s.minRadius));
  s.maxRadius = float(j.getNum("max_radius", s.maxRadius));
  s.minY = int(j.getNum("min_y", s.minY));
  s.maxY = int(j.getNum("max_y", s.maxY));
  s.surfaceMargin = int(j.getNum("surface_margin", s.surfaceMargin));
  s.branchChance = float(j.getNum("branch_chance", s.branchChance));
  s.maxBranches = int(j.getNum("max_branches", s.maxBranches));
  s.branchLengthFrac = float(j.getNum("branch_length_frac", s.branchLengthFrac));
  s.branchRadiusFrac = float(j.getNum("branch_radius_frac", s.branchRadiusFrac));
  if (!loadCarverDensity(l, j, biomeNames, "carver 'worm_cave'", s.density)) return nullptr;
  return std::make_unique<WormCaveCarver>(s);
}

/// Fills a per-biome density table from "density" (default) plus the optional "biomes"
/// object mapping biome names to per-column probabilities. Unknown biome names are an error.
///
/// B-L3 (P3-2) scalar footgun: a nonzero scalar "density" WITHOUT a "biomes"
/// object used to fill EVERY biome's density with that scalar — so an author
/// who wrote "density": 0.5 naming the biomes they meant elsewhere (or
/// copying the feature-level "density" key, which several feature types
/// ALSO read as their overall probability) silently enabled the feature in
/// all 256 biomes. Now: a nonzero scalar with no "biomes" object is IGNORED
/// — the table fills 0 and a loud error is logged (the load itself
/// succeeds; density 0 is the safe failure mode for a decoration gate).
/// With a "biomes" object present the scalar remains the default for
/// unlisted biomes, exactly as before (wafer.json relies on this: every
/// scalar density in it is paired with a "biomes" object, so its loading
/// is unchanged).
bool loadBiomeDensity(Loader& l, const Json& j, const std::vector<std::string>& biomeNames,
                      const std::string& section, std::array<float, 256>& out) {
  const Json* b = j.get("biomes");
  const double scalar = j.getNum("density", 0);
  if (!b && scalar != 0) {
    out.fill(0.f);
    logError("procgen: %s: scalar 'density' (%g) without a 'biomes' object is IGNORED "
             "(per-biome densities all 0) — name the biomes it applies to in a 'biomes' object (B-L3)",
             section.c_str(), scalar);
    return true;
  }
  out.fill(float(scalar));
  if (!b) return true;
  if (b->type != Json::Type::Object)
    return l.fail(section, "'biomes' must be an object mapping biome names to densities");
  for (const auto& [name, v] : b->obj) {
    if (v.type != Json::Type::Number)
      return l.fail(section, "density for biome '" + name + "' must be a number");
    const auto it = std::find(biomeNames.begin(), biomeNames.end(), name);
    if (it == biomeNames.end()) return l.fail(section, "unknown biome '" + name + "'");
    const float d = float(v.num);
    if (!(d >= 0.f && d <= 1.f)) return l.fail(section, "density for biome '" + name + "' must be in [0, 1]");
    out[size_t(it - biomeNames.begin())] = d;
  }
  return true;
}

/// Loads a "lithoform_stamp" feature: a baked .lthm model (via the Loader's ModelResolver)
/// stamped into the world on a coarse cell lattice. JSON keys: "model" (resolver name),
/// "materials" ({"<palette byte>": "<material name>"}), "cell_size", "density", "biomes",
/// "rotate" (0/1), "y_offset", "anchor_center" (0/1), "seed".
std::unique_ptr<IFeature> loadLithoStampFeature(Loader& l, const Json& j,
                                               const std::vector<std::string>& biomeNames) {
  const std::string section = "feature '" + j.getStr("name") + "'";
  if (!l.models)
    return l.fail(section, "'lithoform_stamp' needs a ModelResolver (the game did not supply one)"),
           nullptr;
  const std::string modelName = j.getStr("model");
  if (modelName.empty()) return l.fail(section, "missing 'model'"), nullptr;
  const auto model = l.models(modelName);
  if (!model) return l.fail(section, "unknown model '" + modelName + "'"), nullptr;
  const Json* mats = j.get("materials");
  if (!mats || mats->type != Json::Type::Object || mats->obj.empty())
    return l.fail(section, "'materials' must be a non-empty object mapping palette bytes to names"),
           nullptr;
  std::array<MaterialId, 256> pal;
  pal.fill(kAirMaterial);
  for (const auto& [key, v] : mats->obj) {
    if (v.type != Json::Type::String)
      return l.fail(section, "'materials' values must be material names"), nullptr;
    int p = -1;
    try {
      size_t pos = 0;
      p = std::stoi(key, &pos);
      if (pos != key.size()) p = -1;
    } catch (...) {
      p = -1;
    }
    if (p < 0 || p > 255)
      return l.fail(section, "bad palette key '" + key + "' (decimal 0..255)"), nullptr;
    const auto id = l.resolveMaterial(v.str);
    if (!id) return l.fail(section, "unknown material '" + v.str + "'"), nullptr;
    pal[size_t(p)] = *id;
  }
  LithoStampSettings s;
  s.seedName = j.getStr("seed", j.getStr("name"));
  s.cellSize = std::max(16, int(j.getNum("cell_size", s.cellSize)));
  s.density = std::clamp(float(j.getNum("density", s.density)), 0.f, 1.f);
  s.biomes.density.fill(1.f);
  if (!loadBiomeDensity(l, j, biomeNames, section, s.biomes.density)) return nullptr;
  s.rotate = j.getNum("rotate", 1.0) != 0.0;
  s.yOffset = int(j.getNum("y_offset", 0.0));
  s.anchorCenter = j.getNum("anchor_center", 0.0) != 0.0;
  // Model cells are unit cells in [0, size); recenter x/z on the model middle so the
  // 90-degree rotations pivot around the center. y stays 0-up from the model bottom.
  std::vector<LithoStampVoxel> cells;
  cells.reserve(model->cells.size());
  const int half = model->size / 2;
  for (const auto& c : model->cells) {
    const MaterialId m = pal[c.palette];
    if (m == kAirMaterial) continue;  // unmapped palette byte: leave the cell empty
    cells.push_back(LithoStampVoxel{c.x - half, c.y, c.z - half, m});
  }
  if (cells.empty())
    return l.fail(section, "model '" + modelName + "' produced no stampable cells"), nullptr;
  return std::make_unique<LithoStampFeature>(s, std::move(cells));
}

std::unique_ptr<IFeature> loadStructureFeature(Loader& l, const Json& j,
                                              const std::vector<std::string>& biomeNames) {
  const std::string section = "feature '" + j.getStr("name") + "'";
  StructureSettings s;
  s.seedName = j.getStr("seed", j.getStr("name"));
  s.cellSize = std::max(8, int(j.getNum("cell_size", s.cellSize)));
  s.density = std::clamp(float(j.getNum("density", s.density)), 0.f, 1.f);
  // Per-biome gate multipliers: default 1 (all biomes), per-name overrides in [0, 1].
  s.biomes.density.fill(1.f);
  if (const Json* b = j.get("biomes")) {
    if (b->type != Json::Type::Object)
      return l.fail(section, "'biomes' must be an object mapping biome names to multipliers"), nullptr;
    for (const auto& [name, v] : b->obj) {
      if (v.type != Json::Type::Number)
        return l.fail(section, "multiplier for biome '" + name + "' must be a number"), nullptr;
      const auto it = std::find(biomeNames.begin(), biomeNames.end(), name);
      if (it == biomeNames.end())
        return l.fail(section, "unknown biome '" + name + "'"), nullptr;
      const float m = float(v.num);
      if (!(m >= 0.f && m <= 1.f))
        return l.fail(section, "multiplier for biome '" + name + "' must be in [0, 1]"), nullptr;
      s.biomes.density[size_t(it - biomeNames.begin())] = m;
    }
  }
  l.material(j, "surface", s.surface, section, false);
  s.snap = std::max(0, int(j.getNum("snap", s.snap)));
  s.rotate = j.getBool("rotate", s.rotate);
  // ---- placement solver rules (2026-10-12) ----
  s.spacing = std::clamp(float(j.getNum("spacing", s.spacing)), 0.f, 4096.f);
  s.minSlope = std::max(0.f, float(j.getNum("min_slope", s.minSlope)));
  s.maxSlope = float(j.getNum("max_slope", s.maxSlope));
  if (!(s.maxSlope >= s.minSlope))
    return l.fail(section, "'max_slope' must be >= 'min_slope'"), nullptr;
  s.minHeight = int(j.getNum("min_height", s.minHeight));
  s.maxHeight = int(j.getNum("max_height", s.maxHeight));
  if (s.maxHeight < s.minHeight)
    return l.fail(section, "'max_height' must be >= 'min_height'"), nullptr;
  s.nearWater = j.getBool("near_water", s.nearWater);
  s.waterRadius = std::max(1, int(j.getNum("water_radius", s.waterRadius)));
  s.seaLevel = int(j.getNum("sea_level", s.seaLevel));
  const std::string anchor = j.getStr("anchor", "surface");
  if (anchor == "surface") s.anchor = AnchorMode::Surface;
  else if (anchor == "fixed_y") s.anchor = AnchorMode::FixedY;
  else if (anchor == "floating") s.anchor = AnchorMode::Floating;
  else return l.fail(section, "'anchor' must be 'surface', 'fixed_y' or 'floating'"), nullptr;
  s.fixedY = int(j.getNum("fixed_y", s.fixedY));
  s.hoverHeight = int(j.getNum("hover_height", s.hoverHeight));
  // ---- blueprint source: registry name XOR inline cells ----
  const std::string structName = j.getStr("structure", "");
  const Json* bp = j.get("blueprint");
  if (!structName.empty() && bp)
    return l.fail(section, "'structure' and 'blueprint' are mutually exclusive"), nullptr;
  if (!structName.empty()) {
    // Registry-backed: resolve the name now; the feature copies variants + rules.
    const StructureEntry* e = StructureRegistry::instance().find(structName);
    if (!e) return l.fail(section, "unknown structure '" + structName + "' (not in the registry)"), nullptr;
    // Merge: explicit JSON rule fields override the entry's rules. (A field is
    // "explicit" when present in the JSON; otherwise the entry's value stands.)
    StructurePlacementRules rules = e->rules;
    if (j.get("spacing")) rules.spacing = s.spacing;
    if (j.get("min_slope")) rules.minSlope = s.minSlope;
    if (j.get("max_slope")) rules.maxSlope = s.maxSlope;
    if (j.get("min_height")) rules.minHeight = s.minHeight;
    if (j.get("max_height")) rules.maxHeight = s.maxHeight;
    if (j.get("near_water")) rules.nearWater = s.nearWater;
    if (j.get("water_radius")) rules.waterRadius = s.waterRadius;
    if (j.get("sea_level")) rules.seaLevel = s.seaLevel;
    if (j.get("anchor")) rules.anchor = s.anchor;
    if (j.get("fixed_y")) rules.fixedY = s.fixedY;
    if (j.get("hover_height")) rules.hoverHeight = s.hoverHeight;
    s.spacing = rules.spacing;
    s.minSlope = rules.minSlope;
    s.maxSlope = rules.maxSlope;
    s.minHeight = rules.minHeight;
    s.maxHeight = rules.maxHeight;
    s.nearWater = rules.nearWater;
    s.waterRadius = rules.waterRadius;
    s.seaLevel = rules.seaLevel;
    s.anchor = rules.anchor;
    s.fixedY = rules.fixedY;
    s.hoverHeight = rules.hoverHeight;
    s.structureName = structName;
    std::vector<StructureFeature::Variant> variants;
    variants.reserve(e->variants.size());
    for (const auto& v : e->variants) variants.push_back({v.cells, v.weight});
    return std::make_unique<StructureFeature>(std::move(s), std::move(variants));
  }
  if (!bp || bp->type != Json::Type::Array || bp->arr.empty())
    return l.fail(section, "'blueprint' must be a non-empty array of {d, m} cells (or use 'structure': \"<name>\")"), nullptr;
  std::vector<BlueprintCell> cells;
  cells.reserve(bp->arr.size());
  for (const Json& c : bp->arr) {
    const Json* d = c.get("d");
    if (!d || d->type != Json::Type::Array || d->arr.size() != 3)
      return l.fail(section, "blueprint cell needs 'd': [dx, dy, dz]"), nullptr;
    BlueprintCell cell;
    bool bad = false;
    int dv[3] = {0, 0, 0};
    for (int k = 0; k < 3; ++k) {
      if (d->arr[size_t(k)].type != Json::Type::Number) bad = true;
      const double v = d->arr[size_t(k)].num;
      // Bound the coordinates: unclamped values overflow int conversion (UB)
      // and blow up reach_ / std::abs(INT_MIN). 512 horizontal, 1024 vertical
      // is beyond any sane structure.
      const double lim = (k == 1) ? 1024.0 : 512.0;
      if (!std::isfinite(v) || v < -lim || v > lim) bad = true;
      dv[k] = int(v);
    }
    if (bad) return l.fail(section, "blueprint 'd' entries must be finite numbers within ±512 (dy ±1024)"), nullptr;
    cell.dx = dv[0];
    cell.dy = dv[1];
    cell.dz = dv[2];
    // dy < 0 (buried) is allowed now: place() overwrites terrain for buried cells.
    if (!l.material(c, "m", cell.material, section)) return nullptr;
    cell.decay = std::clamp(float(c.getNum("decay", 0)), 0.f, 1.f);
    cells.push_back(cell);
  }
  return std::make_unique<StructureFeature>(std::move(s), std::move(cells));
}

// ---- ecosystem diversity (2026-10-12) ----

std::unique_ptr<IFeature> loadGroundCoverFeature(Loader& l, const Json& j,
                                                const std::vector<std::string>& biomeNames) {
  const std::string section = "feature '" + j.getStr("name") + "'";
  GroundCoverSettings s;
  s.seedName = j.getStr("seed", "");
  const Json* entries = j.get("entries");
  if (!entries || entries->type != Json::Type::Array || entries->arr.empty()) {
    l.fail(section, "'entries' must be a non-empty array");
    return nullptr;
  }
  for (const Json& e : entries->arr) {
    if (e.type != Json::Type::Object) {
      l.fail(section, "groundcover entry must be an object");
      return nullptr;
    }
    GroundCoverEntry entry;
    if (!l.material(e, "material", entry.material, section)) return nullptr;
    l.material(e, "surface", entry.surface, section, false);
    entry.minH = std::max(1, int(e.getNum("min_height", entry.minH)));
    entry.maxH = std::max(entry.minH, int(e.getNum("max_height", entry.maxH)));
    entry.tintByBiome = e.getStr("tint", "") == "biome";
    if (!loadBiomeDensity(l, e, biomeNames, section, entry.biomes.density)) return nullptr;
    s.entries.push_back(std::move(entry));
  }
  return std::make_unique<GroundCoverFeature>(std::move(s));
}

std::unique_ptr<IFeature> loadRockFormationFeature(Loader& l, const Json& j,
                                                  const std::vector<std::string>& biomeNames) {
  const std::string section = "feature '" + j.getStr("name") + "'";
  RockFormationSettings s;
  s.seedName = j.getStr("seed", "");
  if (!l.material(j, "material", s.material, section)) return nullptr;
  l.material(j, "surface", s.surface, section, false);
  // Clamp: unclamped max_radius/max_height lets a hostile JSON hang the gen
  // thread ((2r+1)^2 loop) or overflow dx*dx. 64 blocks is beyond any sane rock.
  s.minR = std::clamp(int(j.getNum("min_radius", s.minR)), 1, 64);
  s.maxR = std::clamp(int(j.getNum("max_radius", s.maxR)), s.minR, 64);
  s.minH = std::clamp(int(j.getNum("min_height", s.minH)), 1, 256);
  s.maxH = std::clamp(int(j.getNum("max_height", s.maxH)), s.minH, 256);
  s.wobble = std::clamp(float(j.getNum("wobble", s.wobble)), 0.f, 0.95f);
  s.density = std::clamp(float(j.getNum("density", s.density)), 0.f, 1.f);
  if (!loadBiomeDensity(l, j, biomeNames, section, s.biomes.density)) return nullptr;
  return std::make_unique<RockFormationFeature>(std::move(s));
}

std::unique_ptr<IFeature> loadWorleyOreFeature(Loader& l, const Json& j,
                                              const std::vector<std::string>&) {
  const std::string section = "feature '" + j.getStr("name") + "'";
  WorleyOreVeinSettings s;
  s.seedName = j.getStr("seed", "");
  if (!l.material(j, "material", s.material, section) || !l.material(j, "replace", s.replace, section))
    return nullptr;
  s.minY = int(j.getNum("min_y", s.minY));
  s.maxY = std::max(s.minY, int(j.getNum("max_y", s.maxY)));
  s.frequency = float(j.getNum("frequency", s.frequency));
  if (!(s.frequency > 0.f) || !std::isfinite(s.frequency)) {
    l.fail(section, "'frequency' must be a positive finite number");
    return nullptr;
  }
  s.threshold = std::clamp(float(j.getNum("threshold", s.threshold)), 0.f, 2.f);
  s.clusterFreq = float(j.getNum("cluster_frequency", s.clusterFreq));
  if (s.clusterFreq < 0.f || !std::isfinite(s.clusterFreq)) {
    l.fail(section, "'cluster_frequency' must be >= 0");
    return nullptr;
  }
  s.clusterThreshold = std::clamp(float(j.getNum("cluster_threshold", s.clusterThreshold)), -1.f, 1.f);
  return std::make_unique<WorleyOreVeinFeature>(std::move(s));
}

/// Parses "biome_transitions": [{"a": name, "b": name, "result": name, "width": n}].
bool loadBiomeTransitions(Loader& l, const Json& json, const std::vector<std::string>& biomeNames,
                          GeneratorPipeline::Builder& b) {
  const Json* list = json.get("biome_transitions");
  if (!list) return true;
  constexpr const char* section = "biome_transitions";
  if (list->type != Json::Type::Array) return l.fail(section, "must be an array");
  std::vector<BiomeTransition> rules;
  for (const Json& t : list->arr) {
    if (t.type != Json::Type::Object) return l.fail(section, "transition must be an object");
    BiomeTransition rule;
    for (const auto [key, out] : {std::pair<const char*, uint8_t*>{"a", &rule.a},
                                  {"b", &rule.b}, {"result", &rule.result}}) {
      const std::string name = t.getStr(key, "");
      const auto it = std::find(biomeNames.begin(), biomeNames.end(), name);
      if (it == biomeNames.end()) return l.fail(section, std::string("unknown biome '") + name + "'");
      *out = uint8_t(it - biomeNames.begin());
    }
    rule.width = std::clamp(int(t.getNum("width", rule.width)), 1, 8);
    rules.push_back(rule);
  }
  if (!rules.empty()) b.biomePost(std::make_unique<BiomeTransitionStage>(std::move(rules)));
  return true;
}

/// Parses "fauna": {"<biome>": [{"mob": id, "weight": w, "pack": [min, max]}]}.
bool loadFauna(Loader& l, const Json& json, const std::vector<std::string>& biomeNames,
               GeneratorPipeline::Builder& b) {
  const Json* fauna = json.get("fauna");
  if (!fauna) return true;
  constexpr const char* section = "fauna";
  if (fauna->type != Json::Type::Object) return l.fail(section, "must be an object keyed by biome name");
  std::vector<std::vector<FaunaEntry>> tables(biomeNames.size());
  for (const auto& [name, v] : fauna->obj) {
    const auto it = std::find(biomeNames.begin(), biomeNames.end(), name);
    if (it == biomeNames.end()) return l.fail(section, std::string("unknown biome '") + name + "'");
    if (v.type != Json::Type::Array) return l.fail(section, "fauna for '" + name + "' must be an array");
    auto& table = tables[size_t(it - biomeNames.begin())];
    for (const Json& e : v.arr) {
      if (e.type != Json::Type::Object) return l.fail(section, "fauna entry must be an object");
      FaunaEntry entry;
      entry.mob = e.getStr("mob", "");
      if (entry.mob.empty()) return l.fail(section, "fauna entry needs 'mob'");
      entry.weight = float(e.getNum("weight", entry.weight));
      if (!(entry.weight > 0.f) || !std::isfinite(entry.weight))
        return l.fail(section, "'weight' must be a positive finite number");
      if (const Json* pack = e.get("pack")) {
        if (pack->type != Json::Type::Array || pack->arr.size() != 2 ||
            pack->arr[0].type != Json::Type::Number || pack->arr[1].type != Json::Type::Number)
          return l.fail(section, "'pack' must be [min, max] numbers");
        entry.minPack = std::max(1, int(pack->arr[0].num));
        entry.maxPack = std::max(entry.minPack, int(pack->arr[1].num));
      }
      table.push_back(std::move(entry));
    }
  }
  b.fauna(std::move(tables));
  return true;
}

/// Parses "snowcap": {"snow": mat, "base_line": y, "latitude_falloff": f, ...}.
bool loadSnowCap(Loader& l, const Json& json, GeneratorPipeline::Builder& b) {
  const Json* sc = json.get("snowcap");
  if (!sc) return true;
  constexpr const char* section = "snowcap";
  if (sc->type != Json::Type::Object) return l.fail(section, "must be an object");
  SnowCapSettings s;
  s.seedName = sc->getStr("seed", "");
  if (!l.material(*sc, "snow", s.snow, section)) return false;
  s.baseLine = int(sc->getNum("base_line", s.baseLine));
  s.latitudeFalloff = float(sc->getNum("latitude_falloff", s.latitudeFalloff));
  s.noiseAmp = float(sc->getNum("noise_amp", s.noiseAmp));
  s.noiseFreq = float(sc->getNum("noise_freq", s.noiseFreq));
  if (!(s.noiseFreq > 0.f) || !std::isfinite(s.noiseFreq))
    return l.fail(section, "'noise_freq' must be a positive finite number");
  s.minHeight = int(sc->getNum("min_height", s.minHeight));
  b.surface(std::make_unique<SnowCapRule>(std::move(s)));
  return true;
}

/// Parses "sand_drifts": {"sand": mat, "frequency": f, "threshold": t, "depth": n, "biomes": {...}}.
bool loadSandDrifts(Loader& l, const Json& json, const std::vector<std::string>& biomeNames,
                     GeneratorPipeline::Builder& b) {
  const Json* sd = json.get("sand_drifts");
  if (!sd) return true;
  constexpr const char* section = "sand_drifts";
  if (sd->type != Json::Type::Object) return l.fail(section, "must be an object");
  SandDriftSettings s;
  s.seedName = sd->getStr("seed", "");
  if (!l.material(*sd, "sand", s.sand, section)) return false;
  s.frequency = float(sd->getNum("frequency", s.frequency));
  if (!(s.frequency > 0.f) || !std::isfinite(s.frequency))
    return l.fail(section, "'frequency' must be a positive finite number");
  s.threshold = std::clamp(float(sd->getNum("threshold", s.threshold)), -1.f, 1.f);
  s.depth = std::max(1, int(sd->getNum("depth", s.depth)));
  if (!loadBiomeDensity(l, *sd, biomeNames, section, s.biomes.density)) return false;
  b.surface(std::make_unique<SandDriftRule>(std::move(s)));
  return true;
}

std::unique_ptr<IFeature> loadFeature(Loader& l, const Json& j, const std::vector<std::string>& biomeNames) {
  const std::string type = j.getStr("type"), section = "feature '" + j.getStr("name") + "'";
  if (type == "dungeon") return std::make_unique<DungeonSlot>();
  if (type == "trace_tree") {
    TraceTreeSettings s;
    if (!l.material(j, "trunk", s.trunk, section) || !l.material(j, "branch", s.branch, section) ||
        !l.material(j, "tip_red", s.tipRed, section) || !l.material(j, "tip_blue", s.tipBlue, section))
      return nullptr;
    l.material(j, "surface", s.surface, section, false);
    s.minTrunkH = std::max(2, int(j.getNum("min_trunk", s.minTrunkH)));
    s.maxTrunkH = std::max(s.minTrunkH, int(j.getNum("max_trunk", s.maxTrunkH)));
    s.minBranches = std::max(1, int(j.getNum("min_branches", s.minBranches)));
    s.maxBranches = std::max(s.minBranches, int(j.getNum("max_branches", s.maxBranches)));
    s.minBranchLen = std::max(1, int(j.getNum("min_branch_len", s.minBranchLen)));
    s.maxBranchLen = std::max(s.minBranchLen, int(j.getNum("max_branch_len", s.maxBranchLen)));
    s.redTipChance = std::clamp(float(j.getNum("red_tip_chance", s.redTipChance)), 0.f, 1.f);
    if (!loadBiomeDensity(l, j, biomeNames, section, s.biomes.density)) return nullptr;
    return std::make_unique<TraceTreeFeature>(s);
  }
  if (type == "capacitor_cans") {
    CapacitorCanSettings s;
    if (!l.material(j, "can", s.can, section)) return nullptr;
    l.material(j, "surface", s.surface, section, false);
    s.minCans = std::max(1, int(j.getNum("min_cans", s.minCans)));
    s.maxCans = std::max(s.minCans, int(j.getNum("max_cans", s.maxCans)));
    s.minCanH = std::max(1, int(j.getNum("min_height", s.minCanH)));
    s.maxCanH = std::max(s.minCanH, int(j.getNum("max_height", s.maxCanH)));
    if (!loadBiomeDensity(l, j, biomeNames, section, s.biomes.density)) return nullptr;
    return std::make_unique<CapacitorCanFeature>(s);
  }
  if (type == "resonator_spire") {
    ResonatorSpireSettings s;
    if (!l.material(j, "crystal", s.crystal, section)) return nullptr;
    l.material(j, "surface", s.surface, section, false);
    s.minH = std::max(1, int(j.getNum("min_height", s.minH)));
    s.maxH = std::max(s.minH, int(j.getNum("max_height", s.maxH)));
    if (!loadBiomeDensity(l, j, biomeNames, section, s.biomes.density)) return nullptr;
    return std::make_unique<ResonatorSpireFeature>(s);
  }
  if (type == "structure") return loadStructureFeature(l, j, biomeNames);
  if (type == "lithoform_stamp") return loadLithoStampFeature(l, j, biomeNames);
  if (type == "groundcover") return loadGroundCoverFeature(l, j, biomeNames);
  if (type == "rock_formation") return loadRockFormationFeature(l, j, biomeNames);
  if (type == "worley_ore") return loadWorleyOreFeature(l, j, biomeNames);
  if (type != "ore_vein") {
    l.fail(section, "unknown type '" + type + "' (ore_vein, worley_ore, dungeon, trace_tree, capacitor_cans, resonator_spire, structure, lithoform_stamp, groundcover, rock_formation)");
    return nullptr;
  }
  OreVeinSettings s;
  if (!l.material(j, "material", s.material, section) || !l.material(j, "replace", s.replace, section)) return nullptr;
  s.veinsPerChunk = int(j.getNum("count", s.veinsPerChunk));
  s.size = int(j.getNum("size", s.size));
  s.minY = int(j.getNum("min_y", s.minY));
  s.maxY = int(j.getNum("max_y", s.maxY));
  return std::make_unique<OreVeinFeature>(s);
}

/// Resolves the "water" material: an explicit "water" key, else the registry's "water".
/// Fails when the registry has no water block: an oceans/lakes section without water is a
/// config error, not a silent no-op.
bool loadWaterMaterial(Loader& l, const Json& j, const char* section, MaterialId& out) {
  if (const Json* w = j.get("water")) {
    if (w->type != Json::Type::String) return l.fail(section, "'water' must be a material name");
    return l.material(j, "water", out, section);
  }
  Json synth = Json::object();
  synth.add("water", Json::string("water"));
  return l.material(synth, "water", out, section);
}

bool loadOceanSettings(Loader& l, const Json& j, const std::vector<std::string>& biomeNames,
                       uint64_t dimensionSeed, OceanSettings& out) {
  constexpr const char* section = "oceans";
  out.seed = dimensionSeed;
  out.seaLevel = int(j.getNum("sea_level", out.seaLevel));
  if (out.seaLevel < 1) return l.fail(section, "'sea_level' must be >= 1");
  out.maxDepth = int(j.getNum("max_depth", out.maxDepth));
  if (out.maxDepth < 1) return l.fail(section, "'max_depth' must be >= 1");
  // No engine cap (2026-10-13 parameterization): max_depth is a settings value a
  // game/mod sets freely (LithoKraft ships 196). The checks below are sanity only.
  if (const Json* md = j.get("min_depth")) {
    if (md->type != Json::Type::Number) return l.fail(section, "'min_depth' must be a number");
    out.minDepth = int(md->num);
    if (out.minDepth < 0 || out.minDepth > out.maxDepth)
      return l.fail(section, "'min_depth' must be in [0, 'max_depth']");
  }
  out.minSolidBelow = int(j.getNum("min_solid_below", out.minSolidBelow));
  if (out.minSolidBelow < 0) return l.fail(section, "'min_solid_below' must be >= 0");
  // Sanity, expressed in data: the deepest ocean floor (sea_level - max_depth) must
  // keep at least min_solid_below solid layers above y 0 (default 60, itself settable).
  if (out.seaLevel - out.maxDepth < out.minSolidBelow)
    return l.fail(section, "'sea_level' - 'max_depth' must leave >= 'min_solid_below' (" +
                            std::to_string(out.minSolidBelow) + ") solid layers above y 0 (got " +
                            std::to_string(out.seaLevel - out.maxDepth) + ")");
  if (!loadWaterMaterial(l, j, section, out.water)) return false;
  if (!l.material(j, "floor", out.floorMaterial, section, false)) return false;
  out.depthFrequency = float(j.getNum("depth_frequency", out.depthFrequency));
  if (!(out.depthFrequency > 0.f) || !std::isfinite(out.depthFrequency))
    return l.fail(section, "'depth_frequency' must be a positive finite number");
  return loadBiomeDensity(l, j, biomeNames, section, out.biomes.density);
}

bool loadLakeSettings(Loader& l, const Json& j, const std::vector<std::string>& biomeNames,
                      uint64_t dimensionSeed, LakeSettings& out) {
  constexpr const char* section = "lakes";
  out.seed = dimensionSeed;
  out.waterTable = int(j.getNum("water_table", out.waterTable));
  if (out.waterTable < 1) return l.fail(section, "'water_table' must be >= 1");
  out.maxDepth = int(j.getNum("max_depth", out.maxDepth));
  if (out.maxDepth < 1) return l.fail(section, "'max_depth' must be >= 1");
  // No engine cap (2026-10-13 parameterization): an independent settings value
  // (LithoKraft ships 98); it is no longer derived from, or limited by, the ocean's.
  if (!loadWaterMaterial(l, j, section, out.water)) return false;
  out.threshold = float(j.getNum("threshold", out.threshold));
  if (!(out.threshold >= 0.f && out.threshold <= 1.f))
    return l.fail(section, "'threshold' must be in [0, 1]");
  out.frequency = float(j.getNum("frequency", out.frequency));
  if (!(out.frequency > 0.f) || !std::isfinite(out.frequency))
    return l.fail(section, "'frequency' must be a positive finite number");
  return loadBiomeDensity(l, j, biomeNames, section, out.biomes.density);
}

bool loadShoreLayers(Loader& l, const Json& j, const char* section, SurfaceLayers& out) {
  if (!l.material(j, "top", out.top, section, false)) return false;
  if (!l.material(j, "filler", out.filler, section, false)) return false;
  out.fillerDepth = int(j.getNum("depth", out.fillerDepth));
  if (out.fillerDepth < 0) return l.fail(section, "'depth' must be >= 0");
  return true;
}

bool loadShorelineSettings(Loader& l, const Json& j, const std::vector<std::string>& biomeNames,
                           ShorelineRule::Settings& out) {
  constexpr const char* section = "shoreline";
  out.band = int(j.getNum("band", out.band));
  out.depth = int(j.getNum("depth", out.depth));
  if (out.band < 0 || out.depth < 0) return l.fail(section, "'band'/'depth' must be >= 0");
  out.perBiome.assign(biomeNames.size(), SurfaceLayers{});
  if (const Json* d = j.get("default")) {
    if (d->type != Json::Type::Object) return l.fail(section, "'default' must be an object");
    SurfaceLayers def;
    if (!loadShoreLayers(l, *d, section, def)) return false;
    std::fill(out.perBiome.begin(), out.perBiome.end(), def);
  }
  if (const Json* b = j.get("biomes")) {
    if (b->type != Json::Type::Object)
      return l.fail(section, "'biomes' must be an object mapping biome names to layers");
    for (const auto& [name, v] : b->obj) {
      if (v.type != Json::Type::Object)
        return l.fail(section, "shoreline for biome '" + name + "' must be an object");
      const auto it = std::find(biomeNames.begin(), biomeNames.end(), name);
      if (it == biomeNames.end()) return l.fail(section, "unknown biome '" + name + "'");
      if (!loadShoreLayers(l, v, section, out.perBiome[size_t(it - biomeNames.begin())])) return false;
    }
  }
  return true;
}

}  // namespace

std::string_view toString(ClimateParam p) { return kClimateNames[size_t(p)]; }

std::unique_ptr<const GeneratorPipeline> loadGenerator(const Json& json, uint64_t dimensionSeed,
                                                       const MaterialResolver& materials, std::string& err) {
  return loadGenerator(json, dimensionSeed, materials, {}, err);
}

std::unique_ptr<const GeneratorPipeline> loadGenerator(const Json& json, uint64_t dimensionSeed,
                                                       const MaterialResolver& materials,
                                                       std::vector<std::unique_ptr<ISurfaceRule>> extraSurface,
                                                       std::string& err) {
  return loadGenerator(json, dimensionSeed, materials, std::move(extraSurface), ModelResolver{}, err);
}

std::unique_ptr<const GeneratorPipeline> loadGenerator(const Json& json, uint64_t dimensionSeed,
                                                       const MaterialResolver& materials,
                                                       std::vector<std::unique_ptr<ISurfaceRule>> extraSurface,
                                                       ModelResolver models, std::string& err) {
  Loader l(materials, err);
  l.models = std::move(models);
  const Json* graphJson = json.get("graph");
  const Json* terrainJson = json.get("terrain");
  if (!graphJson || !terrainJson) {
    l.fail("generator", "needs \"graph\" and \"terrain\"");
    return nullptr;
  }
  GraphDesc desc;
  std::string graphErr;
  if (!GraphDesc::fromJson(*graphJson, desc, graphErr) || !(l.graph = NoiseGraph::compile(desc, dimensionSeed, graphErr))) {
    l.fail("graph", graphErr);
    return nullptr;
  }
  GeneratorPipeline::Builder b(l.graph);
  auto terrain = loadTerrain(l, *terrainJson);
  if (!terrain) return nullptr;
  const auto* biomeTerrain = dynamic_cast<const BiomeHeightmapTerrain*>(terrain.get());
  b.terrain(std::move(terrain));
  if (const Json* c = json.get("climate")) {
    auto climate = loadClimate(l, *c);
    if (!climate) return nullptr;
    b.climate(std::move(climate));
  }
  std::vector<std::string> biomeNames;
  if (const Json* biomesJson = json.get("biomes")) {
    // Array form: hard multi-noise selection (legacy). Object form: {"selector": {...},
    // "list": [...]} with an optional "selector": {"type": "blended", "blend_radius": r,
    // "blend_count": n} for Minecraft-style biome blending.
    const Json* list = biomesJson;
    std::string selectorType = "multi_noise";
    float blendRadius = 0.3f;
    int blendCount = 3;
    if (biomesJson->type == Json::Type::Object) {
      list = biomesJson->get("list");
      if (const Json* sel = biomesJson->get("selector")) {
        selectorType = sel->getStr("type", "multi_noise");
        blendRadius = float(sel->getNum("blend_radius", blendRadius));
        blendCount = int(sel->getNum("blend_count", blendCount));
      }
    } else if (biomesJson->type != Json::Type::Array) {
      l.fail("biomes", "must be an array or {selector, list}");
      return nullptr;
    }
    std::vector<BiomePoint> points;
    std::vector<SurfaceLayers> surfaces;
    if (!list) {
      l.fail("biomes", "object form needs a 'list' array of biomes");
      return nullptr;
    }
    if (!loadBiomes(l, *list, points, surfaces)) return nullptr;
    biomeNames.reserve(points.size());
    for (const auto& bp : points) biomeNames.push_back(bp.name);
    const size_t biomeCount = points.size();
    if (selectorType == "blended") {
      if (!(blendRadius > 0.f)) {
        l.fail("biomes", "the 'blended' selector needs blend_radius > 0");
        return nullptr;
      }
      if (blendCount < 2) blendCount = 2;
      if (blendCount > 4) blendCount = 4;
      b.biomes(std::make_unique<BlendedBiomeSelector>(std::move(points), blendRadius, blendCount));
    } else if (selectorType == "multi_noise") {
      b.biomes(std::make_unique<MultiNoiseBiomeSelector>(std::move(points)));
    } else {
      l.fail("biomes", "unknown selector type '" + selectorType + "' (multi_noise, blended)");
      return nullptr;
    }
    const Json* surface = json.get("surface");
    b.surface(std::make_unique<LayeredSurface>(std::move(surfaces),
                                              surface && surface->getStr("top_state") == "biome", dimensionSeed));
    // Ecosystem diversity (2026-10-12): transition bands rewrite the biome map before
    // terrain runs; fauna tables ride on the pipeline for the entity system.
    if (!loadBiomeTransitions(l, json, biomeNames, b)) return nullptr;
    if (!loadFauna(l, json, biomeNames, b)) return nullptr;
    // Weather-shaped surface rules: snow caps and sand drifts modify the biome surface
    // before water fills.
    if (!loadSnowCap(l, json, b)) return nullptr;
    if (!loadSandDrifts(l, json, biomeNames, b)) return nullptr;
    if (biomeTerrain && biomeTerrain->biomeCount() != biomeCount) {
      l.fail("terrain", "'biome_heightmap' has " + std::to_string(biomeTerrain->biomeCount()) +
                       " entries but the generator defines " + std::to_string(biomeCount) + " biomes");
      return nullptr;
    }
  }
  for (auto& rule : extraSurface) b.surface(std::move(rule));
  // Water, after the JSON surface rules: ocean fill, then lake fill (lakes win columns
  // both claim), then the shoreline rim. Parsed here -- the biome list is needed for the
  // per-biome tables -- but appended after the surface rules by construction order.
  OceanSettings oceanSettings;
  LakeSettings lakeSettings;
  bool haveOcean = false, haveLake = false;
  if (const Json* oj = json.get("oceans")) {
    if (!loadOceanSettings(l, *oj, biomeNames, dimensionSeed, oceanSettings)) return nullptr;
    b.surface(std::make_unique<OceanFillRule>(oceanSettings));
    haveOcean = true;
  }
  if (const Json* lj = json.get("lakes")) {
    if (!loadLakeSettings(l, *lj, biomeNames, dimensionSeed, lakeSettings)) return nullptr;
    b.surface(std::make_unique<LakeFillRule>(lakeSettings));
    haveLake = true;
  }
  if (haveOcean || haveLake || json.get("shoreline")) {
    ShorelineRule::Settings shore;
    if (const Json* sh = json.get("shoreline"))
      if (!loadShorelineSettings(l, *sh, biomeNames, shore)) return nullptr;
    b.surface(std::make_unique<ShorelineRule>(shore, oceanSettings, haveOcean, lakeSettings, haveLake));
  }
  if (const Json* list = json.get("carvers"))
    for (const Json& c : list->arr) {
      auto carver = loadCarver(l, c, biomeNames);
      if (!carver) return nullptr;
      b.carver(std::move(carver));
    }
  if (const Json* list = json.get("features"))
    for (const Json& f : list->arr) {
      auto feature = loadFeature(l, f, biomeNames);
      if (!feature) return nullptr;
      if (!b.feature(f.getStr("name"), std::move(feature))) {
        l.fail("features", "duplicate feature name '" + f.getStr("name") + "'");
        return nullptr;
      }
    }
  return b.build();
}

std::unique_ptr<const GeneratorPipeline> loadGenerator(const std::string& text, uint64_t dimensionSeed,
                                                       const MaterialResolver& materials, std::string& err) {
  return loadGenerator(text, dimensionSeed, materials, {}, err);
}

std::unique_ptr<const GeneratorPipeline> loadGenerator(const std::string& text, uint64_t dimensionSeed,
                                                       const MaterialResolver& materials,
                                                       std::vector<std::unique_ptr<ISurfaceRule>> extraSurface,
                                                       std::string& err) {
  Json j;
  if (!Json::parse(text, j, &err)) return nullptr;
  return loadGenerator(j, dimensionSeed, materials, std::move(extraSurface), err);
}
std::unique_ptr<const GeneratorPipeline> loadGenerator(const std::string& text, uint64_t dimensionSeed,
                                                       const MaterialResolver& materials,
                                                       std::vector<std::unique_ptr<ISurfaceRule>> extraSurface,
                                                       ModelResolver models, std::string& err) {
  Json json;
  if (!Json::parse(text, json, &err)) {
    err = "generator: " + err;
    return nullptr;
  }
  return loadGenerator(json, dimensionSeed, materials, std::move(extraSurface), std::move(models), err);
}


}  // namespace litholattice::procgen
