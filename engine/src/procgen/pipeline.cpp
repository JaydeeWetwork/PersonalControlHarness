#include "litholattice/procgen/pipeline.h"
#include <algorithm>

namespace litholattice::procgen {

bool ColumnBuffer::layerEmpty(int y) const {
  const auto begin = material_.begin() + std::ptrdiff_t(index(0, y, 0));
  return std::all_of(begin, begin + size_ * size_, [](MaterialId m) { return m == kAirMaterial; });
}

bool FeatureRegistry::add(std::string name, std::unique_ptr<IFeature> feature) {
  if (contains(name)) return false;
  features_.push_back({std::move(name), std::move(feature)});
  return true;
}

bool FeatureRegistry::contains(std::string_view name) const {
  return std::any_of(features_.begin(), features_.end(), [&](const Entry& e) { return e.name == name; });
}

void FeatureRegistry::placeAll(StageContext& ctx) const {
  for (const Entry& e : features_) {
    Rng rng(deriveSeed(ctx.chunk.chunkSeed, nameHash(e.name)));
    e.feature->place(ctx, rng);
  }
}

void FeatureRegistry::forEach(const std::function<void(std::string_view, IFeature&)>& fn) {
  for (Entry& e : features_) fn(e.name, *e.feature);
}

void FeatureRegistry::forEach(const std::function<void(std::string_view, const IFeature&)>& fn) const {
  for (const Entry& e : features_) fn(e.name, *e.feature);
}

ColumnBuffer GeneratorPipeline::generate(const ChunkInfo& chunk, ColumnMaps* mapsOut) const {
  const int n = chunk.size;
  const size_t columns = size_t(n) * size_t(n);
  std::vector<float> xs(columns), ys(columns, 0.f), zs(columns), ws(columns, 0.f);
  for (int z = 0; z < n; ++z)
    for (int x = 0; x < n; ++x) {
      xs[size_t(z * n + x)] = float(chunk.worldX(x));
      zs[size_t(z * n + x)] = float(chunk.worldZ(z));
    }
  const PointsView points{xs, ys, zs, ws};
  GraphContext graphCtx;
  graphCtx.bind(points);

  ColumnBuffer blocks(n, chunk.height);
  ColumnMaps maps;
  maps.height.assign(columns, -1);
  maps.climate.assign(columns, ClimatePoint{0.5f, 0.5f, 0.5f, 0.5f});
  maps.biome.assign(columns, 0);
  StageContext ctx{chunk, blocks, maps, graphCtx, points};

  if (climate_) climate_->sample(ctx);
  if (biomes_) {
    const size_t nb = biomes_->biomeCount();
    if (nb > 0 && nb <= 256) {
      maps.biomeCount = nb;
      maps.biomeWeight.assign(columns * nb, 0.f);
    }
    for (size_t i = 0; i < columns; ++i) {
      maps.biome[i] = biomes_->select(maps.climate[i]);
      if (maps.biomeCount > 0) biomes_->blendWeights(maps.climate[i], maps.biomeWeight.data() + i * nb);
    }
  }
  // Biome post-stages (transition bands, ...): rewrite the biome map before terrain runs.
  for (const auto& bp : biomePost_) bp->process(ctx);
  terrain_->generate(ctx);
  for (const auto& s : surface_) s->apply(ctx);
  for (const auto& c : carvers_) c->carve(ctx);
  features_.placeAll(ctx);
  if (mapsOut) *mapsOut = std::move(maps);
  return blocks;
}

namespace {
/// Binds `n` world columns (xs[i], 0, zs[i]) and runs the pipeline's climate stage + biome
/// selector over them, writing the dominant biome-table index per point. Shared by biomeAt
/// (n == 1) and sampleBiomes. Mirrors generate()'s binding (y = 0, w = 0).
void selectBiomesAt(const IClimateStage* climate, const IBiomeSelector* biomes,
                    const int32_t* xs, const int32_t* zs, size_t n, uint8_t* out) {
  std::vector<float> fxs(n), fys(n, 0.f), fzs(n), fws(n, 0.f);
  for (size_t i = 0; i < n; ++i) {
    fxs[i] = float(xs[i]);
    fzs[i] = float(zs[i]);
  }
  const PointsView points{fxs, fys, fzs, fws};
  GraphContext graphCtx;
  graphCtx.bind(points);
  ChunkInfo chunk;  // 1-wide pseudo-chunk; only the binding above matters here
  chunk.size = 1;
  ColumnBuffer blocks(1, 2);
  ColumnMaps maps;
  maps.height.assign(n, -1);
  maps.climate.assign(n, ClimatePoint{0.5f, 0.5f, 0.5f, 0.5f});
  maps.biome.assign(n, 0);
  StageContext ctx{chunk, blocks, maps, graphCtx, points};
  if (climate) climate->sample(ctx);
  for (size_t i = 0; i < n; ++i) out[i] = biomes ? biomes->select(maps.climate[i]) : 0;
}
}  // namespace

uint8_t GeneratorPipeline::biomeAt(int32_t wx, int32_t wz) const {
  uint8_t out = 0;
  selectBiomesAt(climate_.get(), biomes_.get(), &wx, &wz, 1, &out);
  return out;
}

void GeneratorPipeline::sampleBiomes(const int32_t* xs, const int32_t* zs, size_t n, uint8_t* out) const {
  if (n == 0) return;
  selectBiomesAt(climate_.get(), biomes_.get(), xs, zs, n, out);
}

void GeneratorPipeline::visitFeatures(const std::function<void(std::string_view, const IFeature&)>& fn) const {
  features_.forEach(fn);
}

const std::vector<FaunaEntry>& GeneratorPipeline::faunaForBiome(uint8_t biome) const {
  static const std::vector<FaunaEntry> kEmpty;
  return biome < fauna_.size() ? fauna_[biome] : kEmpty;
}

std::vector<FaunaEntry> GeneratorPipeline::faunaAt(int32_t wx, int32_t wz) const {
  return faunaForBiome(biomeAt(wx, wz));
}

GeneratorPipeline::Builder::Builder(std::shared_ptr<const NoiseGraph> graph)
    : p_(std::make_unique<GeneratorPipeline>(Passkey{})) {
  p_->graph_ = std::move(graph);
}
GeneratorPipeline::Builder& GeneratorPipeline::Builder::climate(std::unique_ptr<IClimateStage> s) {
  p_->climate_ = std::move(s);
  return *this;
}
GeneratorPipeline::Builder& GeneratorPipeline::Builder::biomes(std::unique_ptr<IBiomeSelector> s) {
  p_->biomes_ = std::move(s);
  return *this;
}
GeneratorPipeline::Builder& GeneratorPipeline::Builder::terrain(std::unique_ptr<ITerrainStage> s) {
  p_->terrain_ = std::move(s);
  return *this;
}
GeneratorPipeline::Builder& GeneratorPipeline::Builder::biomePost(std::unique_ptr<IBiomePostStage> s) {
  p_->biomePost_.push_back(std::move(s));
  return *this;
}
GeneratorPipeline::Builder& GeneratorPipeline::Builder::fauna(std::vector<std::vector<FaunaEntry>> tables) {
  p_->fauna_ = std::move(tables);
  return *this;
}
GeneratorPipeline::Builder& GeneratorPipeline::Builder::surface(std::unique_ptr<ISurfaceRule> s) {
  p_->surface_.push_back(std::move(s));
  return *this;
}
GeneratorPipeline::Builder& GeneratorPipeline::Builder::carver(std::unique_ptr<ICarver> s) {
  p_->carvers_.push_back(std::move(s));
  return *this;
}
bool GeneratorPipeline::Builder::feature(std::string name, std::unique_ptr<IFeature> f) {
  return p_->features_.add(std::move(name), std::move(f));
}
std::unique_ptr<const GeneratorPipeline> GeneratorPipeline::Builder::build() {
  if (!p_ || !p_->terrain_ || !p_->graph_) return nullptr;
  // Let features bind pipeline services (StructureFeature's biome probe); every pipeline
  // built through the Builder gets this automatically, including test pipelines.
  // Let features and carvers bind pipeline services (StructureFeature's and RiverCarver's
  // biome probes); every pipeline built through the Builder gets this automatically,
  // including test pipelines.
  p_->features_.forEach([p = p_.get()](std::string_view, IFeature& f) { f.onPipelineBuilt(p); });
  for (auto& c : p_->carvers_) c->onPipelineBuilt(p_.get());
  for (auto& bp : p_->biomePost_) bp->onPipelineBuilt(p_.get());
  return std::move(p_);
}

}  // namespace litholattice::procgen
