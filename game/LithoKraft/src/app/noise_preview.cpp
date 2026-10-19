#include "noise_preview.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>
#include "litholattice/assets/image.h"
#include "litholattice/core/json.h"
#include "litholattice/core/log.h"
#include "litholattice/procgen/graph.h"
#include "litholattice/procgen/seed.h"

namespace lithokraft {

int runNoisePreview(const NoisePreviewOptions& o) {
  std::ifstream file(o.graphFile, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  Json json;
  std::string err;
  if (!file || !Json::parse(text.str(), json, &err)) {
    logError("noise preview: cannot read %s %s", o.graphFile.c_str(), err.c_str());
    return 1;
  }
  const Json* graphJson = json.get("graph") ? json.get("graph") : &json;  // generator file or bare graph
  procgen::GraphDesc desc;
  if (!procgen::GraphDesc::fromJson(*graphJson, desc, err)) {
    logError("noise preview: %s", err.c_str());
    return 1;
  }
  const auto graph = procgen::NoiseGraph::compile(desc, procgen::dimensionSeed(o.seed, "preview"), err);
  if (!graph) {
    logError("noise preview: %s", err.c_str());
    return 1;
  }
  std::string outputName = o.output;
  if (outputName.empty()) outputName = graph->outputIndex("height") >= 0 ? "height" : graph->outputNames().front();
  const int output = graph->outputIndex(outputName);
  if (output < 0) {
    logError("noise preview: graph has no output '%s'", outputName.c_str());
    return 1;
  }

  const int n = std::clamp(o.size, 1, 8192);
  std::vector<float> values(size_t(n) * size_t(n));
  const size_t width = size_t(n);
  std::vector<float> xs(width), ys(width, o.y), zs(width);
  procgen::GraphContext ctx;
  for (int row = 0; row < n; ++row) {  // one batch per image row, centred on the origin
    for (int col = 0; col < n; ++col) {
      xs[size_t(col)] = (float(col) - float(n) * 0.5f) * o.scale;
      zs[size_t(col)] = (float(row) - float(n) * 0.5f) * o.scale;
    }
    ctx.bind({xs, ys, zs, {}});
    graph->evaluate(output, ctx, std::span(values).subspan(size_t(row) * size_t(n), size_t(n)));
  }
  float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
  for (const float v : values)
    if (std::isfinite(v)) {
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  const float span = hi > lo ? hi - lo : 1.f;
  std::vector<uint8_t> rgba;
  rgba.reserve(values.size() * 4);
  for (const float v : values) {
    const uint8_t g = std::isfinite(v) ? uint8_t(std::clamp((v - lo) / span, 0.f, 1.f) * 255.f + 0.5f) : 0;
    rgba.insert(rgba.end(), {g, g, g, 255});
  }
  if (!Image(n, n, std::move(rgba)).savePng(o.outFile, &err)) {
    logError("noise preview: %s", err.c_str());
    return 1;
  }
  logInfo("noise preview: %s output '%s' %dx%d at %.3g blocks/px, seed %llu: min %.4g max %.4g -> %s", o.graphFile.c_str(),
          outputName.c_str(), n, n, double(o.scale), static_cast<unsigned long long>(o.seed), double(lo), double(hi),
          o.outFile.c_str());
  return 0;
}

}  // namespace lithokraft
