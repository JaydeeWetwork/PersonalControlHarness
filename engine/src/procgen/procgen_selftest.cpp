#include "litholattice/procgen/procgen_selftest.h"
#include "litholattice/procgen/diversity_selftest.h"
#include "litholattice/procgen/diversity_selftest.h"
#include "litholattice/procgen/litho_formula_voxelize_selftest.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include "litholattice/procgen/generator_loader.h"
#include "litholattice/procgen/modifiers.h"
#include "litholattice/procgen/stages.h"

namespace litholattice::procgen {
namespace {

class Checker {
 public:
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }

 private:
  int fails_ = 0;
};

float sampleDims(const INoise& n, int dims, float x, float y, float z, float w) {
  return dims == 2 ? n.sample2(x, y) : dims == 3 ? n.sample3(x, y, z) : n.sample4(x, y, z, w);
}

/// Range, finiteness and (for continuous noise) a bounded finite-difference gradient.
void checkNoise(Checker& check, const std::string& name, const INoise& n, bool continuous) {
  const NoiseRange r = n.range();
  for (int dims = 2; dims <= 4; ++dims) {
    Rng rng(uint64_t(dims) * 1234567u);
    bool inRange = true, finite = true, smooth = true;
    float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest(), maxStep = 0;
    for (int i = 0; i < 20000; ++i) {
      const float x = rng.range(-300.f, 300.f), y = rng.range(-300.f, 300.f), z = rng.range(-300.f, 300.f),
                  w = rng.range(-300.f, 300.f);
      const float v = sampleDims(n, dims, x, y, z, w);
      finite = finite && std::isfinite(v);
      inRange = inRange && v >= r.min - 1e-4f && v <= r.max + 1e-4f;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
      if (continuous && i % 4 == 0) {
        const float step = std::abs(sampleDims(n, dims, x + 1e-3f, y + 1e-3f, z, w) - v);
        maxStep = std::max(maxStep, step);
        smooth = smooth && step < 0.03f;  // |gradient| < ~20 per unit
      }
    }
    // Uses a reasonable share of its range (catches a dead or badly scaled noise). Cellular
    // ranges are upper bounds over every dimension count, so cap the requirement.
    const bool spread = hi - lo > std::min(0.5f, 0.5f * (r.max - r.min));
    char buf[192];
    std::snprintf(buf, sizeof buf, "%s %dD: finite, in [%.2f, %.2f] (saw %.3f..%.3f)", name.c_str(), dims, double(r.min),
                  double(r.max), double(lo), double(hi));
    std::string what = buf;
    if (continuous) {
      std::snprintf(buf, sizeof buf, ", continuous (max step %.4f per 0.001)", double(maxStep));
      what += buf;
    }
    check(finite && inRange && spread && smooth, what);
  }
}

void noiseChecks(Checker& check) {
  const struct {
    NoiseType type;
    bool continuous;
  } types[] = {{NoiseType::Perlin, true}, {NoiseType::OpenSimplex2F, true}, {NoiseType::OpenSimplex2S, true},
               {NoiseType::Value, true},  {NoiseType::Cellular, true},      {NoiseType::White, false}};
  for (const auto& t : types) {
    NoiseSettings s;
    s.type = t.type;
    s.seed = 42;
    checkNoise(check, std::string(toString(t.type)), *createNoise(s), t.continuous);
  }
  for (const auto ret : {CellularReturn::F2, CellularReturn::F2MinusF1})
    for (const auto metric : {DistanceMetric::Manhattan, DistanceMetric::Chebyshev}) {
      NoiseSettings s;
      s.type = NoiseType::Cellular;
      s.cellularReturn = ret;
      s.metric = metric;
      checkNoise(check, "cellular " + std::string(toString(ret)) + " " + std::string(toString(metric)), *createNoise(s), true);
    }
  NoiseSettings base;
  base.seed = 7;
  const std::shared_ptr<const INoise> src = createNoise(base);
  for (const auto type : {FractalType::FBm, FractalType::Ridged, FractalType::Billow, FractalType::Turbulence,
                          FractalType::PingPong}) {
    FractalSettings f;
    f.type = type;
    checkNoise(check, "fractal " + std::string(toString(type)), *createFractal(src, f, 9), true);
  }
  checkNoise(check, "domain warp", *createDomainWarp(src, createNoise({NoiseType::Value, 3}), 0.5f, 0.5f), true);
  check(parseFractalType("pingpong") == FractalType::PingPong && parseFractalType("ping_pong") == FractalType::PingPong &&
            std::string(toString(FractalType::PingPong)) == "pingpong",
        "pingpong: name parses (pingpong / ping_pong) and round-trips");

  // Ping-pong differs from ridged (its single-fold sibling) on the same source/seed.
  {
    FractalSettings fr;
    fr.type = FractalType::Ridged;
    FractalSettings fp;
    fp.type = FractalType::PingPong;
    const auto ridged = createFractal(src, fr, 9), pingpong = createFractal(src, fp, 9);
    bool differs = false, inRange = true;
    for (int i = 0; i < 256; ++i) {
      const float v = pingpong->sample3(float(i) * 0.37f, 1.5f, float(i) * -0.91f);
      differs = differs || v != ridged->sample3(float(i) * 0.37f, 1.5f, float(i) * -0.91f);
      inRange = inRange && v >= 0.f && v <= 1.f;
    }
    check(differs && inRange, "pingpong: distinct from ridged, output in [0, 1]");
    const float golden = pingpong->sample3(0.37f, 1.5f, -0.91f);
    std::printf("    pingpong golden sample3(0.37, 1.5, -0.91) = %.9g\n", double(golden));
    check(std::abs(golden - 0.58929199f) < 1e-6f,
          "pingpong: golden value pinned (implementation-defined, from the 2026-10-13 first run)");
  }

  // ---- curl noise (2026-10-13): deterministic, finite, (approximately) divergence-free
  {
    NoiseSettings ps;
    ps.type = NoiseType::OpenSimplex2S;
    ps.seed = 5;
    const auto pot = createNoise(ps);
    const CurlVec2 a = curlNoise2(*pot, 1.25f, -3.5f);
    const CurlVec2 b = curlNoise2(*pot, 1.25f, -3.5f);
    check(a.x == b.x && a.y == b.y, "curl2: deterministic (same point -> same vector)");
    std::printf("    curl2 golden at (1.25, -3.5) = (%.9g, %.9g)\n", double(a.x), double(a.y));
    NoiseSettings ps2 = ps;
    ps2.seed = 6;
    const auto pot2 = createNoise(ps2);
    const CurlVec2 c = curlNoise2(*pot2, 1.25f, -3.5f);
    check(c.x != a.x || c.y != a.y, "curl2: different seed -> different field");
    // Divergence of the curl field: mathematically zero. Measured with the SAME
    // central-difference stencil the construction uses (h == eps), the mixed
    // partials cancel term by term, so the residual is float rounding only.
    bool finite = true;
    double maxDiv = 0, maxMag = 0;
    const float h = 0.01f;  // == the curlNoise default eps
    for (int i = 0; i < 40; ++i)
      for (int j = 0; j < 40; ++j) {
        const float x = -4.f + float(i) * 0.2f, y = -4.f + float(j) * 0.2f;
        const CurlVec2 v = curlNoise2(*pot, x, y);
        finite = finite && std::isfinite(v.x) && std::isfinite(v.y);
        maxMag = std::max(maxMag, double(std::abs(v.x) + std::abs(v.y)));
        const float dvx = (curlNoise2(*pot, x + h, y).x - curlNoise2(*pot, x - h, y).x) / (2.f * h);
        const float dvy = (curlNoise2(*pot, x, y + h).y - curlNoise2(*pot, x, y - h).y) / (2.f * h);
        maxDiv = std::max(maxDiv, double(std::abs(dvx + dvy)));
      }
    std::printf("    curl2: max |div| %.4g, max |v| %.4g over the sample grid\n", maxDiv, maxMag);
    check(finite && maxMag > 0.05, "curl2: finite and non-degenerate over a sample grid");
    check(maxDiv < 0.02, "curl2: divergence-free (same-stencil discrete identity, float residual only)");
    check(std::abs(a.x - (-1.98410475f)) < 1e-5f && std::abs(a.y - 0.735200942f) < 1e-5f,
          "curl2: golden value pinned (implementation-defined, from the 2026-10-13 first run)");
    // 3D
    NoiseSettings q1;
    q1.type = NoiseType::OpenSimplex2S;
    q1.seed = 11;
    NoiseSettings q2 = q1;
    q2.seed = 12;
    NoiseSettings q3 = q1;
    q3.seed = 13;
    const auto pX = createNoise(q1), pY = createNoise(q2), pZ = createNoise(q3);
    const CurlVec3 v1 = curlNoise3(*pX, *pY, *pZ, 0.5f, -1.25f, 2.75f);
    const CurlVec3 v2 = curlNoise3(*pX, *pY, *pZ, 0.5f, -1.25f, 2.75f);
    check(v1.x == v2.x && v1.y == v2.y && v1.z == v2.z, "curl3: deterministic");
    std::printf("    curl3 golden at (0.5, -1.25, 2.75) = (%.9g, %.9g, %.9g)\n", double(v1.x), double(v1.y),
                double(v1.z));
    bool finite3 = std::isfinite(v1.x) && std::isfinite(v1.y) && std::isfinite(v1.z);
    double maxDiv3 = 0, maxMag3 = 0;
    for (int i = 0; i < 12; ++i)
      for (int j = 0; j < 12; ++j)
        for (int k = 0; k < 12; ++k) {
          const float x = -2.f + float(i) * 0.35f, y = -2.f + float(j) * 0.35f, z = -2.f + float(k) * 0.35f;
          const CurlVec3 v = curlNoise3(*pX, *pY, *pZ, x, y, z);
          finite3 = finite3 && std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
          maxMag3 = std::max(maxMag3, double(std::abs(v.x) + std::abs(v.y) + std::abs(v.z)));
          const float dvx = (curlNoise3(*pX, *pY, *pZ, x + h, y, z).x - curlNoise3(*pX, *pY, *pZ, x - h, y, z).x) / (2.f * h);
          const float dvy = (curlNoise3(*pX, *pY, *pZ, x, y + h, z).y - curlNoise3(*pX, *pY, *pZ, x, y - h, z).y) / (2.f * h);
          const float dvz = (curlNoise3(*pX, *pY, *pZ, x, y, z + h).z - curlNoise3(*pX, *pY, *pZ, x, y, z - h).z) / (2.f * h);
          maxDiv3 = std::max(maxDiv3, double(std::abs(dvx + dvy + dvz)));
        }
    std::printf("    curl3: max |div| %.4g, max |v| %.4g over the sample grid\n", maxDiv3, maxMag3);
    check(finite3 && maxMag3 > 0.05, "curl3: finite and non-degenerate over a sample grid");
    check(maxDiv3 < 0.05, "curl3: divergence-free (same-stencil discrete identity, float residual only)");
    check(std::abs(v1.x - (-0.521783113f)) < 1e-5f && std::abs(v1.y - 0.101734757f) < 1e-5f &&
              std::abs(v1.z - (-3.30431461f)) < 1e-4f,
          "curl3: golden value pinned (implementation-defined, from the 2026-10-13 first run)");
  }

  // ---- cellular cell identity (2026-10-13)
  {
    NoiseSettings cs;
    cs.type = NoiseType::Cellular;
    cs.seed = 42;
    const auto cell = createNoise(cs);
    const auto cellSame = createNoise(cs);
    NoiseSettings csOther = cs;
    csOther.seed = 43;
    const auto cellOther = createNoise(csOther);
    check(cell->cellId3(0.2f, 0.3f, 0.1f) != 0, "cellular id: non-zero for cellular noise");
    check(cell->cellId3(0.2f, 0.3f, 0.1f) == cellSame->cellId3(0.2f, 0.3f, 0.1f),
          "cellular id: same seed + position -> same id");
    check(cell->cellId2(0.2f, 0.3f) == cellSame->cellId2(0.2f, 0.3f), "cellular id: 2D form deterministic");
    // The id changes across cell borders: a scan across several cells sees several ids.
    std::set<uint64_t> ids;
    for (int i = 0; i <= 40; ++i) ids.insert(cell->cellId3(-5.f + float(i) * 0.25f, 0.3f, 0.1f));
    check(ids.size() >= 3, "cellular id: scan across cells sees multiple ids (" + std::to_string(ids.size()) + ")");
    // Deep inside a cell the id is locally constant (all small offsets agree).
    bool foundInterior = false;
    for (int i = 0; i <= 40 && !foundInterior; ++i) {
      const float x = -5.f + float(i) * 0.25f;
      const uint64_t id = cell->cellId3(x, 0.3f, 0.1f);
      bool all = true;
      for (const float d : {-0.03f, 0.03f}) {
        all = all && cell->cellId3(x + d, 0.3f, 0.1f) == id && cell->cellId3(x, 0.3f + d, 0.1f) == id &&
              cell->cellId3(x, 0.3f, 0.1f + d) == id;
      }
      foundInterior = all;
    }
    check(foundInterior, "cellular id: locally constant inside a cell");
    check(cell->cellId3(0.2f, 0.3f, 0.1f) != cellOther->cellId3(0.2f, 0.3f, 0.1f),
          "cellular id: different seed -> different id at the same position");
    NoiseSettings vs;
    vs.type = NoiseType::Value;
    vs.seed = 42;
    check(createNoise(vs)->cellId3(0.2f, 0.3f, 0.1f) == 0, "cellular id: non-cellular noise returns 0");
    std::printf("    cellular id golden at (0.2, 0.3, 0.1) seed 42 = %llx\n",
                (unsigned long long)cell->cellId3(0.2f, 0.3f, 0.1f));
    check(cell->cellId3(0.2f, 0.3f, 0.1f) == 0x57e1fabaull,
          "cellular id: golden value pinned (implementation-defined, from the 2026-10-13 first run)");
  }

  // Seeding: same seed reproduces, a different seed changes the field.
  NoiseSettings other = base;
  other.seed = 8;
  const auto a = createNoise(base), b = createNoise(base), c = createNoise(other);
  bool same = true, differs = false;
  for (int i = 0; i < 64; ++i) {
    const float x = float(i) * 0.37f, z = float(i) * -0.91f;
    same = same && a->sample3(x, 1.5f, z) == b->sample3(x, 1.5f, z);
    differs = differs || a->sample3(x, 1.5f, z) != c->sample3(x, 1.5f, z);
  }
  check(same && differs, "noise: same seed reproduces, different seed differs");

  // Batch sampling equals scalar sampling.
  std::vector<float> xs(100), ys(100), zs(100), out(100);
  for (size_t i = 0; i < xs.size(); ++i) {
    xs[i] = float(i) * 0.31f;
    ys[i] = float(i) * 0.17f;
    zs[i] = float(i) * -0.23f;
  }
  a->sampleBatch(3, {xs, ys, zs, {}}, out);
  bool batch = true;
  for (size_t i = 0; i < xs.size(); ++i) batch = batch && out[i] == a->sample3(xs[i], ys[i], zs[i]);
  check(batch, "noise: sampleBatch matches scalar sampling");
}

constexpr const char* kGraphText = R"({
  "nodes": {
    "base": {"type": "noise", "noise": "perlin", "frequency": 0.02, "fractal": {"type": "fbm", "octaves": 4},
             "warp": {"noise": "opensimplex2f", "amplitude": 0.4, "frequency": 0.5}},
    "cells": {"type": "noise", "noise": "cellular", "dims": 3, "frequency": [0.05, 0.1, 0.05],
              "cellular": {"return": "f2-f1", "metric": "manhattan"}},
    "shaped": {"type": "curve", "input": "base", "interpolation": "cubic", "points": [[-1, -1], [0, 0.2], [1, 1]]},
    "mix": {"type": "blend", "a": "shaped", "b": {"type": "terrace", "input": "base", "step": 0.25, "smoothness": 0.3},
            "t": {"type": "clamp", "input": "cells", "min": 0, "max": 1}},
    "pick": {"type": "select", "control": {"type": "coordinate", "axis": "y"}, "a": "mix", "b": -1, "threshold": 64, "falloff": 4},
    "height": {"type": "scale_bias", "input": {"type": "cache", "input": "pick"}, "scale": 30, "bias": 70},
    "ripple": {"type": "add", "inputs": [{"type": "wave", "direction": [1, 0, 1], "frequency": 0.1, "amplitude": 2},
                                         {"type": "min", "inputs": ["base", 0.5]}, {"type": "max", "inputs": ["cells", 0]},
                                         {"type": "multiply", "inputs": ["base", "cells"]}, {"type": "constant", "value": 1}]}
  },
  "outputs": {"height": "height", "ripple": "ripple", "flat": 3}
})";

std::vector<float> evalGrid(const NoiseGraph& g, int output) {
  std::vector<float> xs, ys, zs;
  for (int i = 0; i < 256; ++i) {
    xs.push_back(float(i % 16) * 7.f - 50.f);
    ys.push_back(float(i / 4));
    zs.push_back(float(i / 16) * 5.f);
  }
  GraphContext ctx;
  ctx.bind({xs, ys, zs, {}});
  std::vector<float> out(xs.size());
  g.evaluate(output, ctx, out);
  return out;
}

void graphChecks(Checker& check) {
  GraphDesc d1, d2;
  std::string err;
  const bool parsed = GraphDesc::parse(kGraphText, d1, err);
  check(parsed, "graph: parses every node type (" + (parsed ? std::to_string(d1.nodes.size()) + " nodes" : err) + ")");
  if (!parsed) return;
  const std::string text1 = d1.toJson().dump();
  const bool reparsed = GraphDesc::parse(text1, d2, err);
  check(reparsed && d2.toJson().dump() == text1, "graph: JSON round trip is lossless (dump -> parse -> dump)");
  const auto g1 = NoiseGraph::compile(d1, 99, err), g2 = NoiseGraph::compile(d2, 99, err);
  check(g1 && g2, "graph: compiles" + (err.empty() ? std::string() : " (" + err + ")"));
  if (!g1 || !g2) return;
  bool equal = true, finite = true;
  for (const std::string& out : g1->outputNames()) {
    const auto a = evalGrid(*g1, g1->outputIndex(out)), b = evalGrid(*g2, g2->outputIndex(out));
    equal = equal && a == b;
    for (const float v : a) finite = finite && std::isfinite(v);
  }
  check(equal && finite, "graph: round-tripped graph evaluates identically, no NaN");
  const auto flat = evalGrid(*g1, g1->outputIndex("flat"));
  check(flat.front() == 3.f && flat.back() == 3.f, "graph: numeric output becomes a constant node");
  const int h = g1->outputIndex("height");
  check(g1->evaluateAt(h, 10.f, 200.f, 10.f) == 40.f, "graph: select above threshold + falloff picks b (-1 * 30 + 70)");
  GraphContext ctx;
  std::vector<float> xs{1, 2}, ys{0, 0}, zs{3, 4}, first(2), second(2);
  ctx.bind({xs, ys, zs, {}});
  g1->evaluate(h, ctx, first);
  ctx.bind({xs, ys, zs, {}});  // same points: the cache node serves the stored values
  g1->evaluate(h, ctx, second);
  check(first == second && first[0] == g1->evaluateAt(h, 1, 0, 3), "graph: cache node and batch match scalar evaluation");

  const struct {
    const char* text;
    const char* what;
  } bad[] = {
      {R"({"nodes": {"a": {"type": "sparkle"}}, "outputs": {"o": "a"}})", "unknown node type"},
      {R"({"nodes": {"a": {"type": "clamp"}}, "outputs": {"o": "a"}})", "missing input"},
      {R"({"nodes": {"a": {"type": "constant", "vaule": 1}}, "outputs": {"o": "a"}})", "unknown parameter"},
      {R"({"nodes": {"a": {"type": "clamp", "input": "nope"}}, "outputs": {"o": "a"}})", "unknown input node"},
      {R"({"nodes": {"a": {"type": "constant"}}, "outputs": {"o": "b"}})", "unknown output node"},
  };
  for (const auto& b : bad) {
    GraphDesc d;
    std::string e;
    const bool rejected = !GraphDesc::parse(b.text, d, e);
    check(rejected && !e.empty(), std::string("graph: rejects ") + b.what + " (" + e + ")");
  }
  GraphDesc cyclic;
  const bool cycleParsed = GraphDesc::parse(
      R"({"nodes": {"a": {"type": "clamp", "input": "b"}, "b": {"type": "scale_bias", "input": "a"}}, "outputs": {"o": "a"}})",
      cyclic, err);
  const bool cycleRejected = cycleParsed && !NoiseGraph::compile(cyclic, 0, err);
  check(cycleRejected && err.find("cycle") != std::string::npos, "graph: compile rejects cycles (" + err + ")");
}

/// Standalone operator nodes (2026-10-13): abs / invert / power as one-input nodes,
/// fractal / warp as leaf nodes (previously only parameter blocks inside `noise`).
void graphNewNodeChecks(Checker& check) {
  std::string err;
  const auto compile = [&](const char* text) {
    GraphDesc d;
    if (!GraphDesc::parse(text, d, err)) return std::shared_ptr<const NoiseGraph>{};
    return NoiseGraph::compile(d, 99, err);
  };
  const std::string baseText =
      R"({"nodes": {"n": {"type": "noise", "noise": "perlin", "seed": 7, "frequency": 0.05}}, "outputs": {"o": "n"}})";
  const auto base = compile(baseText.c_str());
  check(base != nullptr, "graph nodes: base noise graph compiles");
  if (!base) return;
  const auto baseVals = evalGrid(*base, 0);

  const auto unary = [&](const char* nodeJson, const char* what, const std::function<float(float)>& expect) {
    const std::string text = std::string(
                                 R"({"nodes": {"n": {"type": "noise", "noise": "perlin", "seed": 7, "frequency": 0.05}, "a": )") +
                             nodeJson + R"(}, "outputs": {"o": "a"}})";
    const auto g = compile(text.c_str());
    check(g != nullptr, std::string("graph nodes: ") + what + " compiles" + (g ? "" : " (" + err + ")"));
    if (!g) return;
    const auto vals = evalGrid(*g, 0);
    bool ok = vals.size() == baseVals.size();
    for (size_t i = 0; i < vals.size() && ok; ++i) ok = vals[i] == expect(baseVals[i]);
    check(ok, std::string("graph nodes: ") + what + " matches its definition on every grid point");
  };
  unary(R"({"type": "abs", "input": "n"})", "abs node", [](float v) { return std::abs(v); });
  unary(R"({"type": "invert", "input": "n"})", "invert node", [](float v) { return -v; });
  unary(R"({"type": "power", "input": "n", "exponent": 2})", "power node (sign-preserving square)",
        [](float v) { return (v < 0 ? -1.f : 1.f) * std::pow(std::abs(v), 2.f); });
  unary(R"({"type": "power", "input": "n", "exponent": 0.5})", "power node (sign-preserving sqrt)",
        [](float v) { return (v < 0 ? -1.f : 1.f) * std::pow(std::abs(v), 0.5f); });

  // fractal leaf node: deterministic, finite, and different from the plain noise.
  {
    const auto g = compile(
        R"({"nodes": {"f": {"type": "fractal", "noise": "perlin", "seed": 7, "frequency": 0.05, "fractal": {"type": "ridged", "octaves": 3}}}, "outputs": {"o": "f"}})");
    check(g != nullptr, "graph nodes: standalone fractal node compiles" + (g ? std::string() : " (" + err + ")"));
    if (g) {
      const auto a = evalGrid(*g, 0), b = evalGrid(*g, 0);
      bool finite = true, differs = false;
      for (size_t i = 0; i < a.size(); ++i) {
        finite = finite && std::isfinite(a[i]);
        differs = differs || a[i] != baseVals[i];
      }
      check(a == b && finite && differs, "graph nodes: fractal node deterministic, finite, differs from plain noise");
    }
    const auto bad = compile(
        R"({"nodes": {"f": {"type": "fractal", "noise": "perlin", "seed": 7}}, "outputs": {"o": "f"}})");
    check(bad == nullptr, "graph nodes: fractal node without a fractal block is rejected (" + err + ")");
  }
  // warp leaf node: same contract.
  {
    const auto g = compile(
        R"({"nodes": {"w": {"type": "warp", "noise": "perlin", "seed": 7, "frequency": 0.05, "warp": {"noise": "opensimplex2s", "amplitude": 1.5, "frequency": 0.5}}}, "outputs": {"o": "w"}})");
    check(g != nullptr, "graph nodes: standalone warp node compiles" + (g ? std::string() : " (" + err + ")"));
    if (g) {
      const auto a = evalGrid(*g, 0), b = evalGrid(*g, 0);
      bool finite = true, differs = false;
      for (size_t i = 0; i < a.size(); ++i) {
        finite = finite && std::isfinite(a[i]);
        differs = differs || a[i] != baseVals[i];
      }
      check(a == b && finite && differs, "graph nodes: warp node deterministic, finite, differs from unwarped noise");
    }
    const auto bad = compile(
        R"({"nodes": {"w": {"type": "warp", "noise": "perlin", "seed": 7}}, "outputs": {"o": "w"}})");
    check(bad == nullptr, "graph nodes: warp node without a warp block is rejected (" + err + ")");
  }
}

void biomeChecks(Checker& check) {
  BiomePoint desert{"desert", {0.9f, 0.1f, 0.5f, 0.5f}, {1, 1, 0, 0}};
  BiomePoint jungle{"jungle", {0.9f, 0.9f, 0.5f, 0.5f}, {1, 1, 0, 0}};
  BiomePoint tundra{"tundra", {0.1f, 0.5f, 0.5f, 0.5f}, {1, 0, 0, 0}};
  const MultiNoiseBiomeSelector sel({desert, jungle, tundra});
  check(sel.select({0.85f, 0.2f, 0.f, 1.f}) == 0 && sel.select({0.8f, 0.8f, 1.f, 0.f}) == 1 &&
            sel.select({0.05f, 0.95f, 0.5f, 0.5f}) == 2,
        "biomes: multi-noise picks the nearest climate point (weights ignore unused parameters)");
  const MultiNoiseBiomeSelector tie({{"a", {0.5f, 0.5f, 0.25f, 0.5f}, {0, 0, 1, 0}}, {"b", {0.5f, 0.5f, 0.75f, 0.5f}, {0, 0, 1, 0}}});
  check(tie.select({0, 0, 0.5f, 0}) == 0 && tie.select({0, 0, 0.51f, 0}) == 1, "biomes: ties go to the earlier biome");
}

constexpr const char* kGeneratorText = R"({
  "graph": {"nodes": {
      "c": {"type": "scale_bias", "scale": 0.5, "bias": 0.5, "input": {"type": "noise", "noise": "opensimplex2s", "frequency": 0.01}},
      "h": {"type": "add", "inputs": [60, {"type": "noise", "noise": "perlin", "frequency": 0.03, "fractal": {"type": "ridged", "octaves": 3}}]}},
    "outputs": {"height": "h", "continentalness": "c"}},
  "terrain": {"type": "heightmap", "output": "height", "material": "rock"},
  "climate": {"continentalness": "continentalness"},
  "biomes": [{"name": "low", "climate": {"continentalness": 0.2}, "surface": {"top": "turf", "filler": "soil", "depth": 2}},
             {"name": "high", "climate": {"continentalness": 0.8}, "surface": {"top": "soil", "depth": 0}}],
  "surface": {"top_state": "biome"},
  "carvers": [{"type": "worm_cave", "chance": 1, "min_y": 20, "max_y": 40}, {"type": "ravine"}],
  "features": [{"name": "ore", "type": "ore_vein", "material": "ore", "replace": "rock", "count": 10, "size": 8, "min_y": 2, "max_y": 40},
               {"name": "dungeon", "type": "dungeon"}]
})";

std::optional<MaterialId> testMaterial(std::string_view name) {
  if (name == "rock") return MaterialId{1};
  if (name == "turf") return MaterialId{2};
  if (name == "soil") return MaterialId{3};
  if (name == "ore") return MaterialId{4};
  if (name == "trace") return MaterialId{5};
  if (name == "via") return MaterialId{6};
  if (name == "trunk") return MaterialId{10};
  if (name == "branch") return MaterialId{11};
  if (name == "tipred") return MaterialId{12};
  if (name == "tipblue") return MaterialId{13};
  if (name == "can") return MaterialId{14};
  if (name == "crystal") return MaterialId{15};
  if (name == "water") return MaterialId{7};
  if (name == "gravel") return MaterialId{8};
  return std::nullopt;
}

uint64_t hashBuffer(const ColumnBuffer& b) {
  uint64_t h = 0;
  for (int y = 0; y < b.height(); ++y)
    for (int z = 0; z < b.size(); ++z)
      for (int x = 0; x < b.size(); ++x) h = mix64(h ^ (uint64_t(b.get(x, y, z)) << 8 | b.state(x, y, z)));
  return h;
}

constexpr const char* kRuinsGeneratorText = R"({
  "graph": {"nodes": {
      "c": {"type": "scale_bias", "scale": 0.5, "bias": 0.5, "input": {"type": "noise", "noise": "opensimplex2s", "frequency": 0.01}},
      "h": {"type": "add", "inputs": [60, {"type": "noise", "noise": "perlin", "frequency": 0.03}]}},
    "outputs": {"height": "h", "continentalness": "c"}},
  "terrain": {"type": "heightmap", "output": "height", "material": "rock"},
  "climate": {"continentalness": "continentalness"},
  "biomes": [{"name": "low", "climate": {"continentalness": 0.2}, "surface": {"top": "turf", "filler": "soil", "depth": 2}},
             {"name": "high", "climate": {"continentalness": 0.8}, "surface": {"top": "soil", "depth": 0}}],
  "carvers": [{"type": "ruins", "trace": "trace", "via": "via", "crust": ["rock", "soil"],
               "delam": ["soil", "rock"], "density": {"low": 2.0, "high": 0.0}}]
})";

/// Largest 6-connected air region fully below the surface margin, per column surface.
int largestBuriedAirRegion(const ColumnBuffer& b, const ColumnMaps& maps, int margin, int& totalAir) {
  const int n = b.size(), h = b.height();
  std::vector<char> seen(size_t(n) * n * h, 0);
  const auto at = [&](int x, int y, int z) { return (size_t(y) * size_t(n) + size_t(z)) * size_t(n) + size_t(x); };
  int largest = 0;
  totalAir = 0;
  for (int z = 0; z < n; ++z)
    for (int x = 0; x < n; ++x) {
      const int ceiling = maps.height[size_t(z * n + x)] - margin;
      for (int y = 1; y <= ceiling; ++y) {
        if (b.get(x, y, z) != kAirMaterial || seen[at(x, y, z)]) continue;
        int region = 0;
        std::vector<std::array<int, 3>> stack{{x, y, z}};
        seen[at(x, y, z)] = 1;
        while (!stack.empty()) {
          const auto p = stack.back();
          stack.pop_back();
          ++region;
          constexpr int kD[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
          for (const auto& d : kD) {
            const int qx = p[0] + d[0], qy = p[1] + d[1], qz = p[2] + d[2];
            if (qx < 0 || qx >= n || qz < 0 || qz >= n || qy < 1 || qy > ceiling) continue;
            if (b.get(qx, qy, qz) != kAirMaterial || seen[at(qx, qy, qz)]) continue;
            seen[at(qx, qy, qz)] = 1;
            stack.push_back({qx, qy, qz});
          }
        }
        largest = std::max(largest, region);
        totalAir += region;
      }
    }
  return largest;
}

void ruinsChecks(Checker& check) {
  std::string err;
  const auto gen = loadGenerator(std::string(kRuinsGeneratorText), dimensionSeed(7, "wafer"), testMaterial, err);
  check(gen != nullptr, "ruins: generator JSON with ruins carver loads" + (err.empty() ? std::string() : " (" + err + ")"));
  if (!gen) return;
  std::string badErr;
  const bool badBiome = !loadGenerator(std::string(R"({"graph": {"nodes": {"h": {"type": "constant"}}, "outputs": {"height": "h"}},"
                                                  R"( "terrain": {"output": "height", "material": "rock"},"
                                                  R"( "biomes": [{"name": "low"}],"
                                                  R"( "carvers": [{"type": "ruins", "trace": "trace", "via": "via", "crust": ["rock"],"
                                                  R"( "density": {"nope": 2}}]})"),
                                       dimensionSeed(7, "wafer"), testMaterial, badErr);
  check(badBiome, "ruins: unknown biome in density rejected (" + badErr + ")");
  std::string crustErr;
  const bool noCrust = !loadGenerator(std::string(R"({"graph": {"nodes": {"h": {"type": "constant"}}, "outputs": {"height": "h"}},"
                                                  R"( "terrain": {"output": "height", "material": "rock"},"
                                                  R"( "carvers": [{"type": "ruins", "trace": "trace", "via": "via"}]})"),
                                        dimensionSeed(7, "wafer"), testMaterial, crustErr);
  check(noCrust, "ruins: missing crust list rejected (" + crustErr + ")");

  const auto info = [](int cx, int cz) {
    ChunkInfo c;
    c.cx = cx;
    c.cz = cz;
    c.size = 16;
    c.height = 128;
    c.dimensionSeed = dimensionSeed(7, "wafer");
    c.chunkSeed = chunkSeed(c.dimensionSeed, cx, cz);
    return c;
  };
  // Determinism: same chunk twice -> identical; different seed -> different.
  const ColumnBuffer a = gen->generate(info(3, -5)), b2 = gen->generate(info(3, -5));
  ChunkInfo other = info(3, -5);
  other.chunkSeed = chunkSeed(dimensionSeed(8, "wafer"), 3, -5);
  const ColumnBuffer c2 = gen->generate(other);
  check(hashBuffer(a) == hashBuffer(b2) && hashBuffer(a) != hashBuffer(c2),
        "ruins: chunk-local determinism (same seed identical, other seed differs)");

  // Structural + placement properties over a sample of chunks.
  int traceBlocks = 0, viaBlocks = 0, buriedViolations = 0, diagonalTouch = 0;
  int traceLow = 0, traceHigh = 0, viaHigh = 0;
  int largestAir = 0, totalAir = 0;
  ColumnMaps maps;
  for (int cz = -2; cz < 2; ++cz)
    for (int cx = -2; cx < 2; ++cx) {
      const ColumnBuffer buf = gen->generate(info(cx, cz), &maps);
      int chunkAir = 0;
      largestAir = std::max(largestAir, largestBuriedAirRegion(buf, maps, 6, chunkAir));
      totalAir += chunkAir;
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
          const size_t i = size_t(z * 16 + x);
          const int surf = maps.height[i];
          for (int y = 0; y <= surf - 6; ++y) {
            const MaterialId m = buf.get(x, y, z);
            if (m != 5 && m != 6) continue;
            if (m == 5) {
              ++traceBlocks;
              // Orthogonal routing: no diagonal trace-to-trace contact on the same level.
              constexpr int kDiag[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
              for (const auto& d : kDiag) {
                const int qx = x + d[0], qz = z + d[1];
                if (qx >= 0 && qx < 16 && qz >= 0 && qz < 16 && buf.get(qx, y, qz) == 5) ++diagonalTouch;
              }
            } else {
              ++viaBlocks;
            }
            if (y > surf - 6 || y < 2) ++buriedViolations;
            if (maps.biome[i] == 1) {
              if (m == 5) ++traceHigh; else ++viaHigh;
            } else if (m == 5) {
              ++traceLow;
            }
          }
          // Surface breach sweep: no trace/via anywhere in the top margin either.
          for (int y = std::max(0, surf - 5); y <= surf; ++y) {
            const MaterialId m = buf.get(x, y, z);
            if (m == 5 || m == 6) ++buriedViolations;
          }
        }
    }
  check(traceBlocks > 50 && viaBlocks > 10,
        "ruins: traces (" + std::to_string(traceBlocks) + ") and via stubs (" + std::to_string(viaBlocks) + ") placed");
  check(buriedViolations == 0, "ruins: nothing reaches the surface margin (0 violations)");
  check(diagonalTouch == 0, "ruins: traces route orthogonally (0 diagonal trace contacts)");
  check(viaHigh == 0 && traceHigh * 8 < traceLow,
        "ruins: density follows the biome (zero-density biome ~ruin-free)");
  check(largestAir <= 8 && totalAir > 0,
        "ruins: no large air pockets (largest " + std::to_string(largestAir) + ", total " + std::to_string(totalAir) + ")");
}

void pipelineChecks(Checker& check, JobPool& pool) {
  std::string err;
  const auto gen = loadGenerator(std::string(kGeneratorText), dimensionSeed(1, "test"), testMaterial, err);
  check(gen != nullptr, "pipeline: generator JSON loads" + (err.empty() ? std::string() : " (" + err + ")"));
  if (!gen) return;
  std::string badErr;
  const bool badRejected = !loadGenerator(std::string(R"({"graph": {"nodes": {"h": {"type": "constant"}}, "outputs": {"height": "h"}},
                                                         "terrain": {"output": "height", "material": "lava"}})"),
                                         0, testMaterial, badErr);
  check(badRejected, "pipeline: unknown material rejected (" + badErr + ")");

  constexpr int kSide = 4;
  const auto info = [](int cx, int cz) {
    ChunkInfo c;
    c.cx = cx;
    c.cz = cz;
    c.size = 16;
    c.height = 128;
    c.dimensionSeed = dimensionSeed(1, "test");
    c.chunkSeed = chunkSeed(c.dimensionSeed, cx, cz);
    return c;
  };
  std::vector<uint64_t> serial(kSide * kSide), parallel(kSide * kSide);
  for (int i = 0; i < kSide * kSide; ++i) serial[size_t(i)] = hashBuffer(gen->generate(info(i % kSide - 2, i / kSide - 2)));
  std::atomic<int> done{0};
  for (int i = kSide * kSide - 1; i >= 0; --i)  // reverse order, on the worker threads
    pool.submit([&, i] {
      parallel[size_t(i)] = hashBuffer(gen->generate(info(i % kSide - 2, i / kSide - 2)));
      done.fetch_add(1);
    });
  pool.waitIdle();
  check(done.load() == kSide * kSide && serial == parallel,
        "pipeline: chunks identical serial vs " + std::to_string(pool.size()) + " JobPool threads");

  // Stage effects on one chunk: surface per biome, state = biome, caves below the surface, ore.
  ColumnMaps maps;
  int caves = 0, ore = 0, surfaceOk = 0, ravineCut = 0;
  bool stateOk = true;
  for (int cz = 0; cz < 3; ++cz)
    for (int cx = 0; cx < 3; ++cx) {
      const ColumnBuffer b = gen->generate(info(cx, cz), &maps);
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
          const size_t i = size_t(z * 16 + x);
          const int h = maps.height[i];
          const MaterialId top = b.get(x, h, z);
          // Ravine-carved columns read air at the recorded surface: the gash is real now.
          surfaceOk += top == kAirMaterial ||
                       (maps.biome[i] == 0 && top == 2 && b.get(x, h - 1, z) == 3) ||
                       (maps.biome[i] == 1 && top == 3);
          ravineCut += (top == kAirMaterial) ? 1 : 0;
          stateOk = stateOk && b.state(x, h, z) == maps.biome[i];
          for (int y = 1; y < h - 6; ++y) {
            caves += b.get(x, y, z) == kAirMaterial;
            ore += b.get(x, y, z) == 4;
          }
        }
    }
  check(surfaceOk == 9 * 256 && stateOk, "pipeline: surface layers and top state follow the biome");
  check(caves > 0 && ore > 0, "pipeline: worm caves carve underground (" + std::to_string(caves) + " blocks), ore veins placed (" +
                                  std::to_string(ore) + ")");
  check(ravineCut > 0, "pipeline: the ravine carver cuts real surface gashes (" + std::to_string(ravineCut) +
                           " carved surface columns)");
  check(dimensionSeed(1, "a") != dimensionSeed(1, "b") && chunkSeed(5, 0, 1) != chunkSeed(5, 1, 0),
        "seeding: dimensions and neighbouring chunks get distinct seeds");
}

/// Stamps state 7 on the top block of every biome-0 column (extraSurface test rule).
class StampTopState final : public ISurfaceRule {
 public:
  void apply(StageContext& ctx) const override {
    const int n = ctx.chunk.size;
    for (int z = 0; z < n; ++z)
      for (int x = 0; x < n; ++x) {
        const size_t i = size_t(z * n + x);
        const int h = ctx.maps.height[i];
        if (h >= 0 && ctx.maps.biome[i] == 0) ctx.blocks.setState(x, h, z, 7);
      }
  }
};

/// Wafer-shaped generator JSON: climate -> 3-biome MultiNoiseBiomeSelector -> per-biome
/// heightmap terrain -> per-biome surface. The "t" climate node and the terrain biome list
/// are parameters so tests can pin the climate or break the biome count.
std::string waferLikeText(const char* tNode, const char* terrainBiomes) {
  std::string text = R"JSON({
  "graph": {"nodes": {
      "t": )JSON";
  text += tNode;
  text += R"JSON(,
      "h0": {"type": "add", "inputs": [100, {"type": "noise", "noise": "perlin", "frequency": 0.05}]},
      "h1": {"type": "constant", "value": 60},
      "h2": {"type": "constant", "value": 80}},
    "outputs": {"temperature": "t", "h0": "h0", "h1": "h1", "h2": "h2"}},
  "climate": {"temperature": "temperature"},
  "biomes": [
    {"name": "hills", "climate": {"temperature": 0.8}, "surface": {"top": "rock", "depth": 0}},
    {"name": "flat", "climate": {"temperature": 0.5}, "surface": {"top": "turf", "depth": 0}},
    {"name": "dead", "climate": {"temperature": 0.2}, "surface": {"top": "soil", "depth": 0}}],
  "terrain": {"type": "biome_heightmap", "depth": 4, "biomes": [)JSON";
  text += terrainBiomes;
  text += "]}}";
  return text;
}

constexpr const char* kNoiseClimate =
    "{\"type\": \"scale_bias\", \"scale\": 0.5, \"bias\": 0.5, "
    "\"input\": {\"type\": \"noise\", \"noise\": \"opensimplex2s\", \"frequency\": 0.01}}";
constexpr const char* kThreeTerrainBiomes =
    "{\"output\": \"h0\", \"material\": \"rock\"}, "
    "{\"output\": \"h1\", \"material\": \"turf\"}, "
    "{\"output\": \"h2\", \"material\": \"soil\"}";

void waferBiomeChecks(Checker& check) {
  const uint64_t seed = dimensionSeed(11, "wafertest");
  const auto info = [seed](int cx, int cz) {
    ChunkInfo c;
    c.cx = cx;
    c.cz = cz;
    c.size = 16;
    c.height = 128;
    c.dimensionSeed = seed;
    c.chunkSeed = chunkSeed(seed, cx, cz);
    return c;
  };
  std::string err;
  const auto gen =
      loadGenerator(waferLikeText(kNoiseClimate, kThreeTerrainBiomes), seed, testMaterial, err);
  check(gen != nullptr, "wafer: biome_heightmap generator loads" + (err.empty() ? std::string() : " (" + err + ")"));
  if (!gen) return;

  // Determinism: two independent loads, same seed -> identical biome maps and buffers.
  std::string err2;
  const auto gen2 = loadGenerator(waferLikeText(kNoiseClimate, kThreeTerrainBiomes), seed, testMaterial, err2);
  ColumnMaps m1, m2;
  const ColumnBuffer b1 = gen->generate(info(3, -7), &m1);
  const ColumnBuffer b2 = gen2->generate(info(3, -7), &m2);
  check(gen2 != nullptr && m1.biome == m2.biome && hashBuffer(b1) == hashBuffer(b2),
        "wafer: same seed -> same biome map and identical chunk (two independent loads)");

  // A different seed moves the climate, so some column must pick another biome.
  const auto genOther =
      loadGenerator(waferLikeText(kNoiseClimate, kThreeTerrainBiomes), dimensionSeed(12, "wafertest"), testMaterial, err);
  bool differs = false;
  for (int cz = 0; cz < 4 && !differs; ++cz)
    for (int cx = 0; cx < 4 && !differs; ++cx) {
      ColumnMaps ma, mb;
      gen->generate(info(cx, cz), &ma);
      // NOTE: genOther was seeded for a different dimension seed; rebuild ChunkInfo accordingly.
      ChunkInfo oi = info(cx, cz);
      oi.dimensionSeed = dimensionSeed(12, "wafertest");
      oi.chunkSeed = chunkSeed(oi.dimensionSeed, cx, cz);
      genOther->generate(oi, &mb);
      differs = ma.biome != mb.biome;
    }
  check(genOther != nullptr && differs, "wafer: different seed -> different biome map");

  // Flatness: constant climate 0.5 pins every column to the "flat" biome (constant height 60).
  std::string flatErr;
  const auto flat = loadGenerator(waferLikeText("{\"type\": \"constant\", \"value\": 0.5}", kThreeTerrainBiomes),
                                  seed, testMaterial, flatErr);
  bool flatOk = flat != nullptr;
  bool turfOk = flatOk;
  for (int cz = 0; cz < 2 && flatOk; ++cz)
    for (int cx = 0; cx < 2 && flatOk; ++cx) {
      ColumnMaps fm;
      const ColumnBuffer fb = flat->generate(info(cx, cz), &fm);
      for (size_t i = 0; i < fm.biome.size() && flatOk; ++i) {
        const int x = int(i % 16), z = int(i / 16);
        flatOk = flatOk && fm.biome[i] == 1 && fm.height[i] == 60;
        turfOk = turfOk && fb.get(x, 60, z) == MaterialId{2};  // LayeredSurface top = turf
      }
    }
  check(flatOk, "wafer: pinned-climate chunk is perfectly flat (height 60 everywhere)");
  check(turfOk, "wafer: flat biome surface uses its own top material");

  // Dead-zone material: constant climate 0.2 -> "dead" biome, soil-keyed crust and surface.
  std::string deadErr;
  const auto dead = loadGenerator(waferLikeText("{\"type\": \"constant\", \"value\": 0.2}", kThreeTerrainBiomes),
                                  seed, testMaterial, deadErr);
  bool deadOk = dead != nullptr;
  if (dead) {
    ColumnMaps dm;
    const ColumnBuffer db = dead->generate(info(0, 0), &dm);
    for (size_t i = 0; i < dm.biome.size() && deadOk; ++i) {
      const int x = int(i % 16), z = int(i / 16);
      deadOk = deadOk && dm.biome[i] == 2 && dm.height[i] == 80 && db.get(x, 80, z) == MaterialId{3} &&
               db.get(x, 79, z) == MaterialId{3};  // soil top and soil terrain material
    }
  }
  check(deadOk, "wafer: dead biome uses its own (stone-keyed) surface material");

  // The loader rejects a biome_heightmap whose entry count does not match the biome table.
  std::string countErr;
  const bool countRejected =
      !loadGenerator(waferLikeText(kNoiseClimate, "{\"output\": \"h0\", \"material\": \"rock\"}"), seed,
                     testMaterial, countErr);
  check(countRejected && countErr.find("biome") != std::string::npos,
        "wafer: biome_heightmap/biome count mismatch rejected (" + countErr + ")");

  // extraSurface: a code rule appended after the JSON surface rules runs in the pipeline.
  std::string stampErr;
  std::vector<std::unique_ptr<ISurfaceRule>> extra;
  extra.push_back(std::make_unique<StampTopState>());
  const auto stamped = loadGenerator(waferLikeText("{\"type\": \"constant\", \"value\": 0.8}", kThreeTerrainBiomes),
                                     seed, testMaterial, std::move(extra), stampErr);
  bool stampOk = stamped != nullptr;
  if (stamped) {
    ColumnMaps sm;
    const ColumnBuffer sb = stamped->generate(info(1, 1), &sm);
    for (size_t i = 0; i < sm.biome.size() && stampOk; ++i) {
      const int x = int(i % 16), z = int(i / 16);
      stampOk = stampOk && sm.biome[i] == 0 && sb.get(x, sm.height[i], z) == MaterialId{1} &&
                sb.state(x, sm.height[i], z) == 7;
    }
  }
  check(stampOk, "wafer: extraSurface code rule runs after the JSON surface rules");
}

// ---- biome blending (Minecraft-style gradients) ----

constexpr const char* kBlendGradientText = R"JSON({
  "graph": {"nodes": {
      "t": {"type": "scale_bias", "scale": 0.02, "bias": 0.64,
            "input": {"type": "coordinate", "axis": "x"}},
      "h0": {"type": "constant", "value": 60},
      "h1": {"type": "constant", "value": 100}},
    "outputs": {"temperature": "t", "h0": "h0", "h1": "h1"}},
  "climate": {"temperature": "temperature"},
  "biomes": {
    "selector": {"type": "blended", "blend_radius": 0.6, "blend_count": 2},
    "list": [
      {"name": "low", "climate": {"temperature": 0.25}, "surface": {"top": "rock", "depth": 0}},
      {"name": "high", "climate": {"temperature": 0.75}, "surface": {"top": "turf", "depth": 0}}]},
  "terrain": {"type": "biome_heightmap", "depth": 4, "biomes": [
    {"output": "h0", "material": "rock"},
    {"output": "h1", "material": "turf"}]}
})JSON";

void biomeBlendChecks(Checker& check) {
  // Unit: the selector math on synthetic climate points.
  {
    BlendedBiomeSelector sel({{"a", {0.2f, 0.5f, 0.5f, 0.5f}, {1, 0, 0, 0}},
                              {"b", {0.8f, 0.5f, 0.5f, 0.5f}, {1, 0, 0, 0}}},
                             0.5f, 2);
    float w[2] = {};
    const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
    sel.blendWeights({0.2f, 0.5f, 0.5f, 0.5f}, w);
    check(w[0] > 0.9f && w[1] < 0.1f && near(w[0] + w[1], 1.f) &&
              sel.select({0.2f, 0.5f, 0.5f, 0.5f}) == 0,
          "blend: at a biome point the weight is ~one-hot on it");
    sel.blendWeights({0.5f, 0.5f, 0.5f, 0.5f}, w);
    check(near(w[0], 0.5f) && near(w[1], 0.5f) && near(w[0] + w[1], 1.f),
          "blend: midpoint splits 50/50 and sums to 1");
    sel.blendWeights({2.f, 0.5f, 0.5f, 0.5f}, w);
    check(near(w[0], 0.f) && near(w[1], 1.f) && sel.select({2.f, 0.5f, 0.5f, 0.5f}) == 1,
          "blend: far away the nearest biome dominates");
    // No-man's-land smoothness: neighbouring climate points get neighbouring weights
    // (no hard flip even far from every biome point).
    float wa[2] = {}, wb[2] = {};
    sel.blendWeights({0.33f, 0.75f, 0.66f, 0.52f}, wa);
    sel.blendWeights({0.329f, 0.751f, 0.659f, 0.526f}, wb);
    check(std::fabs(wa[0] - wb[0]) < 0.05f && std::fabs(wa[1] - wb[1]) < 0.05f,
          "blend: weights are continuous (no hard border in no-man's-land)");
    float w2[2] = {};
    sel.blendWeights({0.4f, 0.5f, 0.5f, 0.5f}, w2);
    sel.blendWeights({0.4f, 0.5f, 0.5f, 0.5f}, w);
    check(w[0] == w2[0] && w[1] == w2[1] && sel.biomeCount() == 2,
          "blend: deterministic, biomeCount reports the table");
  }

  // Pipeline: the object-form selector loads; bad configs are rejected.
  const uint64_t seed = dimensionSeed(13, "blendtest");
  std::string err;
  const auto gen = loadGenerator(std::string(kBlendGradientText), seed, testMaterial, err);
  check(gen != nullptr, "blend: blended selector JSON loads" + (err.empty() ? std::string() : " (" + err + ")"));
  if (!gen) return;
  std::string badErr;
  const bool badType = !loadGenerator(std::string(R"JSON({"graph": {"nodes": {"h": {"type": "constant", "value": 1}},
      "outputs": {"height": "h", "temperature": "h"}},
      "terrain": {"type": "heightmap", "output": "height", "material": "rock"},
      "climate": {"temperature": "temperature"},
      "biomes": {"selector": {"type": "nope"}, "list": [{"name": "a"}]}})JSON"),
                                          seed, testMaterial, badErr);
  check(badType && badErr.find("selector") != std::string::npos,
        "blend: unknown selector type rejected (" + badErr + ")");
  std::string radiusErr;
  const bool badRadius = !loadGenerator(std::string(R"JSON({"graph": {"nodes": {"h": {"type": "constant", "value": 1}},
      "outputs": {"height": "h", "temperature": "h"}},
      "terrain": {"type": "heightmap", "output": "height", "material": "rock"},
      "climate": {"temperature": "temperature"},
      "biomes": {"selector": {"type": "blended", "blend_radius": 0}, "list": [{"name": "a"}]}})JSON"),
                                            seed, testMaterial, radiusErr);
  check(badRadius, "blend: blend_radius <= 0 rejected (" + radiusErr + ")");

  // Boundary gradient: scan x across the biome border; heights must ramp, never wall.
  const auto info = [seed](int cx, int cz) {
    ChunkInfo c;
    c.cx = cx;
    c.cz = cz;
    c.size = 16;
    c.height = 128;
    c.dimensionSeed = seed;
    c.chunkSeed = chunkSeed(seed, cx, cz);
    return c;
  };
  std::vector<int> heights;  // row z = 0, x = -32..47
  std::vector<int> tops;
  ColumnMaps maps;
  bool weightsOk = true;
  for (int cx = -2; cx <= 2; ++cx) {
    const ColumnBuffer b = gen->generate(info(cx, 0), &maps);
    weightsOk = weightsOk && maps.biomeCount == 2 && maps.biomeWeight.size() == 256 * 2;
    for (int x = 0; x < 16; ++x) {
      const size_t i = size_t(x);  // z = 0
      const float wsum = maps.biomeWeight[i * 2] + maps.biomeWeight[i * 2 + 1];
      weightsOk = weightsOk && std::fabs(wsum - 1.f) < 1e-5f;
      heights.push_back(maps.height[i]);
      tops.push_back(int(b.get(x, maps.height[i], 0)));
    }
  }
  check(weightsOk, "blend: pipeline fills 2 normalized weights per column");
  // Softmax never goes exactly one-hot: the ends are ~pure (within a few blocks of the
  // biome heights), the middle is the gradient.
  bool endsOk = heights.front() >= 57 && heights.front() <= 63 && heights.back() >= 97 && heights.back() <= 103;
  int maxStep = 0, mid = 0;
  for (size_t k = 1; k < heights.size(); ++k) maxStep = std::max(maxStep, std::abs(heights[k] - heights[k - 1]));
  for (int h : heights) mid += (h > 63 && h < 97) ? 1 : 0;
  check(endsOk, "blend: ~pure biomes at the scan ends (60 -> 100)");
  check(maxStep <= 4, "blend: no hard wall (max adjacent height step " + std::to_string(maxStep) + " <= 4)");
  check(mid >= 8, "blend: a real gradient exists (" + std::to_string(mid) + " columns strictly between)");

  // Palette dither: pure palettes at the ends, both palettes inside the band.
  int endRock = 0, endTurf = 0;
  for (int k = 0; k < 8; ++k) endRock += (tops[size_t(k)] == 1);
  for (size_t k = heights.size() - 8; k < heights.size(); ++k) endTurf += (tops[k] == 2);
  const bool endsPure = endRock >= 6 && endTurf >= 6;
  bool bandRock = false, bandTurf = false;
  for (size_t k = 16; k < 48; ++k) {
    bandRock = bandRock || tops[k] == 1;
    bandTurf = bandTurf || tops[k] == 2;
  }
  check(endsPure, "blend: palettes pure outside the band");
  check(bandRock && bandTurf, "blend: palettes dither inside the band (noisy gradient, not a line)");

  // Determinism: same seed, same chunk -> identical heights, weights and tops.
  ColumnMaps m2;
  const ColumnBuffer b2 = gen->generate(info(0, 0), &m2);
  ColumnMaps m1;
  const ColumnBuffer b1 = gen->generate(info(0, 0), &m1);
  check(m1.biomeWeight == m2.biomeWeight && hashBuffer(b1) == hashBuffer(b2),
        "blend: same seed -> identical weights and chunk");
}

// ---- silicon forest features (trace trees, capacitor cans, resonator spires)

ChunkInfo treeChunk(int cx, int cz, uint64_t dimSeed) {
  ChunkInfo c;
  c.cx = cx;
  c.cz = cz;
  c.size = 16;
  c.height = 128;
  c.dimensionSeed = dimSeed;
  c.chunkSeed = chunkSeed(dimSeed, cx, cz);
  return c;
}

/// Flat (y 64) rock fixture with one feature, built in code via Builder::feature().
std::unique_ptr<const GeneratorPipeline> makeTreePipeline(uint64_t dimSeed, TraceTreeSettings ts) {
  GraphDesc desc;
  std::string err;
  if (!GraphDesc::parse(R"({"nodes": {"h": {"type": "constant", "value": 64}}, "outputs": {"height": "h"}})", desc,
                        err))
    return nullptr;
  auto graph = NoiseGraph::compile(desc, dimSeed, err);
  if (!graph) return nullptr;
  GeneratorPipeline::Builder b(graph);
  b.terrain(std::make_unique<HeightmapTerrain>(graph, graph->outputIndex("height"), MaterialId{1}, 0));
  if (!b.feature("trees", std::make_unique<TraceTreeFeature>(std::move(ts)))) return nullptr;
  return b.build();
}

/// Independent replica of TraceTreeFeature::place for one base: the expected blocks keyed by
/// chunk-local (x, y, z). Mirrors the implementation's seeded stream consumption exactly, so
/// any divergence (chunk-rng use, wrong reach, out-of-chunk writes) fails the comparison.
void replicaTree(uint64_t featSeed, const TraceTreeSettings& st, int32_t wx, int32_t wz, int bx, int bz, int surfY,
                 int top, long* cross, std::map<std::tuple<int, int, int>, MaterialId>& out) {
  if (unitFloat(latticeHash(featSeed, wx, wz)) >= st.biomes.at(0)) return;  // no biome selector: all biome 0
  const int maxH = st.maxTrunkH + st.maxBranchLen + 1 + kTraceTreeCanopyRadiusVMax;
  if (surfY < 0 || surfY + maxH >= top) return;
  Rng r(deriveSeed(featSeed, mix64((uint64_t(uint32_t(wx)) << 32) | uint64_t(uint32_t(wz)))));
  const int trunkH = r.range(st.minTrunkH, st.maxTrunkH);
  const MaterialId tip = r.uniform() < st.redTipChance ? st.tipRed : st.tipBlue;
  const bool marginBase = bx < 0 || bx >= 16 || bz < 0 || bz >= 16;
  auto put = [&](int x, int y, int z, MaterialId m) {
    if (x < 0 || x >= 16 || z < 0 || z >= 16 || y < 0 || y >= top) return;
    const auto key = std::make_tuple(x, y, z);
    if (out.find(key) != out.end()) return;  // first writer wins (air-only rule)
    out[key] = m;
    if (marginBase && cross) ++(*cross);
  };
  for (int i = 1; i <= trunkH; ++i) put(bx, surfY + i, bz, st.trunk);
  put(bx, surfY + trunkH + 1, bz, tip);
  const int nBr = r.range(st.minBranches, st.maxBranches);
  constexpr int dx8[8] = {1, 1, 0, -1, -1, -1, 0, 1};
  constexpr int dz8[8] = {0, 1, 1, 1, 0, -1, -1, -1};
  int dirs[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  for (int i = 0; i < nBr; ++i) {
    const int j = i + r.range(0, 7 - i);
    const int t = dirs[i];
    dirs[i] = dirs[j];
    dirs[j] = t;
  }
  for (int b = 0; b < nBr; ++b) {
    const int dx = dx8[dirs[b]], dz = dz8[dirs[b]];
    const int len = r.range(st.minBranchLen, st.maxBranchLen);
    int x = bx, z = bz, y = surfY + r.range(2, std::max(2, trunkH));
    for (int i = 0; i < len; ++i) {
      x += dx;
      z += dz;
      y += r.range(0, 1);
      put(x, y, z, i + 1 == len ? tip : st.branch);
    }
  }
  // Canopy: shared generator (its own position-keyed stream; the replica's job
  // here is verifying border replay and first-writer order, not re-deriving the
  // superformula stamp — that is covered directly by treeCanopyChecks).
  const TraceTreeCanopySpec canopy = traceTreeCanopyFor(featSeed, wx, wz);
  const int crownY = surfY + trunkH + 1;
  for (const TraceTreeCanopyCell& cell : traceTreeCanopyCells(canopy))
    put(bx + cell.dx, crownY + cell.dy, bz + cell.dz, cell.tip ? tip : st.branch);
}

void treeBorderChecks(Checker& check) {
  TraceTreeSettings ts;
  ts.trunk = MaterialId{10};
  ts.branch = MaterialId{11};
  ts.tipRed = MaterialId{12};
  ts.tipBlue = MaterialId{13};
  ts.surface = MaterialId{1};
  ts.biomes.density.fill(0.12f);
  const uint64_t dimSeed = dimensionSeed(77, "wafer");
  const auto gen = makeTreePipeline(dimSeed, ts);
  check(gen != nullptr, "trees: code-built pipeline via Builder::feature() builds");
  if (!gen) return;
  const uint64_t featSeed = deriveSeed(dimSeed, nameHash("trace_tree"));
  const int reach = std::max(ts.maxBranchLen, kTraceTreeCanopyRadiusHMax);
  bool ok = true;
  long crossBorder = 0;
  for (int cz = -1; cz <= 1; ++cz)
    for (int cx = -1; cx <= 1; ++cx) {
      ColumnMaps maps;
      const ColumnBuffer buf = gen->generate(treeChunk(cx, cz, dimSeed), &maps);
      for (size_t i = 0; i < 256; ++i) ok = ok && maps.height[i] == 64;  // flat fixture
      std::map<std::tuple<int, int, int>, MaterialId> expected;
      for (int bz = -reach; bz < 16 + reach; ++bz)
        for (int bx = -reach; bx < 16 + reach; ++bx)
          replicaTree(featSeed, ts, int32_t(cx * 16 + bx), int32_t(cz * 16 + bz), bx, bz, 64, 128, &crossBorder,
                      expected);
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
          for (int y = 0; y < 128; ++y) {
            const auto it = expected.find(std::make_tuple(x, y, z));
            const MaterialId want = it == expected.end() ? (y <= 64 ? MaterialId{1} : kAirMaterial) : it->second;
            if (buf.get(x, y, z) != want) ok = false;
          }
    }
  check(ok, "trees: 3x3 chunks match the position-keyed replica exactly (no phantoms, terrain untouched)");
  check(crossBorder > 0, "trees: margin replay writes cross-boundary blocks (" + std::to_string(crossBorder) +
                              " blocks, no clipping)");
  const uint64_t h1 = hashBuffer(gen->generate(treeChunk(5, -7, dimSeed)));
  const uint64_t h2 = hashBuffer(gen->generate(treeChunk(5, -7, dimSeed)));
  const uint64_t otherSeed = dimensionSeed(78, "wafer");
  const auto gen2 = makeTreePipeline(otherSeed, ts);
  const uint64_t h3 = gen2 ? hashBuffer(gen2->generate(treeChunk(5, -7, otherSeed))) : 0;
  check(h1 == h2, "trees: the same chunk regenerates identically");
  check(gen2 && h1 != h3, "trees: a different seed places different trees");
}

constexpr const char* kTreeGeneratorText = R"({
  "graph": {"nodes": {
      "h": {"type": "constant", "value": 64},
      "c": {"type": "add", "inputs": [0.5, {"type": "wave", "axis": "x", "function": "sin", "frequency": 0.07, "amplitude":0.5}]}},
    "outputs": {"height": "h", "continentalness": "c"}},
  "terrain": {"type": "heightmap", "output": "height", "material": "rock", "depth": 0},
  "climate": {"continentalness": "continentalness"},
  "biomes": [{"name": "forest", "climate": {"continentalness": 0.1}},
             {"name": "flats", "climate": {"continentalness": 0.3}},
             {"name": "grove", "climate": {"continentalness": 0.5}},
             {"name": "spire", "climate": {"continentalness": 0.7}},
             {"name": "dead", "climate": {"continentalness": 0.9}}],
  "features": [
    {"name": "trees", "type": "trace_tree", "trunk": "trunk", "branch": "branch",
     "tip_red": "tipred", "tip_blue": "tipblue", "surface": "rock",
     "red_tip_chance": 0.2, "density": 0.0, "biomes": {"forest": 0.09, "flats": 0.006}},
    {"name": "cans", "type": "capacitor_cans", "can": "can", "surface": "rock",
     "density": 0.0, "biomes": {"grove": 0.08}},
    {"name": "spires", "type": "resonator_spire", "crystal": "crystal", "surface": "rock",
     "density": 0.0, "biomes": {"spire": 0.03}}]
})";

/// Superformula canopy checks (M11): determinism, per-instance variance, and
/// silhouette sanity, exercised directly on traceTreeCanopyFor/Cells.
void treeCanopyChecks(Checker& check) {
  const uint64_t featSeed = deriveSeed(dimensionSeed(99, "wafer"), nameHash("trace_tree"));
  auto signature = [](const std::vector<TraceTreeCanopyCell>& cells) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (const TraceTreeCanopyCell& c : cells) {
      const uint64_t key = (uint64_t(uint32_t(c.dx)) << 40) | (uint64_t(uint32_t(c.dy)) << 20) |
                           uint64_t(uint32_t(c.dz)) | (c.tip ? 0x1000000000000ull : 0ull);
      h = mix64(h ^ key);
    }
    return h;
  };
  auto extents = [](const std::vector<TraceTreeCanopyCell>& cells, int& w, int& hgt) {
    int mnx = 99, mxx = -99, mny = 99, mxy = -99;
    for (const TraceTreeCanopyCell& c : cells) {
      mnx = std::min(mnx, c.dx); mxx = std::max(mxx, c.dx);
      mny = std::min(mny, c.dy); mxy = std::max(mxy, c.dy);
    }
    w = cells.empty() ? 0 : mxx - mnx + 1;
    hgt = cells.empty() ? 0 : mxy - mny + 1;
  };

  // Determinism: same (seed, position) -> identical spec and identical stamp.
  {
    const TraceTreeCanopySpec a = traceTreeCanopyFor(featSeed, 123, -456);
    const TraceTreeCanopySpec b = traceTreeCanopyFor(featSeed, 123, -456);
    check(a.archetype == b.archetype && a.radiusH == b.radiusH && a.radiusV == b.radiusV &&
              a.theta.m == b.theta.m && a.theta.n1 == b.theta.n1 && a.phi.n1 == b.phi.n1,
          "canopy: same seed+pos -> identical spec");
    check(signature(traceTreeCanopyCells(a)) == signature(traceTreeCanopyCells(b)),
          "canopy: same seed+pos -> identical stamp");
  }

  // Variance: 64 distinct anchor positions -> many distinct canopy signatures,
  // and all four archetypes show up.
  {
    std::set<uint64_t> sigs;
    bool seenArch[4] = {};
    bool boundsOk = true;
    for (int i = 0; i < 64; ++i) {
      const TraceTreeCanopySpec s = traceTreeCanopyFor(featSeed, i * 37 - 1000, i * 91 + 500);
      seenArch[size_t(s.archetype)] = true;
      boundsOk = boundsOk && s.radiusH >= 1 && s.radiusH <= kTraceTreeCanopyRadiusHMax &&
                 s.radiusV >= 1 && s.radiusV <= kTraceTreeCanopyRadiusVMax;
      const auto cells = traceTreeCanopyCells(s);
      boundsOk = boundsOk && !cells.empty();
      for (const TraceTreeCanopyCell& c : cells)
        boundsOk = boundsOk && std::abs(c.dx) <= kTraceTreeCanopyRadiusHMax &&
                   std::abs(c.dz) <= kTraceTreeCanopyRadiusHMax &&
                   std::abs(c.dy) <= kTraceTreeCanopyRadiusVMax;
      sigs.insert(signature(cells));
    }
    check(sigs.size() >= 16, "canopy: 64 positions -> >=16 distinct signatures (" +
                                 std::to_string(sigs.size()) + " distinct)");
    check(seenArch[0] && seenArch[1] && seenArch[2] && seenArch[3],
          "canopy: all four archetypes appear (round/spire/parasol/lobed)");
    check(boundsOk, "canopy: every stamp nonempty and inside the documented radii");
  }

  // Silhouette sanity: find one spire and one parasol in a deterministic scan;
  // spire must be taller than wide, parasol wider than tall.
  {
    bool spireOk = false, parasolOk = false;
    for (int i = 0; i < 512 && !(spireOk && parasolOk); ++i) {
      const TraceTreeCanopySpec s = traceTreeCanopyFor(featSeed, i * 13 - 3000, i * 7 + 11);
      int w = 0, h = 0;
      extents(traceTreeCanopyCells(s), w, h);
      if (s.archetype == TraceTreeCanopyArchetype::Spire && h > w) spireOk = true;
      if (s.archetype == TraceTreeCanopyArchetype::Parasol && w > h) parasolOk = true;
    }
    check(spireOk, "canopy: spire silhouette is taller than wide");
    check(parasolOk, "canopy: parasol silhouette is wider than tall");
  }

  // Perf (informational, not a gate): stamp generation cost per tree.
  {
    const auto t0 = std::chrono::steady_clock::now();
    size_t totalCells = 0;
    for (int i = 0; i < 1000; ++i)
      totalCells += traceTreeCanopyCells(traceTreeCanopyFor(featSeed, i * 3, i * 5)).size();
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("    canopy perf: %.3f ms/tree (1000 trees, %zu cells total)\n", ms / 1000.0,
                totalCells);
  }
}

void treeLoaderChecks(Checker& check) {
  const std::string head = R"({
    "graph": {"nodes": {"h": {"type": "constant", "value": 64}}, "outputs": {"height": "h"}},
    "terrain": {"type": "heightmap", "output": "height", "material": "rock"},
    "biomes": [{"name": "forest", "climate": {"continentalness": 0.1}}],
    "features": [)";
  const std::string treeFeat =
      R"({"name": "t", "type": "trace_tree", "trunk": "trunk", "branch": "branch",)"
      R"( "tip_red": "tipred", "tip_blue": "tipblue")";
  std::string err;
  const bool badBiome =
      !loadGenerator(head + treeFeat + R"(, "biomes": {"nope": 0.1}}]})", 0, testMaterial, err);
  check(badBiome && err.find("unknown biome") != std::string::npos,
        "trees: unknown biome name rejected (" + err + ")");
  err.clear();
  const bool badDensity =
      !loadGenerator(head + treeFeat + R"(, "biomes": {"forest": 2}}]})", 0, testMaterial, err);
  check(badDensity && err.find("in [0, 1]") != std::string::npos,
        "trees: density outside [0, 1] rejected (" + err + ")");
  err.clear();
  const bool missingMat =
      !loadGenerator(head + R"({"name": "t", "type": "trace_tree", "branch": "branch", "tip_red": "tipred",)"
                           R"( "tip_blue": "tipblue"}]})",
                     0, testMaterial, err);
  check(missingMat && err.find("missing 'trunk'") != std::string::npos,
        "trees: missing material rejected (" + err + ")");
  err.clear();
  const std::string noBiomes = R"({
    "graph": {"nodes": {"h": {"type": "constant", "value": 64}}, "outputs": {"height": "h"}},
    "terrain": {"type": "heightmap", "output": "height", "material": "rock"},
    "features": [)" + treeFeat + R"(, "density": 0.5}]})";
  check(loadGenerator(noBiomes, 0, testMaterial, err) != nullptr,
        "trees: loads with no biomes section (density is the default)" + (err.empty() ? "" : " (" + err + ")"));
  {  // B-L3 (P3-2): that scalar-without-biomes load now IGNORES the scalar
     // (fills 0 + logs loudly) instead of enabling the feature in every
     // biome: the generated chunk must equal the no-feature reference.
    const std::string refText = R"({
      "graph": {"nodes": {"h": {"type": "constant", "value": 64}}, "outputs": {"height": "h"}},
      "terrain": {"type": "heightmap", "output": "height", "material": "rock"}})";
    auto gScalar = loadGenerator(noBiomes, 0, testMaterial, err);
    auto gRef = loadGenerator(refText, 0, testMaterial, err);
    check(gScalar && gRef &&
              hashBuffer(gScalar->generate(treeChunk(3, 3, 0))) == hashBuffer(gRef->generate(treeChunk(3, 3, 0))),
          "trees (B-L3): scalar density without a biomes object is ignored (feature spawns nowhere)");
  }
}

void treeDensityChecks(Checker& check) {
  std::string err;
  const uint64_t dimSeed = dimensionSeed(7, "wafer");
  const auto gen = loadGenerator(std::string(kTreeGeneratorText), dimSeed, testMaterial, err);
  check(gen != nullptr, "trees: generator JSON with the three feature types loads" +
                            (err.empty() ? std::string() : " (" + err + ")"));
  if (!gen) return;
  // Mirror of the JSON density tables: forest=0, flats=1, grove=2, spire=3, dead=4.
  BiomeDensity treeDens, canDens, spireDens;
  treeDens.density.fill(0.f);
  treeDens.density[0] = 0.09f;
  treeDens.density[1] = 0.006f;
  canDens.density.fill(0.f);
  canDens.density[2] = 0.08f;
  spireDens.density.fill(0.f);
  spireDens.density[3] = 0.03f;
  const uint64_t treeSeed = deriveSeed(dimSeed, nameHash("trace_tree"));
  const uint64_t canSeed = deriveSeed(dimSeed, nameHash("capacitor_cans"));
  const uint64_t spireSeed = deriveSeed(dimSeed, nameHash("resonator_spire"));

  // 10x10 chunks; the inner 8x8 is evaluated, the ring supplies margin replays.
  std::map<std::pair<int, int>, ColumnMaps> mapsByChunk;
  std::map<std::pair<int, int>, std::unique_ptr<ColumnBuffer>> bufsByChunk;
  for (int cz = -1; cz <= 8; ++cz)
    for (int cx = -1; cx <= 8; ++cx) {
      ColumnMaps m;
      auto b = std::make_unique<ColumnBuffer>(gen->generate(treeChunk(cx, cz, dimSeed), &m));
      mapsByChunk[{cx, cz}] = std::move(m);
      bufsByChunk[{cx, cz}] = std::move(b);
    }
  struct FeatureCase {
    uint64_t seed;
    int reach;
    const BiomeDensity* dens;
    std::set<uint8_t> allowed;  // dominant biomes a base may initiate from
  };
  const FeatureCase cases[3] = {
      {treeSeed, 3, &treeDens, {0, 1}},
      {canSeed, 2, &canDens, {2}},
      {spireSeed, 1, &spireDens, {3}},
  };
  bool weightsOk = true;
  for (int cz = 0; cz < 8; ++cz)
    for (int cx = 0; cx < 8; ++cx) {
      const ColumnMaps& m = mapsByChunk[{cx, cz}];
      weightsOk = weightsOk && m.biomeCount == 5 && m.biomeWeight.size() == 256 * 5;
    }

  // Gate: recompute the placement decision per column with the same seeded dither the
  // features use. In-chunk bases are exact (no margin approximation); their dominant
  // biome must be allowed -- this is the strict "nothing initiates in Dead Zones" check.
  bool gateOk = true;
  long inChunkBases[3] = {};
  double treePerBiome[5] = {};
  for (int fi = 0; fi < 3; ++fi) {
    const FeatureCase& fc = cases[fi];
    for (int cz = -1; cz <= 8; ++cz)
      for (int cx = -1; cx <= 8; ++cx) {
        const ColumnMaps& m = mapsByChunk[{cx, cz}];
        for (int bz = 0; bz < 16; ++bz)
          for (int bx = 0; bx < 16; ++bx) {
            const int32_t wx = int32_t(cx * 16 + bx), wz = int32_t(cz * 16 + bz);
            if (wx < 0 || wx >= 128 || wz < 0 || wz >= 128) continue;  // inner area only
            const float eff = blendedDensity(*fc.dens, m, size_t(bz * 16 + bx));
            if (eff <= 0.f) continue;
            if (unitFloat(latticeHash(fc.seed, wx, wz)) < eff) {
              ++inChunkBases[fi];
              gateOk = gateOk && fc.allowed.count(m.biome[size_t(bz * 16 + bx)]) > 0;
              if (fi == 0) treePerBiome[m.biome[size_t(bz * 16 + bx)]] += 1;
            }
          }
      }
  }
  check(weightsOk, "trees: pipeline fills one-hot biome weights (5 biomes)");
  check(gateOk, "trees: every initiated base stands in an allowed biome (strict gate, incl. Dead Zones)");
  check(inChunkBases[0] > 50 && inChunkBases[1] > 20 && inChunkBases[2] > 10,
        "trees: all three features initiate (" + std::to_string(inChunkBases[0]) + "/" +
            std::to_string(inChunkBases[1]) + "/" + std::to_string(inChunkBases[2]) + " bases)");
  char buf[256];
  std::snprintf(buf, sizeof buf, "trees: %.1f/chunk in forest, %.1f/chunk on flats", treePerBiome[0] / 64,
                treePerBiome[1] / 64);
  check(treePerBiome[0] / 64 > 2 && treePerBiome[0] / 64 < 12 && treePerBiome[1] / 64 < 2 &&
            treePerBiome[0] > 5 * treePerBiome[1],
        buf);

  // Placement reality: expected bases (including margin replays) vs actual blocks.
  std::set<std::pair<int, int>> expected[3];
  for (int fi = 0; fi < 3; ++fi) {
    const FeatureCase& fc = cases[fi];
    for (int cz = -1; cz <= 8; ++cz)
      for (int cx = -1; cx <= 8; ++cx) {
        const ColumnMaps& m = mapsByChunk[{cx, cz}];
        for (int bz = -fc.reach; bz < 16 + fc.reach; ++bz)
          for (int bx = -fc.reach; bx < 16 + fc.reach; ++bx) {
            const int32_t wx = int32_t(cx * 16 + bx), wz = int32_t(cz * 16 + bz);
            const int lx = std::clamp(bx, 0, 15), lz = std::clamp(bz, 0, 15);
            const float eff = blendedDensity(*fc.dens, m, size_t(lz * 16 + lx));
            if (eff > 0.f && unitFloat(latticeHash(fc.seed, wx, wz)) < eff)
              expected[fi].insert({wx, wz});
          }
      }
  }
  const MaterialId treeMats[4] = {MaterialId{10}, MaterialId{11}, MaterialId{12}, MaterialId{13}};
  bool realityOk = true;
  long tipsRed = 0, tipsBlue = 0;
  for (int cz = 0; cz < 8; ++cz)
    for (int cx = 0; cx < 8; ++cx) {
      const ColumnBuffer& b = *bufsByChunk[{cx, cz}];
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
          const int wx = cx * 16 + x, wz = cz * 16 + z;
          for (int y = 65; y < 128; ++y) {
            const MaterialId m = b.get(x, y, z);
            if (m == MaterialId{12}) ++tipsRed;
            else if (m == MaterialId{13}) ++tipsBlue;
            int fi = -1;
            for (int k = 0; k < 4; ++k)
              if (m == treeMats[k]) fi = 0;
            if (m == MaterialId{14}) fi = 1;
            else if (m == MaterialId{15}) fi = 2;
            if (fi < 0) continue;
            bool near = false;
            for (int dz = -cases[fi].reach; dz <= cases[fi].reach && !near; ++dz)
              for (int dx = -cases[fi].reach; dx <= cases[fi].reach && !near; ++dx)
                near = expected[fi].count({wx + dx, wz + dz}) > 0;
            realityOk = realityOk && near;
            if (fi == 0 && m == MaterialId{10} && b.get(x, y - 1, z) == MaterialId{1})
              realityOk = realityOk && expected[0].count({wx, wz}) > 0;  // trunk => expected base
          }
        }
    }
  // Every base the OWNING chunk planted grew its trunk (no missing placements). Bases only
  // a margin replay accepted (owner declined at a biome+chunk border -- the documented
  // nearest-column approximation) may leave trunkless branch stubs; they are rare and bounded.
  long marginOnly = 0;
  for (int cz = 0; cz < 8; ++cz)
    for (int cx = 0; cx < 8; ++cx) {
      const ColumnBuffer& b = *bufsByChunk[{cx, cz}];
      const ColumnMaps& m = mapsByChunk[{cx, cz}];
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
          const int wx = cx * 16 + x, wz = cz * 16 + z;
          if (expected[0].count({wx, wz}) == 0) continue;
          const float effOwner = blendedDensity(treeDens, m, size_t(z * 16 + x));
          const bool ownerPlants =
              effOwner > 0.f && unitFloat(latticeHash(treeSeed, int32_t(wx), int32_t(wz))) < effOwner;
          if (ownerPlants)
            realityOk = realityOk && b.get(x, 65, z) == MaterialId{10} && b.get(x, 64, z) == MaterialId{1};
          else
            ++marginOnly;
        }
    }
  check(realityOk, "trees: blocks match expected bases (no phantoms, no missing trunks)");
  char mbuf[128];
  std::snprintf(mbuf, sizeof mbuf, "trees: margin-only bases %ld (%.1f%%, border approximation)",
                marginOnly, 100.0 * marginOnly / std::max(1L, inChunkBases[0]));
  check(marginOnly < inChunkBases[0] / 5, mbuf);
  const double redFrac = double(tipsRed) / double(std::max(1L, tipsRed + tipsBlue));
  std::snprintf(buf, sizeof buf, "trees: %.0f%% red tips (%ld red, %ld blue)", redFrac * 100, tipsRed, tipsBlue);
  check(tipsRed + tipsBlue > 100 && redFrac > 0.05 && redFrac < 0.4, buf);
}

/// Fixed-weight blender stub: the integration contract IBiomeSelector::blendWeights.
class FixedBlendSelector final : public IBiomeSelector {
 public:
  explicit FixedBlendSelector(float w0) : w0_(w0) {}
  uint8_t select(const ClimatePoint&) const override { return 0; }
  size_t biomeCount() const override { return 2; }
  void blendWeights(const ClimatePoint&, float* out) const override {
    out[0] = w0_;
    out[1] = 1.f - w0_;
  }

 private:
  float w0_;
};

std::unique_ptr<const GeneratorPipeline> makeBlendPipeline(uint64_t dimSeed, float w0, TraceTreeSettings ts) {
  GraphDesc desc;
  std::string err;
  if (!GraphDesc::parse(R"({"nodes": {"h": {"type": "constant", "value": 64}}, "outputs": {"height": "h"}})", desc,
                        err))
    return nullptr;
  auto graph = NoiseGraph::compile(desc, dimSeed, err);
  if (!graph) return nullptr;
  GeneratorPipeline::Builder b(graph);
  b.terrain(std::make_unique<HeightmapTerrain>(graph, graph->outputIndex("height"), MaterialId{1}, 0));
  b.biomes(std::make_unique<FixedBlendSelector>(w0));
  if (!b.feature("trees", std::make_unique<TraceTreeFeature>(std::move(ts)))) return nullptr;
  return b.build();
}

void blendChecks(Checker& check) {
  // Unit: the blending math on synthetic maps.
  {
    BiomeDensity d;
    d.density.fill(0.f);
    d.density[0] = 0.1f;
    d.density[1] = 0.4f;
    ColumnMaps m;
    m.biome.assign(4, uint8_t(0));
    m.biomeCount = 2;
    m.biomeWeight = {0.3f, 0.7f, 1.f, 0.f, 0.f, 1.f, 0.25f, 0.75f};
    const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-6f; };
    check(near(blendedDensity(d, m, 0), 0.3f * 0.1f + 0.7f * 0.4f), "blend: 30/70 weights scale density");
    check(near(blendedDensity(d, m, 1), 0.1f), "blend: one-hot weight keeps full density");
    check(near(blendedDensity(d, m, 2), 0.4f), "blend: one-hot on the other biome");
    check(near(blendedDensity(d, m, 3), 0.25f * 0.1f + 0.75f * 0.4f), "blend: 25/75 weights scale density");
    ColumnMaps m2;
    m2.biome.assign(4, uint8_t(1));  // no weight vector: dominant biome, weight 1
    check(near(blendedDensity(d, m2, 2), 0.4f), "blend: no weights -> dominant biome density");
    ColumnMaps m3;
    check(blendedDensity(d, m3, 0) == 0.f, "blend: empty maps -> zero density");
  }
  // Integration: 50/50 blend vs one-hot at the same seed. The dither stream is identical, so
  // halving the effective density can only remove trees, never move them (subset property).
  TraceTreeSettings ts;
  ts.trunk = MaterialId{10};
  ts.branch = MaterialId{11};
  ts.tipRed = MaterialId{12};
  ts.tipBlue = MaterialId{13};
  ts.surface = MaterialId{1};
  ts.biomes.density.fill(0.f);
  ts.biomes.density[0] = 0.2f;
  const uint64_t dimSeed = dimensionSeed(99, "wafer");
  const auto full = makeBlendPipeline(dimSeed, 1.f, ts);
  const auto half = makeBlendPipeline(dimSeed, 0.5f, ts);
  check(full && half, "blend: blended and one-hot pipelines build");
  if (!full || !half) return;
  auto bases = [&](const GeneratorPipeline& g, bool checkWeights) {
    std::set<std::pair<int, int>> s;
    for (int cz = 0; cz < 4; ++cz)
      for (int cx = 0; cx < 4; ++cx) {
        ColumnMaps maps;
        const ColumnBuffer buf = g.generate(treeChunk(cx, cz, dimSeed), &maps);
        if (checkWeights) {
          if (maps.biomeWeight.size() != 256 * 2) return s;  // fail visibly below
          for (size_t i = 0; i < 256; ++i)
            if (std::fabs(maps.biomeWeight[i * 2] - 0.5f) > 1e-6f ||
                std::fabs(maps.biomeWeight[i * 2 + 1] - 0.5f) > 1e-6f)
              return s;
        }
        for (int z = 0; z < 16; ++z)
          for (int x = 0; x < 16; ++x)
            if (buf.get(x, 65, z) == MaterialId{10} && buf.get(x, 64, z) == MaterialId{1})
              s.insert({cx * 16 + x, cz * 16 + z});
      }
    return s;
  };
  const auto fullBases = bases(*full, false);
  const auto halfBases = bases(*half, true);
  bool subset = !fullBases.empty() && !halfBases.empty();
  for (const auto& p : halfBases) subset = subset && fullBases.count(p) > 0;
  check(subset, "blend: 50/50 tree bases are a strict subset of the one-hot bases (" +
                    std::to_string(halfBases.size()) + " of " + std::to_string(fullBases.size()) + ")");
  const double ratio = double(halfBases.size()) / double(fullBases.size());
  char buf[128];
  std::snprintf(buf, sizeof buf, "blend: 50/50 blend keeps %.0f%% of trees", ratio * 100);
  check(ratio > 0.3 && ratio < 0.7, buf);
}

void treeFeatureChecks(Checker& check) {
  treeLoaderChecks(check);
  treeCanopyChecks(check);
  treeBorderChecks(check);
  treeDensityChecks(check);
  blendChecks(check);
}

// ---- structures (StructureFeature framework)

constexpr const char* kStructureGeneratorText = R"({
  "graph": {"nodes": {
      "h": {"type": "constant", "value": 64},
      "t": {"type": "constant", "value": 0.8}},
    "outputs": {"height": "h", "temperature": "t"}},
  "terrain": {"type": "heightmap", "output": "height", "material": "rock"},
  "climate": {"temperature": "temperature"},
  "biomes": [{"name": "hot", "climate": {"temperature": 0.8}}, {"name": "cold", "climate": {"temperature": 0.2}}],
  "features": [{"name": "hut", "type": "structure", "cell_size": 32, "density": 1.0,
                "biomes": {"hot": 1.0, "cold": 0.0},
                "blueprint": [{"d": [0, 0, 0], "m": "turf"}, {"d": [1, 0, 0], "m": "turf"},
                              {"d": [0, 1, 0], "m": "ore"}, {"d": [0, 0, 1], "m": "turf", "decay": 0.5}]}]
})";

const StructureFeature* findStructure(const GeneratorPipeline& pipe, const char* name) {
  const StructureFeature* out = nullptr;
  pipe.visitFeatures([&](std::string_view n, const IFeature& f) {
    if (n == name) out = dynamic_cast<const StructureFeature*>(&f);
  });
  return out;
}

void structureLoaderChecks(Checker& check) {
  std::string err;
  const auto gen = loadGenerator(std::string(kStructureGeneratorText), dimensionSeed(7, "hut"), testMaterial, err);
  check(gen != nullptr, "structures: generator JSON with a structure feature loads" +
                            (err.empty() ? std::string() : " (" + err + ")"));
  if (!gen) return;
  const StructureFeature* hut = findStructure(*gen, "hut");
  check(hut != nullptr, "structures: feature visits as a StructureFeature");
  if (hut) {
    check(hut->reach() == 1, "structures: reach = blueprint extent (1)");
    check(hut->blueprint().size() == 4, "structures: blueprint kept 4 cells");
    check(hut->settings().cellSize == 32, "structures: cell_size loaded");
  }

  // biomeAt / sampleBiomes: constant 0.8 temperature -> biome 0 ("hot") everywhere.
  check(gen->biomeAt(10, -20) == 0, "structures: biomeAt agrees with the constant climate");
  {
    int32_t xs[4] = {0, 100, -50, 7}, zs[4] = {0, -3, 999, 7};
    uint8_t got[4] = {9, 9, 9, 9};
    gen->sampleBiomes(xs, zs, 4, got);
    bool all = true;
    for (int i = 0; i < 4; ++i) all &= got[i] == gen->biomeAt(xs[i], zs[i]);
    check(all && got[0] == 0, "structures: sampleBiomes batch == biomeAt singles");
  }

  // Loader rejections.
  const auto bad = [&](const char* what, const std::string& jsonText) {
    std::string e;
    const bool rejected = !loadGenerator(jsonText, dimensionSeed(7, "hut"), testMaterial, e);
    check(rejected, std::string("structures: rejected (") + what + "): " + e);
  };
  bad("unknown biome", R"({"graph": {"nodes": {"h": {"type": "constant"}}, "outputs": {"height": "h"}},)"
                       R"( "terrain": {"output": "height", "material": "rock"},)"
                       R"( "biomes": [{"name": "hot"}],)"
                       R"( "features": [{"name": "s", "type": "structure", "biomes": {"nope": 1.0},)"
                       R"( "blueprint": [{"d": [0,0,0], "m": "turf"}]}]})");
  bad("unknown material", R"({"graph": {"nodes": {"h": {"type": "constant"}}, "outputs": {"height": "h"}},)"
                           R"( "terrain": {"output": "height", "material": "rock"},)"
                           R"( "features": [{"name": "s", "type": "structure",)"
                           R"( "blueprint": [{"d": [0,0,0], "m": "nope"}]}]})");
  bad("missing blueprint", R"({"graph": {"nodes": {"h": {"type": "constant"}}, "outputs": {"height": "h"}},)"
                            R"( "terrain": {"output": "height", "material": "rock"},)"
                            R"( "features": [{"name": "s", "type": "structure"}]})");
  // Negative dy (buried cells) is legal since the placement-solver upgrade: place()
  // overwrites terrain for dy < 0. Verify it loads.
  {
    std::string err;
    const auto gen = loadGenerator(R"({"graph": {"nodes": {"h": {"type": "constant"}}, "outputs": {"height": "h"}},)"
                                   R"( "terrain": {"output": "height", "material": "rock"},)"
                                   R"( "features": [{"name": "s", "type": "structure",)"
                                   R"( "blueprint": [{"d": [0,-1,0], "m": "turf"}]}]})",
                                   dimensionSeed(1, "buried"), testMaterial, err);
    check(gen != nullptr, "structures: negative dy (buried) loads");
  }
  bad("bad d", R"({"graph": {"nodes": {"h": {"type": "constant"}}, "outputs": {"height": "h"}},)"
                R"( "terrain": {"output": "height", "material": "rock"},)"
                R"( "features": [{"name": "s", "type": "structure",)"
                R"( "blueprint": [{"d": [0,0], "m": "turf"}]}]})");
  bad("unknown type", R"({"graph": {"nodes": {"h": {"type": "constant"}}, "outputs": {"height": "h"}},)"
                      R"( "terrain": {"output": "height", "material": "rock"},)"
                      R"( "features": [{"name": "s", "type": "hut"}]})");
}

void structurePlacementChecks(Checker& check) {
  const uint64_t dimSeed = dimensionSeed(7, "hut");
  std::string err;
  const auto gen = loadGenerator(std::string(kStructureGeneratorText), dimSeed, testMaterial, err);
  check(gen != nullptr, "structures: placement generator loads");
  if (!gen) return;
  const StructureFeature* hut = findStructure(*gen, "hut");
  check(hut != nullptr, "structures: hut found");
  if (!hut) return;

  // basesInRect: the 4 interior cells of [0,64)^2 always contribute (density 1.0,
  // all hot-biome); border cells may or may not touch the rect.
  const auto bases = hut->basesInRect(dimSeed, 0, 0, 64, 64);
  check(bases.size() >= 4, "structures: interior cells roll bases at density 1.0");
  check(hut->basesInRect(dimSeed, 0, 0, 64, 64) == bases, "structures: basesInRect deterministic");
  // A different seed shuffles the bases.
  check(hut->basesInRect(dimensionSeed(8, "hut"), 0, 0, 64, 64) != bases,
        "structures: different seed -> different bases");
  // Empty rect -> empty.
  check(hut->basesInRect(dimSeed, 5, 5, 5, 5).empty(), "structures: empty rect -> no bases");

  // Chunk-local determinism: same chunk twice -> identical; margin replay agrees.
  const auto info = [&](int cx, int cz) {
    ChunkInfo c;
    c.cx = cx;
    c.cz = cz;
    c.size = 16;
    c.height = 128;
    c.dimensionSeed = dimSeed;
    c.chunkSeed = chunkSeed(dimSeed, cx, cz);
    return c;
  };
  const ColumnBuffer a = gen->generate(info(0, 0)), a2 = gen->generate(info(0, 0));
  check(hashBuffer(a) == hashBuffer(a2), "structures: same chunk twice -> identical");

  // Never-overwrite + attribution: strip the feature via JSON round-trip, compare buffers.
  std::string stripped;
  {
    litholattice::Json parsed;
    std::string jerr;
    check(litholattice::Json::parse(kStructureGeneratorText, parsed, &jerr), "structures: test JSON parses");
    for (auto& [k, v] : parsed.obj)
      if (k == "features") v = litholattice::Json::array();
    stripped = parsed.dump();
  }
  std::string err2;
  const auto genPlain = loadGenerator(stripped, dimSeed, testMaterial, err2);
  check(genPlain != nullptr, "structures: feature-stripped generator loads");
  if (!genPlain) return;

  std::vector<std::array<int32_t, 2>> area;
  for (int cz = -1; cz <= 1; ++cz)
    for (int cx = -1; cx <= 1; ++cx)
      for (const auto& b : hut->basesInRect(dimSeed, cx * 16 - 2, cz * 16 - 2, cx * 16 + 18, cz * 16 + 18))
        area.push_back(b);
  bool ok = true;
  int diffs = 0, huts = 0;
  for (int cz = -1; cz <= 1; ++cz)
    for (int cx = -1; cx <= 1; ++cx) {
      const ColumnBuffer with = gen->generate(info(cx, cz));
      const ColumnBuffer without = genPlain->generate(info(cx, cz));
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
          for (int y = 0; y < 128; ++y) {
            if (with.get(x, y, z) == without.get(x, y, z)) continue;
            ++diffs;
            if (without.get(x, y, z) != kAirMaterial) ok = false;  // never overwrite
            if (y <= 64) ok = false;                              // structures sit above the surface
            const int32_t wx = cx * 16 + x, wz = cz * 16 + z;
            bool near = false;
            for (const auto& b : area)
              if (std::abs(wx - b[0]) <= hut->reach() && std::abs(wz - b[1]) <= hut->reach()) {
                near = true;
                break;
              }
            if (!near) ok = false;  // unattributed block: out-of-chunk write
          }
      if (diffs > 0) ++huts;
    }
  check(diffs > 0, "structures: the hut actually placed blocks");
  check(ok, "structures: never-overwrite + above-surface + attributed to a base");
}

void structureChecks(Checker& check) {
  structureLoaderChecks(check);
  structurePlacementChecks(check);
}

// ---- cave types (worms, ravines, caverns)

/// Flat rock fixture (surface y=64) with code-built carvers; no biome selector, so a bound
/// biome probe always reads biome 0.
std::unique_ptr<const GeneratorPipeline> makeCarverPipeline(uint64_t dimSeed,
                                                            std::vector<std::unique_ptr<ICarver>> carvers) {
  GraphDesc desc;
  std::string err;
  if (!GraphDesc::parse(R"({"nodes": {"h": {"type": "constant", "value": 64}}, "outputs": {"height": "h"}})",
                        desc, err))
    return nullptr;
  auto graph = NoiseGraph::compile(desc, dimSeed, err);
  if (!graph) return nullptr;
  GeneratorPipeline::Builder b(graph);
  b.terrain(std::make_unique<HeightmapTerrain>(graph, graph->outputIndex("height"), MaterialId{1}, 0));
  for (auto& cv : carvers) b.carver(std::move(cv));
  return b.build();
}

constexpr float kCavePi = std::numbers::pi_v<float>;

/// Independent replica of WormCaveCarver for one source chunk: the exact carved cells in the
/// target chunk (chunk-local coords). Mirrors the seeded stream consumption exactly, so any
/// divergence (replay bug, stream shift, margin change) fails the comparison. Replica
/// settings must already satisfy the carver's clamps; the density table stays empty, exactly
/// like the carver treats it without a configured gate.
void replicaWorm(uint64_t dimSeed, int sx, int sz, const WormCaveSettings& st, int tcx, int tcz,
                 int yLimit, std::set<std::tuple<int, int, int>>& out) {
  const int n = 16;
  const float x0 = float(tcx * n), z0 = float(tcz * n);
  const auto carveSphere = [&](float px, float py, float pz, float r) {
    if (px + r < x0 || px - r >= x0 + float(n) || pz + r < z0 || pz - r >= z0 + float(n)) return;
    const int ylo = std::max(1, int(std::floor(py - r))), yhi = int(std::ceil(py + r));
    for (int lz = std::max(0, int(std::floor(pz - r - z0))); lz <= std::min(n - 1, int(std::ceil(pz + r - z0))); ++lz)
      for (int lx = std::max(0, int(std::floor(px - r - x0))); lx <= std::min(n - 1, int(std::ceil(px + r - x0))); ++lx) {
        const float dx = x0 + float(lx) + 0.5f - px, dz = z0 + float(lz) + 0.5f - pz;
        for (int y = ylo; y <= std::min(yhi, yLimit); ++y) {
          const float dy = float(y) + 0.5f - py;
          if (dx * dx + dy * dy + dz * dz < r * r) out.insert({lx, y, lz});
        }
      }
  };
  const auto walk = [&](Rng& w, float px, float py, float pz, float yaw, float pitch, int length,
                        float rMin, float rMax) {
    for (int step = 0; step < length; ++step) {
      const float t = float(step) / float(length);
      carveSphere(px, py, pz, rMin + (rMax - rMin) * std::sin(kCavePi * t));
      px += std::cos(yaw) * std::cos(pitch);
      pz += std::sin(yaw) * std::cos(pitch);
      py += std::sin(pitch);
      yaw += w.range(-0.25f, 0.25f);
      pitch = pitch * 0.85f + w.range(-0.12f, 0.12f);
    }
  };
  Rng rng(deriveSeed(chunkSeed(dimSeed, sx, sz), nameHash("worm_cave")));
  if (rng.uniform() >= st.chance) return;
  float px = float(sx * n) + rng.range(0.f, float(n));
  float pz = float(sz * n) + rng.range(0.f, float(n));
  float py = float(rng.range(st.minY, st.maxY));
  float yaw = rng.range(0.f, 2.f * kCavePi), pitch = rng.range(-0.3f, 0.3f);
  const int length = rng.range(st.minLength, st.maxLength);
  Rng walkStart = rng;  // pre-walk state, so branches can rewind to the walk start
  Rng walkRng = rng;
  walk(walkRng, px, py, pz, yaw, pitch, length, st.minRadius, st.maxRadius);
  if (st.branchChance > 0.f && st.maxBranches > 0) {
    int forks = 0;
    for (int b = 0; b < st.maxBranches; ++b) {
      if (rng.uniform() < st.branchChance) ++forks;
      else break;
    }
    for (int b = 0; b < forks; ++b) {
      const int forkStep = rng.range(0, length - 1);
      Rng w = walkStart;  // rewind to the parent's walk start, then fast-forward
      float bx = px, by = py, bz = pz, byaw = yaw, bpitch = pitch;
      for (int s2 = 0; s2 < forkStep; ++s2) {
        bx += std::cos(byaw) * std::cos(bpitch);
        bz += std::sin(byaw) * std::cos(bpitch);
        by += std::sin(bpitch);
        byaw += w.range(-0.25f, 0.25f);
        bpitch = bpitch * 0.85f + w.range(-0.12f, 0.12f);
      }
      const int blen = std::max(8, int(float(length) * st.branchLengthFrac));
      const float brMax = st.maxRadius * st.branchRadiusFrac;
      walk(w, bx, by, bz, byaw + w.range(-0.9f, 0.9f), bpitch + w.range(-0.3f, 0.3f), blen,
           std::min(st.minRadius, brMax), brMax);
    }
  }
}

/// One replicated chamber (center + radii), for the wobble property checks.
struct ReplicaChamber {
  float ox, oz, cy, radius, ry;
};

/// Independent replica of CavernCarver for one coarse cell. `gen` supplies biomeAt for the
/// density gate (null = no gate, exactly like the carver without a bound pipeline).
void replicaCavern(uint64_t dimSeed, int ccx, int ccz, const CavernSettings& st,
                   const GeneratorPipeline* gen, int tcx, int tcz, int yLimit,
                   std::set<std::tuple<int, int, int>>& out,
                   std::vector<ReplicaChamber>* records = nullptr) {
  const int n = 16, cell = st.cellSize;
  const uint64_t featSeed = deriveSeed(dimSeed, nameHash("cavern"));
  Rng r(deriveSeed(featSeed, mix64((uint64_t(uint32_t(ccx)) << 32) | uint64_t(uint32_t(ccz)))));
  const float ox = float(ccx * cell) + r.range(0.f, float(cell));
  const float oz = float(ccz * cell) + r.range(0.f, float(cell));
  const float radius = float(r.range(st.minRadius, st.maxRadius));
  float gate = st.chance;
  if (!st.density.empty() && gen) {
    const uint8_t biome = gen->biomeAt(int32_t(std::floor(ox)), int32_t(std::floor(oz)));
    gate = st.chance * (biome < st.density.size() ? st.density[biome] : 1.f);
  }
  if (r.uniform() >= gate) return;
  const float cy = float(r.range(st.minY, st.maxY));
  const float ry = radius * st.squashY;
  if (records) records->push_back({ox, oz, cy, radius, ry});
  if (st.coreFrac < 1.f) return;  // wobble rim: property-checked, not replicated
  const int x0 = tcx * n, z0 = tcz * n;
  const int lx0 = std::max(0, int(std::floor(ox - radius)) - x0);
  const int lx1 = std::min(n - 1, int(std::ceil(ox + radius)) - x0);
  const int lz0 = std::max(0, int(std::floor(oz - radius)) - z0);
  const int lz1 = std::min(n - 1, int(std::ceil(oz + radius)) - z0);
  const int ylo = std::max(1, int(std::floor(cy - ry))), yhi = int(std::ceil(cy + ry));
  for (int lz = lz0; lz <= lz1; ++lz)
    for (int lx = lx0; lx <= lx1; ++lx) {
      const float nx = (float(x0 + lx) + 0.5f - ox) / radius, nz = (float(z0 + lz) + 0.5f - oz) / radius;
      if (nx * nx + nz * nz >= 1.f) continue;
      for (int y = ylo; y <= std::min(yhi, yLimit); ++y) {
        const float ny = (float(y) + 0.5f - cy) / ry;
        if (nx * nx + ny * ny + nz * nz < 1.f) out.insert({lx, y, lz});
      }
    }
}

/// Independent replica of RavineCarver for one source chunk.
void replicaRavine(uint64_t dimSeed, int sx, int sz, const RavineSettings& st, int tcx, int tcz,
                   int surf, std::set<std::tuple<int, int, int>>& out) {
  const int n = 16;
  const float x0 = float(tcx * n), z0 = float(tcz * n);
  Rng rng(deriveSeed(chunkSeed(dimSeed, sx, sz), nameHash("ravine")));
  const float ox = float(sx * n) + rng.range(0.f, float(n));
  const float oz = float(sz * n) + rng.range(0.f, float(n));
  if (rng.uniform() >= st.chance) return;  // no density table in the replica settings
  float yaw = rng.range(0.f, 2.f * kCavePi);
  const int length = rng.range(st.minLength, st.maxLength);
  const float width = rng.range(st.minWidth, st.maxWidth);
  const float depth = rng.range(st.minDepth, st.maxDepth);
  float px = ox, pz = oz;
  for (int step = 0; step < length; ++step) {
    const float t = (float(step) + 0.5f) / float(length);
    const float taper = std::sin(kCavePi * t);
    const float dirX = std::cos(yaw), dirZ = std::sin(yaw);
    const float hw = width * std::sqrt(taper), cut = depth * taper;
    if (cut >= 1.f && hw >= 0.5f && !(px + hw < x0 || px - hw >= x0 + float(n) ||
                                      pz + hw < z0 || pz - hw >= z0 + float(n))) {
      const int lx0 = std::max(0, int(std::floor(px - hw - x0))), lx1 = std::min(n - 1, int(std::ceil(px + hw - x0)));
      const int lz0 = std::max(0, int(std::floor(pz - hw - z0))), lz1 = std::min(n - 1, int(std::ceil(pz + hw - z0)));
      for (int lz = lz0; lz <= lz1; ++lz)
        for (int lx = lx0; lx <= lx1; ++lx) {
          const float rx = x0 + float(lx) + 0.5f - px, rz = z0 + float(lz) + 0.5f - pz;
          const float d = std::fabs(rx * dirZ - rz * dirX);
          if (d >= hw) continue;
          const int cutDepth = int(std::floor(cut * (1.f - d / hw)));
          if (cutDepth < 1) continue;
          for (int y = std::max(st.bottomMargin, surf - cutDepth); y <= surf; ++y) out.insert({lx, y, lz});
        }
    }
    px += dirX;
    pz += dirZ;
    yaw += rng.range(-st.meander, st.meander);
  }
}

/// Carved-air cells in the flat (surface 64) fixture: any air at 1..yLimit was put there by
/// a carver (the fixture is solid rock 0..64).
std::set<std::tuple<int, int, int>> carvedSet(const ColumnBuffer& b, int yLimit) {
  std::set<std::tuple<int, int, int>> out;
  for (int z = 0; z < b.size(); ++z)
    for (int x = 0; x < b.size(); ++x)
      for (int y = 1; y <= yLimit; ++y)
        if (b.get(x, y, z) == kAirMaterial) out.insert({x, y, z});
  return out;
}

/// Solid blocks fully surrounded by air below the surface margin: floating debris. Convex
/// carves (worm spheres, cavern ellipsoids, ravine columns) cannot produce any; the cavern
/// wobble rim's cleanup sweep must dissolve the rest.
int countFloaters(const ColumnBuffer& b, int yLimit) {
  int n = 0;
  const int s = b.size(), h = b.height();
  constexpr int kD[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  for (int z = 0; z < s; ++z)
    for (int x = 0; x < s; ++x)
      for (int y = 2; y <= std::min(yLimit, h - 2); ++y) {
        if (b.get(x, y, z) == kAirMaterial) continue;
        bool floating = true;
        for (const auto& d : kD)
          if (b.get(x + d[0], y + d[1], z + d[2]) != kAirMaterial) {
            floating = false;
            break;
          }
        n += floating ? 1 : 0;
      }
  return n;
}

void wormCaveChecks(Checker& check) {
  const uint64_t dimSeed = dimensionSeed(4242, "caves");
  WormCaveSettings st;
  st.chance = 1.f;
  st.minLength = 48;
  st.maxLength = 48;
  st.minRadius = 2.f;
  st.maxRadius = 2.f;
  st.minY = 30;
  st.maxY = 30;
  st.surfaceMargin = 6;
  std::vector<std::unique_ptr<ICarver>> cv;
  cv.push_back(std::make_unique<WormCaveCarver>(st));
  const auto gen = makeCarverPipeline(dimSeed, std::move(cv));
  check(gen != nullptr, "caves: worm code-built pipeline builds");
  if (!gen) return;

  // Exact replica over 3x3 chunks: every carved cell matches, so cross-chunk replay lines up.
  const int reach = int(std::ceil((float(st.maxLength) + st.maxRadius) / 16.f));
  const int yLimit = 64 - st.surfaceMargin;
  bool exact = true, marginOk = true;
  long carvedTotal = 0;
  int floaters = 0;
  for (int cz = -1; cz <= 1; ++cz)
    for (int cx = -1; cx <= 1; ++cx) {
      ColumnMaps maps;
      const ColumnBuffer buf = gen->generate(treeChunk(cx, cz, dimSeed), &maps);
      for (size_t i = 0; i < 256; ++i) exact = exact && maps.height[i] == 64;
      const auto actual = carvedSet(buf, yLimit);
      carvedTotal += long(actual.size());
      std::set<std::tuple<int, int, int>> expected;
      for (int sz = cz - reach; sz <= cz + reach; ++sz)
        for (int sx = cx - reach; sx <= cx + reach; ++sx)
          replicaWorm(dimSeed, sx, sz, st, cx, cz, yLimit, expected);
      if (actual != expected) exact = false;
      for (int z = 0; z < 16 && marginOk; ++z)
        for (int x = 0; x < 16 && marginOk; ++x)
          for (int y = yLimit + 1; y <= 64; ++y)
            if (buf.get(x, y, z) == kAirMaterial) marginOk = false;
      floaters += countFloaters(buf, yLimit);
    }
  check(exact, "caves: 3x3 worm chunks match the independent replica exactly (cross-chunk replay)");
  check(carvedTotal > 1000, "caves: legacy worms carve real tunnels (" + std::to_string(carvedTotal) + " blocks)");
  check(marginOk, "caves: worms respect the surface margin (nothing carved in the top 6)");
  check(floaters == 0, "caves: no floating debris from worm spheres");

  // Determinism: same chunk twice identical; another seed differs.
  const uint64_t h1 = hashBuffer(gen->generate(treeChunk(5, -7, dimSeed)));
  const uint64_t h2 = hashBuffer(gen->generate(treeChunk(5, -7, dimSeed)));
  std::vector<std::unique_ptr<ICarver>> cv2;
  cv2.push_back(std::make_unique<WormCaveCarver>(st));
  const auto genOther = makeCarverPipeline(dimensionSeed(4243, "caves"), std::move(cv2));
  const uint64_t h3 = genOther ? hashBuffer(genOther->generate(treeChunk(5, -7, dimensionSeed(4243, "caves")))) : 0;
  check(h1 == h2, "caves: the same worm chunk regenerates identically");
  check(genOther && h1 != h3, "caves: a different seed walks different worms");

  // Branches: opt-in forks, still exactly replicable, strictly more tunnel.
  WormCaveSettings bst = st;
  bst.branchChance = 1.f;
  bst.maxBranches = 1;
  std::vector<std::unique_ptr<ICarver>> cv3;
  cv3.push_back(std::make_unique<WormCaveCarver>(bst));
  const auto genB = makeCarverPipeline(dimSeed, std::move(cv3));
  check(genB != nullptr, "caves: branched worm pipeline builds");
  if (genB) {
    bool branchExact = true;
    long branchTotal = 0;
    for (int cz = -1; cz <= 1; ++cz)
      for (int cx = -1; cx <= 1; ++cx) {
        const ColumnBuffer buf = genB->generate(treeChunk(cx, cz, dimSeed));
        const auto actual = carvedSet(buf, yLimit);
        branchTotal += long(actual.size());
        std::set<std::tuple<int, int, int>> expected;
        for (int sz = cz - reach; sz <= cz + reach; ++sz)
          for (int sx = cx - reach; sx <= cx + reach; ++sx)
            replicaWorm(dimSeed, sx, sz, bst, cx, cz, yLimit, expected);
        if (actual != expected) branchExact = false;
      }
    check(branchExact, "caves: branched worms match the replica exactly (forks land on the parent path)");
    check(branchTotal > carvedTotal,
          "caves: branches add tunnel (" + std::to_string(branchTotal) + " > " + std::to_string(carvedTotal) + ")");
    // Branch connectivity: replicate source chunk (0,0)'s parent walk and fork starts
    // directly; every fork must start exactly on the parent's path.
    {
      Rng rng(deriveSeed(chunkSeed(dimSeed, 0, 0), nameHash("worm_cave")));
      bool connected = false;
      if (rng.uniform() < bst.chance) {
        float px = rng.range(0.f, 16.f), pz = rng.range(0.f, 16.f);
        float py = float(rng.range(bst.minY, bst.maxY));
        float yaw = rng.range(0.f, 2.f * kCavePi), pitch = rng.range(-0.3f, 0.3f);
        const int length = rng.range(bst.minLength, bst.maxLength);
        Rng walkStart = rng;
        std::vector<std::array<float, 3>> path;
        {
          Rng w = walkStart;
          float qx = px, qy = py, qz = pz, qyaw = yaw, qpitch = pitch;
          for (int step = 0; step < length; ++step) {
            path.push_back({qx, qy, qz});
            qx += std::cos(qyaw) * std::cos(qpitch);
            qz += std::sin(qyaw) * std::cos(qpitch);
            qy += std::sin(qpitch);
            qyaw += w.range(-0.25f, 0.25f);
            qpitch = qpitch * 0.85f + w.range(-0.12f, 0.12f);
          }
        }
        int forks = 0;
        for (int b = 0; b < bst.maxBranches; ++b) {
          if (rng.uniform() < bst.branchChance) ++forks;
          else break;
        }
        connected = forks > 0;
        for (int b = 0; b < forks; ++b) {
          const int forkStep = rng.range(0, length - 1);
          Rng w = walkStart;
          float bx = px, by = py, bz = pz, byaw = yaw, bpitch = pitch;
          for (int s2 = 0; s2 < forkStep; ++s2) {
            bx += std::cos(byaw) * std::cos(bpitch);
            bz += std::sin(byaw) * std::cos(bpitch);
            by += std::sin(bpitch);
            byaw += w.range(-0.25f, 0.25f);
            bpitch = bpitch * 0.85f + w.range(-0.12f, 0.12f);
          }
          const auto& p = path[size_t(forkStep)];
          const float dx = bx - p[0], dy = by - p[1], dz = bz - p[2];
          connected = connected && dx * dx + dy * dy + dz * dz < 1e-6f;
        }
      }
      check(connected, "caves: every branch starts exactly on its parent's path");
    }
  }
}

void cavernChecks(Checker& check) {
  const uint64_t dimSeed = dimensionSeed(5150, "caverns");
  CavernSettings st;
  st.cellSize = 32;
  st.chance = 1.f;
  st.minRadius = 8;
  st.maxRadius = 8;
  st.squashY = 0.5f;
  st.minY = 32;
  st.maxY = 32;
  st.surfaceMargin = 6;
  st.coreFrac = 1.f;  // pure ellipsoid: exactly replicable
  std::vector<std::unique_ptr<ICarver>> cv;
  cv.push_back(std::make_unique<CavernCarver>(st));
  const auto gen = makeCarverPipeline(dimSeed, std::move(cv));
  check(gen != nullptr, "caves: cavern code-built pipeline builds");
  if (!gen) return;

  const int yLimit = 64 - st.surfaceMargin;
  bool exact = true, marginOk = true;
  long carvedTotal = 0;
  int floaters = 0, chambers = 0;
  for (int cz = -1; cz <= 1; ++cz)
    for (int cx = -1; cx <= 1; ++cx) {
      ColumnMaps maps;
      const ColumnBuffer buf = gen->generate(treeChunk(cx, cz, dimSeed), &maps);
      const auto actual = carvedSet(buf, yLimit);
      carvedTotal += long(actual.size());
      std::set<std::tuple<int, int, int>> expected;
      std::vector<ReplicaChamber> records;
      const int x0 = cx * 16, z0 = cz * 16;
      const int ccx0 = (x0 - st.maxRadius) / st.cellSize - 1, ccx1 = (x0 + 15 + st.maxRadius) / st.cellSize + 1;
      const int ccz0 = (z0 - st.maxRadius) / st.cellSize - 1, ccz1 = (z0 + 15 + st.maxRadius) / st.cellSize + 1;
      for (int ccz = ccz0; ccz <= ccz1; ++ccz)
        for (int ccx = ccx0; ccx <= ccx1; ++ccx)
          replicaCavern(dimSeed, ccx, ccz, st, gen.get(), cx, cz, yLimit, expected, &records);
      chambers += int(records.size());
      if (actual != expected) exact = false;
      for (int z = 0; z < 16 && marginOk; ++z)
        for (int x = 0; x < 16 && marginOk; ++x)
          for (int y = yLimit + 1; y <= 64; ++y)
            if (buf.get(x, y, z) == kAirMaterial) marginOk = false;
      floaters += countFloaters(buf, yLimit);
    }
  check(exact, "caves: 3x3 cavern chunks match the independent replica exactly (chambers line up)");
  check(chambers > 0 && carvedTotal > 2000,
        "caves: chambers carved (" + std::to_string(chambers) + " replicated, " + std::to_string(carvedTotal) +
            " blocks)");
  check(marginOk, "caves: caverns respect the surface margin (nothing carved in the top 6)");
  check(floaters == 0, "caves: no floating debris from cavern ellipsoids");
  const uint64_t h1 = hashBuffer(gen->generate(treeChunk(5, -7, dimSeed)));
  check(h1 == hashBuffer(gen->generate(treeChunk(5, -7, dimSeed))), "caves: the same cavern chunk regenerates identically");

  // Wobble rim: carved cells stay inside the ellipsoid, the core is always carved.
  CavernSettings wst = st;
  wst.coreFrac = 0.5f;
  wst.noiseFreq = 0.125f;
  std::vector<std::unique_ptr<ICarver>> cv2;
  cv2.push_back(std::make_unique<CavernCarver>(wst));
  const auto genW = makeCarverPipeline(dimSeed, std::move(cv2));
  check(genW != nullptr, "caves: wobble-rim cavern pipeline builds");
  if (genW) {
    bool inside = true, coreKept = true;
    int wobFloaters = 0;
    long wobTotal = 0;
    for (int cz = 0; cz < 2; ++cz)
      for (int cx = 0; cx < 2; ++cx) {
        const ColumnBuffer buf = genW->generate(treeChunk(cx, cz, dimSeed));
        const auto actual = carvedSet(buf, yLimit);
        wobTotal += long(actual.size());
        std::set<std::tuple<int, int, int>> dummy;
        std::vector<ReplicaChamber> records;
        const int x0 = cx * 16, z0 = cz * 16;
        const int ccx0 = (x0 - wst.maxRadius) / wst.cellSize - 1, ccx1 = (x0 + 15 + wst.maxRadius) / wst.cellSize + 1;
        const int ccz0 = (z0 - wst.maxRadius) / wst.cellSize - 1, ccz1 = (z0 + 15 + wst.maxRadius) / wst.cellSize + 1;
        for (int ccz = ccz0; ccz <= ccz1; ++ccz)
          for (int ccx = ccx0; ccx <= ccx1; ++ccx)
            replicaCavern(dimSeed, ccx, ccz, wst, genW.get(), cx, cz, yLimit, dummy, &records);
        const float core2 = wst.coreFrac * wst.coreFrac;
        for (const auto& [lx, y, lz] : actual) {
          const float wx = float(x0 + lx) + 0.5f, wz = float(z0 + lz) + 0.5f;
          bool inAny = false;
          for (const auto& ch : records) {
            const float nx = (wx - ch.ox) / ch.radius, nz = (wz - ch.oz) / ch.radius;
            const float ny = (float(y) + 0.5f - ch.cy) / ch.ry;
            if (nx * nx + ny * ny + nz * nz < 1.f) {
              inAny = true;
              break;
            }
          }
          inside = inside && inAny;
        }
        // Core cells (d2 < core2) of every chamber must be carved.
        for (const auto& ch : records) {
          for (int lz = 0; lz < 16 && coreKept; ++lz)
            for (int lx = 0; lx < 16 && coreKept; ++lx) {
              const float wx = float(x0 + lx) + 0.5f, wz = float(z0 + lz) + 0.5f;
              const float nx = (wx - ch.ox) / ch.radius, nz = (wz - ch.oz) / ch.radius;
              if (nx * nx + nz * nz >= core2) continue;
              for (int y = std::max(1, int(std::floor(ch.cy - ch.ry))); y <= std::min(yLimit, int(std::ceil(ch.cy + ch.ry))); ++y) {
                const float ny = (float(y) + 0.5f - ch.cy) / ch.ry;
                if (nx * nx + ny * ny + nz * nz < core2 && buf.get(lx, y, lz) != kAirMaterial)
                  coreKept = false;
              }
            }
        }
        wobFloaters += countFloaters(buf, yLimit);
      }
    check(inside, "caves: wobble-rim carving never leaves its chamber ellipsoid");
    check(coreKept, "caves: the wobble rim never eats the guaranteed core");
    check(wobFloaters == 0, "caves: the cleanup sweep dissolves wobble-rim floaters");
    check(wobTotal != carvedTotal, "caves: the wobble rim changes the carve vs the pure ellipsoid");
  }
}

void ravineChecks(Checker& check) {
  const uint64_t dimSeed = dimensionSeed(6100, "ravines");
  RavineSettings st;
  st.chance = 1.f;
  st.minLength = 48;
  st.maxLength = 48;
  st.minWidth = 6.f;
  st.maxWidth = 6.f;
  st.minDepth = 24.f;
  st.maxDepth = 24.f;
  st.meander = 0.05f;
  st.bottomMargin = 4;
  std::vector<std::unique_ptr<ICarver>> cv;
  cv.push_back(std::make_unique<RavineCarver>(st));
  const auto gen = makeCarverPipeline(dimSeed, std::move(cv));
  check(gen != nullptr, "caves: ravine code-built pipeline builds");
  if (!gen) return;

  const int reach = (st.maxLength + int(std::ceil(st.maxWidth)) + 16 - 1) / 16;
  bool exact = true, floorOk = true, opensAtSurface = false;
  long carvedTotal = 0;
  int maxCut = 0, minCut = 1000, floaters = 0;
  for (int cz = -1; cz <= 1; ++cz)
    for (int cx = -1; cx <= 1; ++cx) {
      const ColumnBuffer buf = gen->generate(treeChunk(cx, cz, dimSeed));
      const auto actual = carvedSet(buf, 64);
      carvedTotal += long(actual.size());
      std::set<std::tuple<int, int, int>> expected;
      for (int sz = cz - reach; sz <= cz + reach; ++sz)
        for (int sx = cx - reach; sx <= cx + reach; ++sx)
          replicaRavine(dimSeed, sx, sz, st, cx, cz, 64, expected);
      if (actual != expected) exact = false;
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
          int cut = 0;
          for (int y = st.bottomMargin; y <= 64; ++y) {
            if (buf.get(x, y, z) != kAirMaterial) continue;
            ++cut;
            if (y == 64) opensAtSurface = true;
          }
          if (cut > 0) {
            maxCut = std::max(maxCut, cut);
            minCut = std::min(minCut, cut);
          }
          for (int y = 1; y < st.bottomMargin; ++y)
            if (buf.get(x, y, z) == kAirMaterial) floorOk = false;
        }
      floaters += countFloaters(buf, 64);
    }
  check(exact, "caves: 3x3 ravine chunks match the independent replica exactly (gashes line up)");
  check(carvedTotal > 2000, "caves: ravines cut real gashes (" + std::to_string(carvedTotal) + " blocks)");
  check(floorOk, "caves: ravines never carve below the bottom margin");
  check(opensAtSurface, "caves: ravine gashes open at the surface");
  check(maxCut >= 20 && minCut <= 4,
        "caves: sloped walls (deep centerline cut " + std::to_string(maxCut) + ", feathered rim " +
            std::to_string(minCut) + ")");
  check(floaters == 0, "caves: no floating debris from ravine cuts");
  const uint64_t h1 = hashBuffer(gen->generate(treeChunk(5, -7, dimSeed)));
  check(h1 == hashBuffer(gen->generate(treeChunk(5, -7, dimSeed))), "caves: the same ravine chunk regenerates identically");
}

constexpr const char* kCaveBiomeText = R"({
  "graph": {"nodes": {
      "t": {"type": "scale_bias", "scale": 0.02, "bias": 0.5, "input": {"type": "coordinate", "axis": "x"}},
      "h": {"type": "constant", "value": 64}},
    "outputs": {"temperature": "t", "height": "h"}},
  "terrain": {"type": "heightmap", "output": "height", "material": "rock"},
  "climate": {"temperature": "temperature"},
  "biomes": [{"name": "west", "climate": {"temperature": 0.2}},
             {"name": "east", "climate": {"temperature": 0.8}}],
  "carvers": [{"type": "cavern", "cell_size": 32, "chance": 1.0, "min_radius": 6, "max_radius": 6,
               "squash_y": 0.5, "min_y": 32, "max_y": 32, "surface_margin": 6, "core_frac": 1.0,
               "density": {"west": 1.0, "east": 0.0}}]
})";

void caveLoaderChecks(Checker& check) {
  std::string err;
  const auto gen = loadGenerator(std::string(kCaveBiomeText), dimensionSeed(7, "cavebiome"), testMaterial, err);
  check(gen != nullptr, "caves: cavern JSON with per-biome density loads" +
                            (err.empty() ? std::string() : " (" + err + ")"));
  if (!gen) return;
  // The density gate is a pure function of the chamber center: replicate it through the
  // pipeline's own biomeAt and compare exactly, then prove every chamber stands in "west".
  bool exact = true, allWest = true;
  int chambers = 0, carvedTotal = 0;
  for (int cx = -2; cx < 2; ++cx) {
    ColumnMaps maps;
    const ColumnBuffer buf = gen->generate(treeChunk(cx, 0, dimensionSeed(7, "cavebiome")), &maps);
    const auto actual = carvedSet(buf, 58);
    carvedTotal += int(actual.size());
    std::set<std::tuple<int, int, int>> expected;
    std::vector<ReplicaChamber> records;
    CavernSettings st;
    st.cellSize = 32;
    st.chance = 1.f;
    st.minRadius = 6;
    st.maxRadius = 6;
    st.squashY = 0.5f;
    st.minY = 32;
    st.maxY = 32;
    st.surfaceMargin = 6;
    st.coreFrac = 1.f;
    st.density = {1.f, 0.f};
    const int x0 = cx * 16;
    for (int ccz = -2; ccz <= 2; ++ccz)
      for (int ccx = (x0 - 6) / 32 - 1; ccx <= (x0 + 15 + 6) / 32 + 1; ++ccx)
        replicaCavern(dimensionSeed(7, "cavebiome"), ccx, ccz, st, gen.get(), cx, 0, 58, expected, &records);
    chambers += int(records.size());
    if (actual != expected) exact = false;
    for (const auto& ch : records)
      if (gen->biomeAt(int32_t(std::floor(ch.ox)), int32_t(std::floor(ch.oz))) != 0) allWest = false;
  }
  check(exact, "caves: JSON cavern matches the replica through the biome-gated path");
  check(allWest && chambers > 0,
        "caves: per-biome density holds (zero-density biome grows no chambers, " + std::to_string(chambers) +
            " chambers all western)");
  check(carvedTotal > 500, "caves: gated caverns carve (" + std::to_string(carvedTotal) + " blocks)");

  // Loader rejections and the configured ravine/worm forms.
  const std::string head = R"({
    "graph": {"nodes": {"h": {"type": "constant", "value": 64}}, "outputs": {"height": "h"}},
    "terrain": {"type": "heightmap", "output": "height", "material": "rock"},
    "biomes": [{"name": "west"}, {"name": "east"}],
    "carvers": [)";
  const auto bad = [&](const char* what, const std::string& carverJson) {
    std::string e;
    const bool rejected = !loadGenerator(head + carverJson + "]})", dimensionSeed(7, "cavebiome"), testMaterial, e);
    check(rejected, std::string("caves: rejected (") + what + "): " + e);
  };
  bad("unknown carver type", R"({"type": "nope"})");
  bad("cavern density names unknown biome", R"({"type": "cavern", "density": {"nope": 1.0}})");
  bad("ravine density names unknown biome", R"({"type": "ravine", "density": {"nope": 1.0}})");
  bad("worm density names unknown biome", R"({"type": "worm_cave", "density": {"nope": 1.0}})");
  std::string okErr;
  const bool ravineOk = loadGenerator(
      head + R"({"type": "ravine", "chance": 1.0, "min_length": 48, "max_length": 48, "min_width": 6, "max_width": 6,)"
             R"( "min_depth": 24, "max_depth": 24, "meander": 0.05, "bottom_margin": 4}]})",
      dimensionSeed(7, "cavebiome"), testMaterial, okErr) != nullptr;
  check(ravineOk, "caves: configured ravine JSON loads" + (okErr.empty() ? "" : " (" + okErr + ")"));
  okErr.clear();
  const bool wormOk = loadGenerator(
      head + R"({"type": "worm_cave", "chance": 1.0, "branch_chance": 0.5, "max_branches": 2,)"
             R"( "branch_length_frac": 0.5, "branch_radius_frac": 0.75, "density": {"west": 2.0}}]})",
      dimensionSeed(7, "cavebiome"), testMaterial, okErr) != nullptr;
  check(wormOk, "caves: worm JSON with branch fields and density loads" + (okErr.empty() ? "" : " (" + okErr + ")"));
}

// ---- noise-field carvers (cheese + spaghetti/noodle, 2026-10-13)

/// Assembled carved-cell grid over a 2x2-chunk region of the flat-rock fixture
/// (surface y=64), restricted to the carve band [bandLo, bandHi].
struct RegionCarve {
  std::set<std::tuple<int, int, int>> cells;  // (world x, y, world z)
  int columns = 0;                            // columns in the region
  int pillarColumns = 0;                      // columns with no carved cell in the band
};

RegionCarve regionCarve(const GeneratorPipeline& gen, uint64_t dimSeed, int bandLo, int bandHi) {
  RegionCarve out;
  for (int cz = 0; cz < 2; ++cz)
    for (int cx = 0; cx < 2; ++cx) {
      const ColumnBuffer buf = gen.generate(treeChunk(cx, cz, dimSeed));
      for (int lz = 0; lz < 16; ++lz)
        for (int lx = 0; lx < 16; ++lx) {
          ++out.columns;
          bool any = false;
          for (int y = bandLo; y <= bandHi; ++y)
            if (buf.get(lx, y, lz) == kAirMaterial) {
              out.cells.insert({cx * 16 + lx, y, cz * 16 + lz});
              any = true;
            }
          if (!any) ++out.pillarColumns;
        }
    }
  return out;
}

/// True bounded-thickness measure: the largest Chebyshev distance from any carved
/// cell to the nearest non-carved cell. A tube of radius r blocks has interior depth
/// <= r + 1; a blob or cavern has deep interior cells. (An axis-run measure would be
/// wrong here: a tube running parallel to an axis has long runs along its length.)
int maxInteriorDepth(const std::set<std::tuple<int, int, int>>& cells) {
  int best = 0;
  for (const auto& [x, y, z] : cells) {
    int d = 0;
    for (;;) {
      const int r = d + 1;
      bool shellAllCarved = true;
      for (int dx = -r; dx <= r && shellAllCarved; ++dx)
        for (int dy = -r; dy <= r && shellAllCarved; ++dy)
          for (int dz = -r; dz <= r; ++dz) {
            if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != r) continue;
            if (!cells.count({x + dx, y + dy, z + dz})) {
              shellAllCarved = false;
              break;
            }
          }
      if (!shellAllCarved) break;
      d = r;
      if (d > 16) break;  // safety: not a tube at all
    }
    best = std::max(best, d + 1);  // distance to the nearest non-carved cell
  }
  return best;
}

void noiseFieldCarverChecks(Checker& check) {
  const int bandLo = 6, bandHi = 56;  // fixture surface 64, margin 8 -> top 56
  // ---- cheese: carved fraction in a sane band, pillars remain, margin respected
  {
    const uint64_t dimSeed = dimensionSeed(777, "cheese");
    std::vector<std::unique_ptr<ICarver>> cv;
    cv.push_back(std::make_unique<CheeseCarver>(CheeseSettings{}));
    const auto gen = makeCarverPipeline(dimSeed, std::move(cv));
    check(gen != nullptr, "cheese: pipeline builds");
    if (gen) {
      const RegionCarve rc = regionCarve(*gen, dimSeed, bandLo, bandHi);
      const double frac = double(rc.cells.size()) / double(rc.columns * (bandHi - bandLo + 1));
      check(frac > 0.03 && frac < 0.65,
            "cheese: carved fraction in a sane band (" + std::to_string(rc.cells.size()) + " cells, " +
                std::to_string(frac) + ")");
      check(rc.pillarColumns > 100,
            "cheese: uncarved pillars remain (" + std::to_string(rc.pillarColumns) + " of " +
                std::to_string(rc.columns) + " columns untouched in the band)");
      bool marginOk = true;
      const ColumnBuffer buf = gen->generate(treeChunk(0, 0, dimSeed));
      for (int lz = 0; lz < 16; ++lz)
        for (int lx = 0; lx < 16; ++lx)
          for (int y = bandHi + 1; y <= 64; ++y)
            if (buf.get(lx, y, lz) == kAirMaterial) marginOk = false;
      check(marginOk, "cheese: surface margin respected (nothing carved in the top 8 below the surface)");
      const uint64_t h1 = hashBuffer(gen->generate(treeChunk(5, -7, dimSeed)));
      check(h1 == hashBuffer(gen->generate(treeChunk(5, -7, dimSeed))),
            "cheese: same chunk regenerates identically");
      std::vector<std::unique_ptr<ICarver>> cv2;
      cv2.push_back(std::make_unique<CheeseCarver>(CheeseSettings{}));
      const auto genOther = makeCarverPipeline(dimensionSeed(778, "cheese"), std::move(cv2));
      check(genOther && h1 != hashBuffer(genOther->generate(treeChunk(5, -7, dimensionSeed(778, "cheese")))),
            "cheese: different seed -> different field");
    }
  }
  // ---- spaghetti / noodle: thin connected tubes (bounded thickness, no isolates)
  const auto tubeStats = [&](bool noodle, uint64_t dimSeed) {
    std::vector<std::unique_ptr<ICarver>> cv;
    if (noodle) cv.push_back(std::make_unique<NoodleCarver>(NoodleCarver().settings()));
    else cv.push_back(std::make_unique<SpaghettiCarver>(SpaghettiCarver().settings()));
    const auto gen = makeCarverPipeline(dimSeed, std::move(cv));
    if (!gen) return std::tuple<long, int, double, uint64_t, int>{-1, 0, 1.0, 0, 0};
    const RegionCarve rc = regionCarve(*gen, dimSeed, bandLo, bandHi);
    long isolated = 0;
    for (const auto& [x, y, z] : rc.cells) {
      constexpr int kD[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
      bool hasNeighbour = false;
      for (const auto& d : kD)
        if (rc.cells.count({x + d[0], y + d[1], z + d[2]})) {
          hasNeighbour = true;
          break;
        }
      if (!hasNeighbour) ++isolated;
    }
    const double isoFrac = rc.cells.empty() ? 1.0 : double(isolated) / double(rc.cells.size());
    // Filled-core witness: the fullest 3x3x3 neighbourhood around any carved cell.
    int maxCore = 0;
    for (const auto& [x, y, z] : rc.cells) {
      int cnt = 0;
      for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
          for (int dz = -1; dz <= 1; ++dz)
            if (rc.cells.count({x + dx, y + dy, z + dz})) ++cnt;
      maxCore = std::max(maxCore, cnt);
    }
    const uint64_t h = hashBuffer(gen->generate(treeChunk(5, -7, dimSeed)));
    return std::tuple<long, int, double, uint64_t, int>{long(rc.cells.size()), maxInteriorDepth(rc.cells), isoFrac, h,
                                                        maxCore};
  };
  long spaghettiCount = 0;
  int spaghettiCore = 0;
  {
    const auto [count, depth, iso, h, core] = tubeStats(false, dimensionSeed(888, "spaghetti"));
    spaghettiCount = count;
    spaghettiCore = core;
    check(count > 200, "spaghetti: tubes carved (" + std::to_string(count) + " cells in the band)");
    // Bounded thickness: no carved cell is more than 2 blocks (Chebyshev) from solid.
    // A ~3-block round tube scores 1 (its centre cell's cube corners are always
    // solid); a blob/cavern interior scores 4+. The filled-core witness (>= 20 of 27
    // neighbours carved somewhere) proves the tubes are filled, not surface shells.
    check(depth >= 1 && depth <= 2,
          "spaghetti: bounded thickness (max interior depth " + std::to_string(depth) +
              " blocks to solid, a tube not a blob)");
    check(core >= 20, "spaghetti: tubes are filled (fullest 3x3x3 core has " + std::to_string(core) + " of 27 carved)");
    check(iso < 0.05, "spaghetti: carved cells form connected tubes (isolated fraction " + std::to_string(iso) + ")");
    const auto [count2, depth2, iso2, h2, core2] = tubeStats(false, dimensionSeed(888, "spaghetti"));
    (void)depth2;
    (void)iso2;
    (void)core2;
    check(count == count2 && h == h2, "spaghetti: same seed regenerates identically");
    const auto [countOther, dO, iO, hOther, cO] = tubeStats(false, dimensionSeed(889, "spaghetti"));
    (void)countOther;
    (void)dO;
    (void)iO;
    (void)cO;
    check(h != hOther, "spaghetti: different seed -> different tubes");
  }
  {
    const auto [count, depth, iso, h, core] = tubeStats(true, dimensionSeed(999, "noodle"));
    (void)h;
    check(count > 50, "noodle: tubes carved (" + std::to_string(count) + " cells in the band)");
    check(depth >= 1 && depth <= 2,
          "noodle: bounded thickness (max interior depth " + std::to_string(depth) + ")");
    check(count < spaghettiCount && core <= spaghettiCore,
          "noodle: thinner than spaghetti (cells " + std::to_string(count) + " < " + std::to_string(spaghettiCount) +
              ", fullest core " + std::to_string(core) + " <= " + std::to_string(spaghettiCore) + ")");
    check(iso < 0.05, "noodle: carved cells form connected tubes (isolated fraction " + std::to_string(iso) + ")");
  }
  // ---- loader: all three are real, selectable carver types
  {
    std::string err;
    const std::string text =
        R"({"graph": {"nodes": {"h": {"type": "constant", "value": 64}}, "outputs": {"height": "h"}},)"
        R"("terrain": {"output": "height", "material": "rock"},)"
        R"("biomes": [{"name": "low"}],)"
        R"("carvers": [{"type": "cheese", "threshold": 0.25}, {"type": "spaghetti"},)"
        R"( {"type": "noodle", "tube_radius": 0.07}]})";
    const auto gen = loadGenerator(text, dimensionSeed(7, "fieldloader"), testMaterial, err);
    check(gen != nullptr, "cheese/spaghetti/noodle: JSON carvers load" +
                              (err.empty() ? std::string() : " (" + err + ")"));
  }
}

void ruinsOpenChecks(Checker& check) {
  const uint64_t dimSeed = dimensionSeed(4242, "ruinsopen");
  const auto settings = [](bool open) {
    RuinsSettings s;
    s.trace = MaterialId{5};
    s.via = MaterialId{6};
    s.crust = {MaterialId{1}};
    s.depthMin = 10;
    s.depthMax = 20;
    s.surfaceMargin = 6;
    s.openMode = open;
    s.rubble = MaterialId{9};
    s.openRoomsPerChunk = 3;
    s.openMinSize = 4;
    s.openMaxSize = 6;
    s.openHeight = 3;
    s.rubbleChance = 0.5f;
    return s;
  };
  const auto build = [&](bool open) {
    std::vector<std::unique_ptr<ICarver>> cv;
    cv.push_back(std::make_unique<RuinsCarver>(settings(open)));
    return makeCarverPipeline(dimSeed, std::move(cv));
  };
  const auto genDefault = build(false);
  const auto genOpen = build(true);
  check(genDefault && genOpen, "ruins open: pipelines build (default + open mode)");
  if (!genDefault || !genOpen) return;

  // Default mode is the pre-change inlay carver: hash-pinned (golden value pinned
  // from the 2026-10-13 run of this test against the unmodified inlay path; the
  // open pass draws no RNG and stamps nothing while openMode is off).
  const uint64_t hDefault = hashBuffer(genDefault->generate(treeChunk(3, -2, dimSeed)));
  std::printf("    ruins default-mode hash: %llx\n", (unsigned long long)hDefault);
  check(hDefault == 0x8d0168dd575353a1ull,
        "ruins open: default mode hash equals the pre-open-mode inlay golden (pinned 2026-10-13)");
  check(hDefault == hashBuffer(build(false)->generate(treeChunk(3, -2, dimSeed))),
        "ruins open: default mode regenerates identically across pipelines");

  long airDefault = 0, airOpen = 0, rubbleOpen = 0;
  bool breach = false;
  for (int cz = 0; cz < 2; ++cz)
    for (int cx = 0; cx < 2; ++cx) {
      ColumnMaps mD, mO;
      const ColumnBuffer bD = genDefault->generate(treeChunk(cx, cz, dimSeed), &mD);
      const ColumnBuffer bO = genOpen->generate(treeChunk(cx, cz, dimSeed), &mO);
      for (int lz = 0; lz < 16; ++lz)
        for (int lx = 0; lx < 16; ++lx) {
          const int surf = mO.height[size_t(lz * 16 + lx)];
          for (int y = 2; y <= surf - 6; ++y) {
            if (bD.get(lx, y, lz) == kAirMaterial) ++airDefault;
            if (bO.get(lx, y, lz) == kAirMaterial) ++airOpen;
            if (bO.get(lx, y, lz) == MaterialId{9}) ++rubbleOpen;
          }
          for (int y = surf - 5; y <= surf; ++y)
            if (bO.get(lx, y, lz) == kAirMaterial || bO.get(lx, y, lz) == MaterialId{9}) breach = true;
        }
    }
  check(airOpen > airDefault + 100,
        "ruins open: open mode creates real air volume (" + std::to_string(airOpen) + " buried air vs " +
            std::to_string(airDefault) + " inlay-only)");
  check(rubbleOpen > 0, "ruins open: rubble scattered on the new floors (" + std::to_string(rubbleOpen) + " cells)");
  check(!breach, "ruins open: open volumes stay buried (surface margin respected, no rubble at the surface)");
  check(hashBuffer(genOpen->generate(treeChunk(3, -2, dimSeed))) != hDefault,
        "ruins open: open mode changes the chunk vs default mode");
  // Loader: open mode is data.
  std::string err;
  const std::string text =
      R"({"graph": {"nodes": {"h": {"type": "constant", "value": 64}}, "outputs": {"height": "h"}},)"
      R"("terrain": {"output": "height", "material": "rock"},)"
      R"("biomes": [{"name": "low"}],)"
      R"("carvers": [{"type": "ruins", "trace": "trace", "via": "via", "crust": ["rock"],)"
      R"( "open_mode": true, "rubble": "gravel", "open_rooms_per_chunk": 2}]})";
  check(loadGenerator(text, dimensionSeed(7, "ruinsopen"), testMaterial, err) != nullptr,
        "ruins open: JSON open_mode loads" + (err.empty() ? std::string() : " (" + err + ")"));
}

void caveChecks(Checker& check) {
  wormCaveChecks(check);
  cavernChecks(check);
  ravineChecks(check);
  caveLoaderChecks(check);
  noiseFieldCarverChecks(check);
  ruinsOpenChecks(check);
}

// ---- mountains (mountain_range / peak nodes, per-biome uplift) ----

/// Mountain generator JSON: blended climate -> plains / ranges / peaks biomes. The ranges
/// biome adds a mountain_range uplift (ridged fbm gated by a foothill mask), the peaks biome
/// a peak uplift (cones over a cellular-F1 distance field). The terrain entry list is a
/// parameter so tests can rewire the uplift (amplitude, off) without touching the graph.
std::string mountainText(const char* terrainBiomes) {
  std::string text = R"JSON({
  "graph": {"nodes": {
      "t": {"type": "scale_bias", "scale": 0.02, "bias": 0.64,
            "input": {"type": "coordinate", "axis": "x"}},
      "base": {"type": "constant", "value": 60},
      "ridges": {"type": "noise", "noise": "opensimplex2s", "frequency": 0.02,
                 "fractal": {"type": "ridged", "octaves": 4}},
      "mask": {"type": "scale_bias", "scale": 0.5, "bias": 0.5,
               "input": {"type": "noise", "noise": "opensimplex2s", "frequency": 0.004}},
      "range_up": {"type": "mountain_range", "amplitude": 1, "power": 1.5,
                   "mask_low": 0.35, "mask_high": 0.7, "ridges": "ridges", "mask": "mask"},
      "dist": {"type": "noise", "noise": "cellular", "cellular": {"return": "f1"}, "frequency": 0.01},
      "peak_up": {"type": "peak", "amplitude": 1, "radius": 0.7, "sharpness": 2, "distance": "dist"}},
    "outputs": {"temperature": "t", "base": "base", "range_up": "range_up", "peak_up": "peak_up"}},
  "climate": {"temperature": "temperature"},
  "biomes": {"selector": {"type": "blended", "blend_radius": 0.6, "blend_count": 3},
    "list": [
      {"name": "plains", "climate": {"temperature": 0.25}, "surface": {"top": "rock", "depth": 0}},
      {"name": "ranges", "climate": {"temperature": 0.55}, "surface": {"top": "turf", "depth": 0}},
      {"name": "peaks", "climate": {"temperature": 0.85}, "surface": {"top": "soil", "depth": 0}}]},
  "terrain": {"type": "biome_heightmap", "depth": 4, "biomes": [)JSON";
  text += terrainBiomes;
  text += "]}}";
  return text;
}

constexpr const char* kMountainTerrain =
    "{\"output\": \"base\", \"material\": \"rock\"}, "
    "{\"output\": \"base\", \"material\": \"turf\", \"uplift\": \"range_up\", \"uplift_amplitude\": 64}, "
    "{\"output\": \"base\", \"material\": \"soil\", \"uplift\": \"peak_up\", \"uplift_amplitude\": 96}";
constexpr const char* kMountainTerrainHalf =
    "{\"output\": \"base\", \"material\": \"rock\"}, "
    "{\"output\": \"base\", \"material\": \"turf\", \"uplift\": \"range_up\", \"uplift_amplitude\": 32}, "
    "{\"output\": \"base\", \"material\": \"soil\", \"uplift\": \"peak_up\", \"uplift_amplitude\": 48}";
constexpr const char* kMountainTerrainZero =
    "{\"output\": \"base\", \"material\": \"rock\"}, "
    "{\"output\": \"base\", \"material\": \"turf\", \"uplift\": \"range_up\", \"uplift_amplitude\": 0}, "
    "{\"output\": \"base\", \"material\": \"soil\", \"uplift\": \"peak_up\", \"uplift_amplitude\": 0}";
constexpr const char* kMountainTerrainNone =
    "{\"output\": \"base\", \"material\": \"rock\"}, "
    "{\"output\": \"base\", \"material\": \"turf\"}, "
    "{\"output\": \"base\", \"material\": \"soil\"}";

ChunkInfo mountainChunk(int cx, int cz, uint64_t dimSeed) {
  ChunkInfo c;
  c.cx = cx;
  c.cz = cz;
  c.size = 16;
  c.height = 256;
  c.dimensionSeed = dimSeed;
  c.chunkSeed = chunkSeed(dimSeed, cx, cz);
  return c;
}

/// Height at one world column (generates the owning chunk).
int mountainHeightAt(const GeneratorPipeline& gen, uint64_t seed, int wx, int wz) {
  const int cx = wx >= 0 ? wx / 16 : (wx - 15) / 16;  // floor division for negatives
  const int cz = wz >= 0 ? wz / 16 : (wz - 15) / 16;
  ColumnMaps maps;
  gen.generate(mountainChunk(cx, cz, seed), &maps);
  return maps.height[size_t((wz - cz * 16) * 16 + (wx - cx * 16))];
}

void mountainNodeChecks(Checker& check) {
  const auto eval = [&](const std::string& nodeJson) {
    GraphDesc desc;
    std::string err;
    if (!GraphDesc::parse("{\"nodes\": {\"n\": " + nodeJson + "}, \"outputs\": {\"o\": \"n\"}}", desc, err))
      return std::numeric_limits<float>::quiet_NaN();
    auto g = NoiseGraph::compile(desc, dimensionSeed(3, "mountains"), err);
    if (!g) return std::numeric_limits<float>::quiet_NaN();
    return g->evaluateAt(g->outputIndex("o"), 0.f, 0.f, 0.f);
  };
  const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
  check(near(eval("{\"type\": \"mountain_range\", \"amplitude\": 64, \"power\": 2,"
                 " \"ridges\": 0.5, \"mask\": 1}"),
             16.f),
        "mountains: mountain_range node math (64 * 0.5^2 * 1 = 16)");
  check(near(eval("{\"type\": \"mountain_range\", \"amplitude\": 64, \"power\": 1,"
                 " \"mask_low\": 0.25, \"mask_high\": 0.75, \"ridges\": 1, \"mask\": 0.5}"),
             32.f),
        "mountains: foothill mask falloff (smoothstep(0.5) = 0.5 -> 32)");
  check(near(eval("{\"type\": \"mountain_range\", \"amplitude\": 64,"
                 " \"mask_low\": 0.25, \"mask_high\": 0.75, \"ridges\": 1, \"mask\": 0.2}"),
             0.f),
        "mountains: mask below mask_low gates the uplift to 0");
  check(near(eval("{\"type\": \"peak\", \"amplitude\": 96, \"radius\": 1, \"sharpness\": 2, \"distance\": 0.25}"),
             54.f),
        "mountains: peak cone math (96 * 0.75^2 = 54)");
  check(near(eval("{\"type\": \"peak\", \"amplitude\": 96, \"radius\": 1, \"sharpness\": 2, \"distance\": 1.5}"),
             0.f),
        "mountains: peak is 0 beyond its radius");
  check(near(eval("{\"type\": \"peak\", \"amplitude\": 96, \"radius\": 1, \"sharpness\": 2, \"distance\": 0}"),
             96.f),
        "mountains: peak reaches full amplitude at distance 0");
  // Bad node configs are rejected at graph compile. (Load first, then report: argument
  // evaluation order would otherwise print a stale `err` in the message.)
  std::string err;
  const std::string tiny = "{\"graph\": {\"nodes\": {\"n\": %s}, \"outputs\": {\"o\": \"n\"}},"
                           " \"terrain\": {\"type\": \"heightmap\", \"output\": \"o\", \"material\": \"rock\"}}";
  char buf[512];
  std::snprintf(buf, sizeof buf, tiny.c_str(),
                "{\"type\": \"mountain_range\", \"mask_low\": 0.7, \"mask_high\": 0.7,"
                " \"ridges\": 1, \"mask\": 1}");
  const bool badMask = !loadGenerator(buf, 1, testMaterial, err);
  check(badMask && err.find("mask_high") != std::string::npos,
        "mountains: mask_low >= mask_high rejected (" + err + ")");
  std::snprintf(buf, sizeof buf, tiny.c_str(), "{\"type\": \"peak\", \"radius\": 0, \"distance\": 0.5}");
  const bool badRadius = !loadGenerator(buf, 1, testMaterial, err);
  check(badRadius && err.find("radius") != std::string::npos,
        "mountains: peak radius <= 0 rejected (" + err + ")");
  std::snprintf(buf, sizeof buf, tiny.c_str(),
                "{\"type\": \"peak\", \"radius\": 1, \"sharpness\": -1, \"distance\": 0.5}");
  const bool badSharp = !loadGenerator(buf, 1, testMaterial, err);
  check(badSharp && err.find("sharpness") != std::string::npos,
        "mountains: peak sharpness < 0 rejected (" + err + ")");
}

void mountainPipelineChecks(Checker& check) {
  const uint64_t seed = dimensionSeed(21, "mountains");
  std::string err;
  const auto gen = loadGenerator(mountainText(kMountainTerrain), seed, testMaterial, err);
  check(gen != nullptr, "mountains: uplift generator loads" + (err.empty() ? std::string() : " (" + err + ")"));
  if (!gen) return;

  // Determinism: two independent loads, same seed -> identical heights and chunk.
  std::string err2;
  const auto gen2 = loadGenerator(mountainText(kMountainTerrain), seed, testMaterial, err2);
  ColumnMaps m1, m2;
  const ColumnBuffer b1 = gen->generate(mountainChunk(3, -7, seed), &m1);
  const ColumnBuffer b2 = gen2->generate(mountainChunk(3, -7, seed), &m2);
  check(gen2 != nullptr && m1.height == m2.height && hashBuffer(b1) == hashBuffer(b2),
        "mountains: same seed -> byte-identical heights and chunk (two independent loads)");

  // Known-seed spot checks: fixed world columns, recorded heights (seed 21, "mountains").
  const std::array<std::array<int, 3>, 6> spots{{{/*wx*/ -40, /*wz*/ 8, /*h*/ 60},
                                                {-16, -16, 65},
                                                {0, 0, 64},
                                                {24, 32, 71},
                                                {48, -8, 68},
                                                {80, 80, 65}}};
  bool spotsOk = true;
  std::string spotMsg;
  for (const auto& s : spots) {
    const int h = mountainHeightAt(*gen, seed, s[0], s[1]);
    if (h != s[2]) {
      spotsOk = false;
      spotMsg += " (" + std::to_string(s[0]) + "," + std::to_string(s[1]) + ": got " + std::to_string(h) +
                 ", want " + std::to_string(s[2]) + ")";
    }
  }
  check(spotsOk, "mountains: known-seed spot heights" + spotMsg);

  // A different seed moves the mountain fields.
  const uint64_t otherSeed = dimensionSeed(22, "mountains");
  std::string err3;
  const auto genOther = loadGenerator(mountainText(kMountainTerrain), otherSeed, testMaterial, err3);
  bool differs = false;
  if (genOther) {
    ColumnMaps mo;
    genOther->generate(mountainChunk(3, -7, otherSeed), &mo);
    for (size_t i = 0; i < m1.height.size() && !differs; ++i) differs = mo.height[i] != m1.height[i];
  }
  check(genOther != nullptr && differs, "mountains: different seed -> different relief");

  // Amplitude: half the amplitude everywhere ~= half the uplift (floor tolerance +-1);
  // uplift is non-negative, so heights never drop below the zero-amplitude baseline.
  std::string errH, errZ;
  const auto genHalf = loadGenerator(mountainText(kMountainTerrainHalf), seed, testMaterial, errH);
  const auto genZero = loadGenerator(mountainText(kMountainTerrainZero), seed, testMaterial, errZ);
  bool ampOk = genHalf != nullptr && genZero != nullptr;
  bool liftOk = ampOk;
  int maxLift = 0;
  if (ampOk) {
    for (int cz = -2; cz <= 2 && ampOk; ++cz)
      for (int cx = -2; cx <= 2 && ampOk; ++cx) {
        ColumnMaps mh, mz, mf;
        genHalf->generate(mountainChunk(cx, cz, seed), &mh);
        genZero->generate(mountainChunk(cx, cz, seed), &mz);
        gen->generate(mountainChunk(cx, cz, seed), &mf);
        for (size_t i = 0; i < mh.height.size() && ampOk; ++i) {
          const int dFull = mf.height[i] - mz.height[i];
          const int dHalf = mh.height[i] - mz.height[i];
          maxLift = std::max(maxLift, dFull);
          ampOk = ampOk && dFull >= 0 && dHalf >= 0 && std::abs(dFull - 2 * dHalf) <= 1;
        }
      }
  }
  check(ampOk, "mountains: uplift_amplitude scales the uplift linearly (full ~= 2x half, +-1)");
  check(liftOk && maxLift >= 16,
        "mountains: uplift actually lifts terrain (max +" + std::to_string(maxLift) + " >= 16)");

  // Zero amplitude == no uplift keys: the loader default path matches explicit 0.
  std::string errN;
  const auto genNone = loadGenerator(mountainText(kMountainTerrainNone), seed, testMaterial, errN);
  bool noneOk = genNone != nullptr;
  if (noneOk) {
    ColumnMaps mz, mn;
    genZero->generate(mountainChunk(1, 2, seed), &mz);
    genNone->generate(mountainChunk(1, 2, seed), &mn);
    noneOk = mz.height == mn.height;
  }
  check(noneOk, "mountains: omitted uplift behaves exactly like uplift_amplitude 0");

  // Blending continuity: scan x across plains -> ranges -> peaks (crossing chunk borders);
  // heights must ramp, never wall, even with +-96 blocks of uplift in play.
  std::vector<int> heights;
  for (int cx = -4; cx <= 3; ++cx) {
    ColumnMaps ms;
    gen->generate(mountainChunk(cx, 0, seed), &ms);
    for (int x = 0; x < 16; ++x) heights.push_back(ms.height[size_t(8 * 16 + x)]);  // z = 8
  }
  int maxStep = 0;
  for (size_t k = 1; k < heights.size(); ++k) maxStep = std::max(maxStep, std::abs(heights[k] - heights[k - 1]));
  const int hMin = *std::min_element(heights.begin(), heights.end());
  const int hMax = *std::max_element(heights.begin(), heights.end());
  check(maxStep <= 8, "mountains: no cliffs at biome borders (max adjacent step " +
                          std::to_string(maxStep) + " <= 8 across 128 columns)");
  check(hMax - hMin >= 30, "mountains: real relief across the scan (range " + std::to_string(hMax - hMin) +
                               " >= 30)");

  // Loader rejections: unknown uplift output, and uplift_amplitude without uplift.
  std::string badErr;
  const bool badOut = !loadGenerator(mountainText("{\"output\": \"base\", \"material\": \"rock\"}, "
                                                  "{\"output\": \"base\", \"material\": \"turf\", "
                                                  "\"uplift\": \"nope\", \"uplift_amplitude\": 64}, "
                                                  "{\"output\": \"base\", \"material\": \"soil\"}"),
                                     seed, testMaterial, badErr);
  check(badOut && badErr.find("graph has no output") != std::string::npos,
        "mountains: unknown uplift output rejected (" + badErr + ")");
  std::string loneErr;
  const bool loneAmp = !loadGenerator(mountainText("{\"output\": \"base\", \"material\": \"rock\"}, "
                                                   "{\"output\": \"base\", \"material\": \"turf\", "
                                                   "\"uplift_amplitude\": 64}, "
                                                   "{\"output\": \"base\", \"material\": \"soil\"}"),
                                      seed, testMaterial, loneErr);
  check(loneAmp && loneErr.find("uplift_amplitude") != std::string::npos,
        "mountains: uplift_amplitude without uplift rejected (" + loneErr + ")");
}

void mountainChecks(Checker& check) {
  mountainNodeChecks(check);
  mountainPipelineChecks(check);
}

// ---- rivers

/// Sloped terrain (height = 80 - 0.2*x + wobble): rivers flow downhill toward +x.
constexpr const char* kRiverGeneratorText = R"({
  "graph": {"nodes": {
      "x": {"type": "coordinate", "axis": "x"},
      "slope": {"type": "scale_bias", "scale": -0.2, "bias": 80, "input": "x"},
      "n": {"type": "noise", "noise": "perlin", "frequency": 0.02},
      "wob": {"type": "scale_bias", "scale": 8, "input": "n"},
      "h": {"type": "add", "inputs": ["slope", "wob"]}},
    "outputs": {"height": "h"}},
  "terrain": {"type": "heightmap", "output": "height", "material": "rock"},
  "carvers": [{"type": "river", "water_material": "water", "bed_material": "gravel",
               "cell_size": 64, "chance": 1.0, "max_steps": 64, "step_len": 4,
               "width": 4, "depth": 4, "freeboard": 1, "meander": 2,
               "min_start_height": -1000, "water_table": -1000}]
})";

/// Same terrain, no carvers: the pre-carve reference for the water-placement checks.
constexpr const char* kRiverNoCarveText = R"({
  "graph": {"nodes": {
      "x": {"type": "coordinate", "axis": "x"},
      "slope": {"type": "scale_bias", "scale": -0.2, "bias": 80, "input": "x"},
      "n": {"type": "noise", "noise": "perlin", "frequency": 0.02},
      "wob": {"type": "scale_bias", "scale": 8, "input": "n"},
      "h": {"type": "add", "inputs": ["slope", "wob"]}},
    "outputs": {"height": "h"}},
  "terrain": {"type": "heightmap", "output": "height", "material": "rock"}
})";

/// Flat terrain (60) with a SEPARATE routing output (sloped plane): exercises the
/// routing_output wiring and the routing/terrain-disagreement caveat. Meander off so the
/// channel is a clean cosine cross-section for the bank-shape checks.
constexpr const char* kRiverBankText = R"({
  "graph": {"nodes": {
      "flat": {"type": "constant", "value": 60},
      "x": {"type": "coordinate", "axis": "x"},
      "route": {"type": "scale_bias", "scale": -0.5, "bias": 70, "input": "x"}},
    "outputs": {"height": "flat", "route": "route"}},
  "terrain": {"type": "heightmap", "output": "height", "material": "rock"},
  "carvers": [{"type": "river", "water_material": "water", "routing_output": "route",
               "cell_size": 64, "chance": 1.0, "max_steps": 64, "step_len": 4,
               "width": 4, "depth": 4, "freeboard": 1, "meander": 0,
               "min_start_height": -1000, "water_table": -1000}]
})";

void riverChecks(Checker& check) {
  std::string err;
  auto gen = loadGenerator(kRiverGeneratorText, dimensionSeed(7, "wafer"), testMaterial, err);
  check(gen != nullptr, "rivers: generator JSON with river carver loads" + (err.empty() ? std::string() : " (" + err + ")"));
  if (!gen) return;
  auto ref = loadGenerator(kRiverNoCarveText, dimensionSeed(7, "wafer"), testMaterial, err);
  check(ref != nullptr, "rivers: reference generator (no carver) loads");

  // Loader rejects bad river configs.
  std::string badErr;
  const char* kGraphTerrain = R"("graph": {"nodes": {"h": {"type": "constant", "value": 60}},
    "outputs": {"height": "h"}}, "terrain": {"type": "heightmap", "output": "height", "material": "rock"})";
  auto noWater = loadGenerator(std::string("{") + kGraphTerrain + R"(, "carvers": [{"type": "river"}]})",
                               dimensionSeed(7, "wafer"), testMaterial, badErr);
  check(!noWater && badErr.find("water_material") != std::string::npos,
        "rivers: missing water_material rejected (" + badErr + ")");
  badErr.clear();
  auto badRoute = loadGenerator(std::string("{") + kGraphTerrain +
                                    R"(, "carvers": [{"type": "river", "water_material": "water",
                                          "routing_output": "nope"}]})",
                                dimensionSeed(7, "wafer"), testMaterial, badErr);
  check(!badRoute && badErr.find("nope") != std::string::npos,
        "rivers: unknown routing_output rejected (" + badErr + ")");
  badErr.clear();
  auto badBiome = loadGenerator(std::string("{") + kGraphTerrain +
                                    R"(, "carvers": [{"type": "river", "water_material": "water",
                                          "biomes": {"nope": 0.5}}]})",
                                dimensionSeed(7, "wafer"), testMaterial, badErr);
  check(!badBiome && badErr.find("unknown biome") != std::string::npos,
        "rivers: unknown biome density rejected (" + badErr + ")");

  const auto info = [](int cx, int cz) {
    ChunkInfo c;
    c.cx = cx;
    c.cz = cz;
    c.size = 16;
    c.height = 128;
    c.dimensionSeed = dimensionSeed(7, "wafer");
    c.chunkSeed = chunkSeed(c.dimensionSeed, cx, cz);
    return c;
  };

  // traceRiver: deterministic, non-empty on a slope, and never flows uphill.
  RiverSettings s;
  s.water = 7;
  s.bed = 8;
  s.density.fill(1.f);
  s.chance = 1.f;
  s.cellSize = 64;
  s.minStartHeight = -1000.f;
  s.waterTable = -1000.f;
  const NoiseGraph& g = gen->graph();
  const int hOut = g.outputIndex("height");
  const uint64_t ds = dimensionSeed(7, "wafer");
  const auto p1 = RiverCarver::traceRiver(g, hOut, ds, 0, 0, s, nullptr);
  const auto p2 = RiverCarver::traceRiver(g, hOut, ds, 0, 0, s, nullptr);
  bool same = p1.size() == p2.size() && !p1.empty();
  for (size_t i = 0; same && i < p1.size(); ++i)
    same = p1[i].x == p2[i].x && p1[i].z == p2[i].z && p1[i].h == p2[i].h;
  check(same, "rivers: traceRiver deterministic (identical points for the same seed)");
  bool downhill = !p1.empty();
  for (size_t i = 1; i < p1.size(); ++i) downhill = downhill && p1[i].h <= p1[i - 1].h + s.uphillTol + 1e-4f;
  check(downhill, "rivers: traced path never flows uphill (monotone within uphillTol)");

  // Full pipeline: same seed -> identical river cells; other seed -> different.
  const ColumnBuffer a = gen->generate(info(0, 0)), b2 = gen->generate(info(0, 0));
  ChunkInfo other = info(0, 0);
  other.dimensionSeed = dimensionSeed(8, "wafer");
  other.chunkSeed = chunkSeed(other.dimensionSeed, 0, 0);
  const ColumnBuffer c2 = gen->generate(other);
  check(hashBuffer(a) == hashBuffer(b2) && hashBuffer(a) != hashBuffer(c2),
        "rivers: chunk determinism (same seed identical, other seed differs)");

  // Water placement: level-16 source only, only in carved columns, at/below the bank.
  int waterCells = 0, badWater = 0, carvedCols = 0;
  for (int cz = 0; cz < 3; ++cz)
    for (int cx = 0; cx < 3; ++cx) {
      ColumnMaps maps, refMaps;
      const ColumnBuffer b = gen->generate(info(cx, cz), &maps);
      const ColumnBuffer r = ref->generate(info(cx, cz), &refMaps);
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
          const size_t i = size_t(z * 16 + x);
          const int newS = maps.height[i], oldS = refMaps.height[i];
          const bool carved = newS < oldS;
          carvedCols += carved ? 1 : 0;
          if (carved && b.get(x, newS, z) != 8) ++badWater;  // channel floor re-materialed
          for (int y = 0; y < b.height(); ++y) {
            if (b.get(x, y, z) != 7) continue;
            ++waterCells;
            if (b.state(x, y, z) != 16 || !carved || y > oldS) ++badWater;
          }
        }
    }
  check(waterCells > 0 && carvedCols > 0,
        "rivers: channels carved with water (" + std::to_string(waterCells) + " water cells, " +
            std::to_string(carvedCols) + " carved columns in 3x3 chunks)");
  check(badWater == 0, "rivers: water is level-16 source, only in carved columns at/below the bank");

  // Cross-border (B-L4, P3-2 — replaces the old "chunk-pure" check, which
  // was tautological: it generated the neighbours but only ever compared
  // the centre chunk against ITSELF, so a carver that dropped its
  // cross-border replay would still pass). Real check: assemble a 3x3
  // world from per-chunk generation and compare EVERY column against an
  // independent replica of the carve (nearest traced point + cosine
  // profile over the pre-carve reference heights, mirroring the worm /
  // cavern replica checks). A replay bug at a chunk border — an origin
  // cell not replayed, a clamped profile — shows up as a mismatch in the
  // border columns of the assembly.
  {
    // All traced points from every origin cell whose walk can reach the
    // 3x3 assembly (origin-cell range = carve()'s replay range, widened
    // to the whole assembly).
    std::vector<RiverPoint> pts;
    const int reachCells = int(std::ceil((float(s.maxSteps) * (float(s.stepLen) + s.meander) + s.width) /
                                         float(s.cellSize))) +
                           1;
    for (int ccz = -reachCells; ccz <= reachCells; ++ccz)
      for (int ccx = -reachCells; ccx <= reachCells; ++ccx) {
        auto p = RiverCarver::traceRiver(g, hOut, ds, ccx, ccz, s, nullptr);
        pts.insert(pts.end(), p.begin(), p.end());
      }
    const float w2 = (s.width + 0.5f) * (s.width + 0.5f);
    const float pi = std::numbers::pi_v<float>;
    bool exact = true;
    int carvedCols = 0, borderCarved = 0;
    for (int cz = 0; cz < 3; ++cz)
      for (int cx = 0; cx < 3; ++cx) {
        ColumnMaps maps, refMaps;
        const ColumnBuffer b = gen->generate(info(cx, cz), &maps);
        ref->generate(info(cx, cz), &refMaps);  // pre-carve reference heights
        for (int z = 0; z < 16; ++z)
          for (int x = 0; x < 16; ++x) {
            const size_t i = size_t(z * 16 + x);
            const int oldS = refMaps.height[i];
            const float wx = float(cx * 16 + x) + 0.5f, wz = float(cz * 16 + z) + 0.5f;
            float bestD2 = w2, bestH = 0.f;
            for (const auto& p : pts) {
              const float ddx = wx - p.x, ddz = wz - p.z;
              const float d2 = ddx * ddx + ddz * ddz;
              if (d2 < bestD2) { bestD2 = d2; bestH = p.h; }
            }
            int expectH = oldS;  // uncarved unless the replica profile cuts
            bool expectWater = false;
            if (bestD2 < w2 && oldS >= 1) {
              const float t = std::min(1.f, std::sqrt(bestD2) / s.width);
              const int cutY = int(std::floor(bestH - s.depth * 0.5f * (1.f + std::cos(pi * t))));
              if (cutY >= 0 && cutY < oldS) {
                expectH = cutY;
                const int waterY = std::min({int(std::floor(bestH - s.freeboard)), oldS, b.height() - 1});
                expectWater = waterY > cutY;
              }
            }
            bool actualWater = false;
            for (int y = 0; y < b.height(); ++y) actualWater = actualWater || b.get(x, y, z) == 7;
            if (maps.height[i] != expectH || actualWater != expectWater) exact = false;
            if (expectH < oldS) {
              ++carvedCols;
              if (x == 0 || x == 15 || z == 0 || z == 15) ++borderCarved;  // carved ON a chunk edge
            }
          }
      }
    check(exact && carvedCols > 0,
          "rivers: assembled 3x3 matches the independent carve replica in every column (" +
              std::to_string(carvedCols) + " carved columns)");
    check(borderCarved > 0,
          "rivers: the channel crosses chunk borders (" + std::to_string(borderCarved) +
              " carved columns on chunk edges, continuous across the seams)");
  }

  // Banks: on flat terrain the carved cross-section is a smooth cosine -- adjacent carved
  // columns never step more than the profile slope allows (no 1-wide vertical slots), and
  // no carved column is isolated.
  auto bankGen = loadGenerator(kRiverBankText, dimensionSeed(7, "wafer"), testMaterial, err);
  check(bankGen != nullptr, "rivers: flat-terrain + routing_output generator loads");
  if (bankGen) {
    const int W = 3 * 16, D = 16;
    std::vector<int> surf(size_t(W * D), -1);  // carved surface, -1 = not carved
    for (int cx = 0; cx < 3; ++cx) {
      ColumnMaps maps;
      const ColumnBuffer b = bankGen->generate(info(cx, 0), &maps);
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) surf[size_t(z * W + cx * 16 + x)] = maps.height[size_t(z * 16 + x)] < 60
                                                                            ? maps.height[size_t(z * 16 + x)]
                                                                            : -1;
    }
    int carved = 0, maxStep = 0, isolated = 0;
    for (int z = 0; z < D; ++z)
      for (int x = 0; x < W; ++x) {
        const int v = surf[size_t(z * W + x)];
        if (v < 0) continue;
        ++carved;
        int carvedNeighbours = 0;
        for (int dz = -1; dz <= 1; ++dz)
          for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dz == 0) continue;
            const int nx = x + dx, nz = z + dz;
            if (nx < 0 || nx >= W || nz < 0 || nz >= D) continue;
            const int nv = surf[size_t(nz * W + nx)];
            if (nv < 0) continue;
            ++carvedNeighbours;
            if (dx * dx + dz * dz == 1) maxStep = std::max(maxStep, std::abs(v - nv));
          }
        if (carvedNeighbours == 0) ++isolated;
      }
    // Cosine profile: max slope depth*pi/(2*width) ~= 1.6 per block, plus the quantised
    // routing-height steps between neighbouring path points; 3 is generous headroom.
    check(carved > 0 && maxStep <= 3 && isolated == 0,
          "rivers: sloped banks, no vertical slots (" + std::to_string(carved) + " carved, max adjacent step " +
              std::to_string(maxStep) + ", isolated " + std::to_string(isolated) + ")");
  }
}

}  // namespace

int runProcgenSelfTest(JobPool& pool) {
  std::printf("Procgen self-test (noise, graph, pipeline)\n");
  Checker check;
  noiseChecks(check);
  graphChecks(check);
  graphNewNodeChecks(check);
  biomeChecks(check);
  pipelineChecks(check, pool);
  ruinsChecks(check);
  waferBiomeChecks(check);
  biomeBlendChecks(check);
  treeFeatureChecks(check);
  structureChecks(check);
  caveChecks(check);
  mountainChecks(check);
  riverChecks(check);
  const int divFails = runDiversitySelfTest();
  check(divFails == 0, "diversity self-test: all checks pass");
  const int failsBefore = check.fails();
  const int lithoFails = litho_formula::runLithoFormulaVoxelizeSelfTest();
  const int structRegFails = runStructRegistrySelfTest();
  const int diversityFails = runDiversitySelfTest();
  const int fails = failsBefore + lithoFails + structRegFails + diversityFails;
  std::printf(fails ? "PROCGEN SELFTEST FAILED (%d)\n" : "PROCGEN SELFTEST OK\n", fails);
  return fails;
}

}  // namespace litholattice::procgen
