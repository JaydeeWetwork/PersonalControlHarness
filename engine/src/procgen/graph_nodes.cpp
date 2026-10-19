#include "graph_nodes.h"
#include <algorithm>
#include <array>
#include <cmath>
#include "litholattice/procgen/modifiers.h"
#include "litholattice/procgen/seed.h"

namespace litholattice::procgen::detail {
namespace {

using Inputs = std::span<const std::span<const float>>;

// ---- parameter helpers (values were type-checked loosely; wrong types fall back or fail here)

bool readVec3(const Json& params, const char* key, std::array<float, 3>& out, std::string& err) {
  const Json* v = params.get(key);
  if (!v) return true;
  if (v->type == Json::Type::Number) {
    out.fill(float(v->num));
    return true;
  }
  if (v->type != Json::Type::Array || v->arr.size() != 3) {
    err = std::string("'") + key + "' must be a number or [x, y, z]";
    return false;
  }
  for (size_t i = 0; i < 3; ++i) out[i] = float(v->arr[i].num);
  return true;
}

int axisIndex(std::string_view s) { return s == "x" ? 0 : s == "y" ? 1 : s == "z" ? 2 : -1; }

std::span<const float> axisOf(const PointsView& p, int axis) { return axis == 0 ? p.x : axis == 1 ? p.y : p.z; }

// ---- leaf nodes

class ConstantNode final : public Node {
 public:
  explicit ConstantNode(float v) : v_(v) {}
  void eval(const PointsView&, Inputs, std::span<float> out) const override { std::fill(out.begin(), out.end(), v_); }

 private:
  float v_;
};

class CoordinateNode final : public Node {
 public:
  explicit CoordinateNode(int axis) : axis_(axis) {}
  void eval(const PointsView& p, Inputs, std::span<float> out) const override {
    const auto a = axisOf(p, axis_);
    std::copy(a.begin(), a.end(), out.begin());
  }

 private:
  int axis_;
};

class WaveNode final : public Node {
 public:
  WaveNode(std::array<float, 3> dir, float freq, float amp, float phase, bool cosine)
      : dir_(dir), freq_(freq), amp_(amp), phase_(phase), cosine_(cosine) {}
  void eval(const PointsView& p, Inputs, std::span<float> out) const override {
    for (size_t i = 0; i < out.size(); ++i) {
      const double t = double(freq_) * (double(dir_[0]) * p.x[i] + double(dir_[1]) * p.y[i] + double(dir_[2]) * p.z[i]) + phase_;
      out[i] = float(amp_ * (cosine_ ? std::cos(t) : std::sin(t)));
    }
  }

 private:
  std::array<float, 3> dir_;
  float freq_, amp_, phase_;
  bool cosine_;
};

/// Samples a noise at (p * frequency + offset). 2D noise reads the horizontal plane (x, z).
class NoiseNode final : public Node {
 public:
  NoiseNode(std::shared_ptr<const INoise> noise, int dims, std::array<float, 3> freq, std::array<float, 3> offset, float w)
      : noise_(std::move(noise)), dims_(dims), freq_(freq), offset_(offset), w_(w) {}

  void eval(const PointsView& p, Inputs, std::span<float> out) const override {
    const size_t n = out.size();
    std::vector<float> buf(n * 4);
    const std::span<float> a(buf.data(), n), b(buf.data() + n, n), c(buf.data() + 2 * n, n), d(buf.data() + 3 * n, n);
    for (size_t i = 0; i < n; ++i) {
      a[i] = p.x[i] * freq_[0] + offset_[0];
      b[i] = p.y[i] * freq_[1] + offset_[1];
      c[i] = p.z[i] * freq_[2] + offset_[2];
      d[i] = w_;
    }
    PointsView q;
    if (dims_ == 2) q = {a, c, {}, {}};
    else q = {a, b, c, dims_ == 4 ? std::span<const float>(d) : std::span<const float>()};
    noise_->sampleBatch(dims_, q, out);
  }

 private:
  std::shared_ptr<const INoise> noise_;
  int dims_;
  std::array<float, 3> freq_, offset_;
  float w_;
};

// ---- combiners

enum class Combine { Add, Multiply, Min, Max };

class CombineNode final : public Node {
 public:
  explicit CombineNode(Combine op) : op_(op) {}
  void eval(const PointsView&, Inputs in, std::span<float> out) const override {
    std::copy(in[0].begin(), in[0].end(), out.begin());
    for (size_t k = 1; k < in.size(); ++k)
      for (size_t i = 0; i < out.size(); ++i) {
        const float v = in[k][i];
        switch (op_) {
          case Combine::Add: out[i] += v; break;
          case Combine::Multiply: out[i] *= v; break;
          case Combine::Min: out[i] = std::min(out[i], v); break;
          case Combine::Max: out[i] = std::max(out[i], v); break;
        }
      }
  }

 private:
  Combine op_;
};

/// Shared shape of the one-input transforms: out[i] = f(in[0][i]).
template <typename F>
class MapNode final : public Node {
 public:
  explicit MapNode(F f) : f_(std::move(f)) {}
  void eval(const PointsView&, Inputs in, std::span<float> out) const override {
    for (size_t i = 0; i < out.size(); ++i) out[i] = f_(in[0][i]);
  }

 private:
  F f_;
};

template <typename F>
std::unique_ptr<Node> mapNode(F f) {
  return std::make_unique<MapNode<F>>(std::move(f));
}

/// Piecewise curve through control points: linear, or monotone cubic (Fritsch-Carlson, no
/// overshoot). Inputs outside the control range clamp to the end values.
class Curve {
 public:
  Curve(std::vector<std::pair<float, float>> pts, bool cubic) : pts_(std::move(pts)), cubic_(cubic) {
    const size_t n = pts_.size();
    tangents_.assign(n, 0.f);
    if (!cubic_ || n < 2) return;
    std::vector<float> slope(n - 1);
    for (size_t i = 0; i + 1 < n; ++i)
      slope[i] = (pts_[i + 1].second - pts_[i].second) / std::max(1e-9f, pts_[i + 1].first - pts_[i].first);
    tangents_[0] = slope[0];
    tangents_[n - 1] = slope[n - 2];
    for (size_t i = 1; i + 1 < n; ++i) tangents_[i] = slope[i - 1] * slope[i] <= 0 ? 0.f : (slope[i - 1] + slope[i]) * 0.5f;
    for (size_t i = 0; i + 1 < n; ++i) {
      if (slope[i] == 0.f) { tangents_[i] = tangents_[i + 1] = 0.f; continue; }
      const float a = tangents_[i] / slope[i], b = tangents_[i + 1] / slope[i], s = a * a + b * b;
      if (s > 9.f) {
        const float t = 3.f / std::sqrt(s);
        tangents_[i] = t * a * slope[i];
        tangents_[i + 1] = t * b * slope[i];
      }
    }
  }

  float operator()(float v) const {
    if (v <= pts_.front().first) return pts_.front().second;
    if (v >= pts_.back().first) return pts_.back().second;
    const auto it = std::upper_bound(pts_.begin(), pts_.end(), v, [](float x, const auto& p) { return x < p.first; });
    const size_t i = size_t(it - pts_.begin()) - 1;
    const auto& [x0, y0] = pts_[i];
    const auto& [x1, y1] = pts_[i + 1];
    const float h = x1 - x0, t = (v - x0) / h;
    if (!cubic_) return y0 + (y1 - y0) * t;
    const float t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * y0 + (t3 - 2 * t2 + t) * h * tangents_[i] + (-2 * t3 + 3 * t2) * y1 +
           (t3 - t2) * h * tangents_[i + 1];
  }

 private:
  std::vector<std::pair<float, float>> pts_;
  std::vector<float> tangents_;
  bool cubic_;
};

inline float smoothstep01(float t) { return t * t * (3.f - 2.f * t); }

/// a where control < threshold, b above; smoothstep blend within threshold +- falloff.
class SelectNode final : public Node {
 public:
  SelectNode(float threshold, float falloff) : threshold_(threshold), falloff_(falloff) {}
  void eval(const PointsView&, Inputs in, std::span<float> out) const override {
    for (size_t i = 0; i < out.size(); ++i) {
      const float c = in[0][i], a = in[1][i], b = in[2][i];
      if (falloff_ <= 0.f) {
        out[i] = c < threshold_ ? a : b;
        continue;
      }
      const float t = std::clamp((c - (threshold_ - falloff_)) / (2.f * falloff_), 0.f, 1.f);
      out[i] = a + (b - a) * smoothstep01(t);
    }
  }

 private:
  float threshold_, falloff_;
};

class BlendNode final : public Node {
 public:
  void eval(const PointsView&, Inputs in, std::span<float> out) const override {
    for (size_t i = 0; i < out.size(); ++i) out[i] = in[0][i] + (in[1][i] - in[0][i]) * std::clamp(in[2][i], 0.f, 1.f);
  }
};

/// Mountain uplift from a ridged [0, 1] field gated by a foothill mask:
///   out = amplitude * max(ridges, 0)^power * smoothstep(mask_low, mask_high, mask)
/// The mask (a low-frequency field, e.g. scaled fbm in [0, 1]) carves the ranges out of the
/// plains: where it is low the uplift fades to zero (foothills), where it is high the full
/// ridged relief applies. Pass a constant 1 for `mask` to disable the foothill gating.
class MountainRangeNode final : public Node {
 public:
  MountainRangeNode(float amplitude, float power, float maskLo, float maskHi)
      : amp_(amplitude), power_(power), lo_(maskLo), hi_(maskHi) {}
  void eval(const PointsView&, Inputs in, std::span<float> out) const override {
    for (size_t i = 0; i < out.size(); ++i) {
      const float r = std::max(in[0][i], 0.f);
      const float t = std::clamp((in[1][i] - lo_) / (hi_ - lo_), 0.f, 1.f);
      out[i] = amp_ * std::pow(r, power_) * smoothstep01(t);
    }
  }

 private:
  float amp_, power_, lo_, hi_;
};

/// Cone peak from a distance field (e.g. cellular F1):
///   out = amplitude * max(0, 1 - distance / radius)^sharpness
/// Sharpness 1 is a straight cone; higher values narrow the summit.
class PeakNode final : public Node {
 public:
  PeakNode(float amplitude, float radius, float sharpness)
      : amp_(amplitude), radius_(radius), sharp_(sharpness) {}
  void eval(const PointsView&, Inputs in, std::span<float> out) const override {
    for (size_t i = 0; i < out.size(); ++i) {
      const float t = std::clamp(1.f - in[0][i] / radius_, 0.f, 1.f);
      out[i] = amp_ * std::pow(t, sharp_);
    }
  }

 private:
  float amp_, radius_, sharp_;
};

// ---- factories

std::unique_ptr<Node> makeConstantNode(const Json& p, uint64_t, std::string&) { return makeConstant(float(p.getNum("value"))); }

std::unique_ptr<Node> makeMountainRange(const Json& p, uint64_t, std::string& err) {
  const float lo = float(p.getNum("mask_low", 0)), hi = float(p.getNum("mask_high", 1));
  if (!(hi > lo)) { err = "'mask_high' must be > 'mask_low'"; return nullptr; }
  const float power = float(p.getNum("power", 1));
  if (!(power >= 0)) { err = "'power' must be >= 0"; return nullptr; }
  const float amp = float(p.getNum("amplitude", 1));
  if (!std::isfinite(amp)) { err = "'amplitude' must be finite"; return nullptr; }
  return std::make_unique<MountainRangeNode>(amp, power, lo, hi);
}

std::unique_ptr<Node> makePeak(const Json& p, uint64_t, std::string& err) {
  const float radius = float(p.getNum("radius", 1));
  if (!(radius > 0)) { err = "'radius' must be > 0"; return nullptr; }
  const float sharp = float(p.getNum("sharpness", 1));
  if (!(sharp >= 0)) { err = "'sharpness' must be >= 0"; return nullptr; }
  const float amp = float(p.getNum("amplitude", 1));
  if (!std::isfinite(amp)) { err = "'amplitude' must be finite"; return nullptr; }
  return std::make_unique<PeakNode>(amp, radius, sharp);
}


std::unique_ptr<Node> makeCoordinate(const Json& p, uint64_t, std::string& err) {
  const int axis = axisIndex(p.getStr("axis", "y"));
  if (axis < 0) err = "'axis' must be \"x\", \"y\" or \"z\"";
  return axis < 0 ? nullptr : std::make_unique<CoordinateNode>(axis);
}

std::unique_ptr<Node> makeWave(const Json& p, uint64_t, std::string& err) {
  std::array<float, 3> dir{1, 0, 0};
  if (const Json* a = p.get("axis")) {
    const int i = axisIndex(a->str);
    if (i < 0) { err = "'axis' must be \"x\", \"y\" or \"z\""; return nullptr; }
    dir = {0, 0, 0};
    dir[size_t(i)] = 1;
  }
  if (!readVec3(p, "direction", dir, err)) return nullptr;
  const std::string fn = p.getStr("function", "sin");
  if (fn != "sin" && fn != "cos") { err = "'function' must be \"sin\" or \"cos\""; return nullptr; }
  return std::make_unique<WaveNode>(dir, float(p.getNum("frequency", 1)), float(p.getNum("amplitude", 1)),
                                    float(p.getNum("phase", 0)), fn == "cos");
}

bool readNoiseSettings(const Json& p, uint64_t seed, NoiseSettings& s, std::string& err) {
  const auto type = parseNoiseType(p.getStr("noise", "opensimplex2s"));
  if (!type) { err = "unknown noise '" + p.getStr("noise") + "'"; return false; }
  s.type = *type;
  s.seed = seed;
  if (const Json* c = p.get("cellular")) {
    const auto r = parseCellularReturn(c->getStr("return", "f1"));
    const auto m = parseDistanceMetric(c->getStr("metric", "euclidean"));
    if (!r || !m) { err = "bad cellular 'return' or 'metric'"; return false; }
    s.cellularReturn = *r;
    s.metric = *m;
    s.jitter = float(c->getNum("jitter", 1));
  }
  return true;
}

std::shared_ptr<const INoise> withFractal(std::shared_ptr<const INoise> n, const Json* f, uint64_t seed, std::string& err) {
  if (!f) return n;
  const auto type = parseFractalType(f->getStr("type", "fbm"));
  if (!type) { err = "unknown fractal type '" + f->getStr("type") + "'"; return nullptr; }
  FractalSettings fs;
  fs.type = *type;
  fs.octaves = int(f->getNum("octaves", fs.octaves));
  fs.lacunarity = float(f->getNum("lacunarity", fs.lacunarity));
  fs.gain = float(f->getNum("gain", fs.gain));
  return createFractal(std::move(n), fs, deriveSeed(seed, nameHash("fractal")));
}

/// Shared builder for the noise-family leaf nodes (noise / fractal / warp).
/// requireFractal / requireWarp turn the optional parameter blocks of the plain
/// `noise` node into the node's defining contract (standalone operator nodes,
/// 2026-10-13): a `fractal` node without a "fractal" block, or a `warp` node
/// without a "warp" block, is a compile error rather than a silent plain noise.
std::unique_ptr<Node> makeNoiseImpl(const Json& p, uint64_t seed, std::string& err, bool requireFractal,
                                    bool requireWarp) {
  if (requireFractal && !p.get("fractal")) { err = "a 'fractal' node needs a fractal block"; return nullptr; }
  if (requireWarp && !p.get("warp")) { err = "a 'warp' node needs a warp block"; return nullptr; }
  NoiseSettings s;
  if (!readNoiseSettings(p, seed, s, err)) return nullptr;
  std::shared_ptr<const INoise> n = withFractal(createNoise(s), p.get("fractal"), seed, err);
  if (!n) return nullptr;
  if (const Json* w = p.get("warp")) {
    NoiseSettings ws;
    const uint64_t warpSeed = deriveSeed(seed, nameHash("warp"));
    if (!readNoiseSettings(*w, warpSeed, ws, err)) return nullptr;
    auto warp = withFractal(createNoise(ws), w->get("fractal"), warpSeed, err);
    if (!warp) return nullptr;
    n = createDomainWarp(std::move(n), std::move(warp), float(w->getNum("amplitude", 1)), float(w->getNum("frequency", 1)));
  }
  const int dims = int(p.getNum("dims", 2));
  if (dims < 2 || dims > 4) { err = "'dims' must be 2, 3 or 4"; return nullptr; }
  std::array<float, 3> freq{1, 1, 1}, offset{0, 0, 0};
  if (!readVec3(p, "frequency", freq, err) || !readVec3(p, "offset", offset, err)) return nullptr;
  return std::make_unique<NoiseNode>(std::move(n), dims, freq, offset, float(p.getNum("w", 0)));
}

std::unique_ptr<Node> makeNoise(const Json& p, uint64_t seed, std::string& err) {
  return makeNoiseImpl(p, seed, err, false, false);
}

std::unique_ptr<Node> makeFractalNode(const Json& p, uint64_t seed, std::string& err) {
  return makeNoiseImpl(p, seed, err, true, false);
}

std::unique_ptr<Node> makeWarpNode(const Json& p, uint64_t seed, std::string& err) {
  return makeNoiseImpl(p, seed, err, false, true);
}

std::unique_ptr<Node> makeAbs(const Json&, uint64_t, std::string&) {
  return mapNode([](float v) { return std::abs(v); });
}

std::unique_ptr<Node> makeInvert(const Json&, uint64_t, std::string&) {
  return mapNode([](float v) { return -v; });
}

/// Sign-preserving power: out = sign(v) * |v|^exponent (exponent > 0), so negative
/// inputs keep their sign instead of going NaN on fractional exponents.
std::unique_ptr<Node> makePower(const Json& p, uint64_t, std::string& err) {
  const float e = float(p.getNum("exponent", 2));
  if (!(e > 0) || !std::isfinite(e)) { err = "'exponent' must be a positive finite number"; return nullptr; }
  return mapNode([e](float v) { return (v < 0 ? -1.f : 1.f) * std::pow(std::abs(v), e); });
}

template <Combine Op>
std::unique_ptr<Node> makeCombine(const Json&, uint64_t, std::string&) {
  return std::make_unique<CombineNode>(Op);
}

std::unique_ptr<Node> makeClamp(const Json& p, uint64_t, std::string& err) {
  const float lo = float(p.getNum("min", -1)), hi = float(p.getNum("max", 1));
  if (lo > hi) { err = "'min' > 'max'"; return nullptr; }
  return mapNode([lo, hi](float v) { return std::clamp(v, lo, hi); });
}

std::unique_ptr<Node> makeCurve(const Json& p, uint64_t, std::string& err) {
  std::vector<std::pair<float, float>> pts;
  if (const Json* a = p.get("points"))
    for (const Json& pt : a->arr) {
      if (pt.arr.size() != 2) { err = "curve 'points' must be [[x, y], ...]"; return nullptr; }
      pts.emplace_back(float(pt.arr[0].num), float(pt.arr[1].num));
    }
  if (pts.size() < 2) { err = "curve needs at least 2 points"; return nullptr; }
  for (size_t i = 1; i < pts.size(); ++i)
    if (!(pts[i].first > pts[i - 1].first)) { err = "curve points must have increasing x"; return nullptr; }
  const std::string mode = p.getStr("interpolation", "linear");
  if (mode != "linear" && mode != "cubic") { err = "'interpolation' must be \"linear\" or \"cubic\""; return nullptr; }
  return mapNode(Curve(std::move(pts), mode == "cubic"));
}

std::unique_ptr<Node> makeSelect(const Json& p, uint64_t, std::string& err) {
  const float falloff = float(p.getNum("falloff", 0));
  if (falloff < 0) { err = "'falloff' must be >= 0"; return nullptr; }
  return std::make_unique<SelectNode>(float(p.getNum("threshold", 0)), falloff);
}

std::unique_ptr<Node> makeBlend(const Json&, uint64_t, std::string&) { return std::make_unique<BlendNode>(); }

/// Quantizes to multiples of `step`; `smoothness` (0..1) rounds the step edges.
std::unique_ptr<Node> makeTerrace(const Json& p, uint64_t, std::string& err) {
  const float step = float(p.getNum("step", 1)), smooth = std::clamp(float(p.getNum("smoothness", 0)), 0.f, 1.f);
  if (!(step > 0)) { err = "'step' must be > 0"; return nullptr; }
  return mapNode([step, smooth](float v) {
    const float s = v / step, f = std::floor(s), t = s - f;
    const float r = smooth <= 0 ? 0.f : smoothstep01(std::clamp((t - (1.f - smooth)) / smooth, 0.f, 1.f));
    return (f + r) * step;
  });
}

std::unique_ptr<Node> makeScaleBias(const Json& p, uint64_t, std::string&) {
  const float scale = float(p.getNum("scale", 1)), bias = float(p.getNum("bias", 0));
  return mapNode([scale, bias](float v) { return v * scale + bias; });
}

std::unique_ptr<Node> makeIdentity(const Json&, uint64_t, std::string&) {
  return mapNode([](float v) { return v; });
}

const std::vector<NodeType>& nodeTypes() {
  static const std::vector<NodeType> types{
      {"constant", {}, false, {"value"}, makeConstantNode},
      {"coordinate", {}, false, {"axis"}, makeCoordinate},
      {"wave", {}, false, {"axis", "direction", "frequency", "amplitude", "phase", "function"}, makeWave},
      {"noise", {}, false, {"noise", "seed", "frequency", "offset", "dims", "w", "fractal", "warp", "cellular"}, makeNoise},
      {"lithoform", {}, false, {"theta", "phi", "radius", "falloff", "rotation", "center"}, makeLithoform},
      {"add", {}, true, {}, makeCombine<Combine::Add>},
      {"multiply", {}, true, {}, makeCombine<Combine::Multiply>},
      {"min", {}, true, {}, makeCombine<Combine::Min>},
      {"max", {}, true, {}, makeCombine<Combine::Max>},
      {"clamp", {"input"}, false, {"min", "max"}, makeClamp},
      {"curve", {"input"}, false, {"points", "interpolation"}, makeCurve},
      {"select", {"control", "a", "b"}, false, {"threshold", "falloff"}, makeSelect},
      {"blend", {"a", "b", "t"}, false, {}, makeBlend},
      {"mountain_range", {"ridges", "mask"}, false, {"amplitude", "power", "mask_low", "mask_high"},
       makeMountainRange},
      {"peak", {"distance"}, false, {"amplitude", "radius", "sharpness"}, makePeak},
      {"terrace", {"input"}, false, {"step", "smoothness"}, makeTerrace},
      {"scale_bias", {"input"}, false, {"scale", "bias"}, makeScaleBias},
      {"abs", {"input"}, false, {}, makeAbs},
      {"invert", {"input"}, false, {}, makeInvert},
      {"power", {"input"}, false, {"exponent"}, makePower},
      {"fractal", {}, false, {"noise", "seed", "frequency", "offset", "dims", "w", "fractal", "warp", "cellular"}, makeFractalNode},
      {"warp", {}, false, {"noise", "seed", "frequency", "offset", "dims", "w", "fractal", "warp", "cellular"}, makeWarpNode},
      {kCacheType, {"input"}, false, {}, makeIdentity},
  };
  return types;
}

}  // namespace

const NodeType* findNodeType(std::string_view name) {
  for (const NodeType& t : nodeTypes())
    if (t.name == name) return &t;
  return nullptr;
}

std::unique_ptr<Node> makeConstant(float value) { return std::make_unique<ConstantNode>(value); }

}  // namespace litholattice::procgen::detail
