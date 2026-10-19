#include "litholattice/procgen/noise.h"
#include <array>
#include "noise_impl.h"

namespace litholattice::procgen {

void INoise::sampleBatch(int dims, const PointsView& p, std::span<float> out) const {
  const size_t n = std::min(p.size(), out.size());
  switch (dims) {
    case 2: for (size_t i = 0; i < n; ++i) out[i] = sample2(p.x[i], p.y[i]); break;
    case 3: for (size_t i = 0; i < n; ++i) out[i] = sample3(p.x[i], p.y[i], p.z[i]); break;
    default: for (size_t i = 0; i < n; ++i) out[i] = sample4(p.x[i], p.y[i], p.z[i], p.w[i]); break;
  }
}

std::unique_ptr<INoise> createNoise(const NoiseSettings& s) {
  switch (s.type) {
    case NoiseType::Perlin: return detail::makePerlin(s.seed);
    case NoiseType::OpenSimplex2F: return detail::makeOpenSimplex2(s.seed, false);
    case NoiseType::OpenSimplex2S: return detail::makeOpenSimplex2(s.seed, true);
    case NoiseType::Value: return detail::makeValue(s.seed);
    case NoiseType::Cellular: return detail::makeCellular(s);
    case NoiseType::White: return detail::makeWhite(s.seed);
  }
  return nullptr;
}

namespace {

template <typename E, size_t N>
using Names = std::array<std::pair<E, std::string_view>, N>;

constexpr Names<NoiseType, 6> kNoiseNames{{{NoiseType::Perlin, "perlin"},
                                           {NoiseType::OpenSimplex2F, "opensimplex2f"},
                                           {NoiseType::OpenSimplex2S, "opensimplex2s"},
                                           {NoiseType::Value, "value"},
                                           {NoiseType::Cellular, "cellular"},
                                           {NoiseType::White, "white"}}};
constexpr Names<CellularReturn, 3> kReturnNames{
    {{CellularReturn::F1, "f1"}, {CellularReturn::F2, "f2"}, {CellularReturn::F2MinusF1, "f2-f1"}}};
constexpr Names<DistanceMetric, 3> kMetricNames{{{DistanceMetric::Euclidean, "euclidean"},
                                                 {DistanceMetric::Manhattan, "manhattan"},
                                                 {DistanceMetric::Chebyshev, "chebyshev"}}};

template <typename E, size_t N>
std::string_view nameOf(const Names<E, N>& names, E e) {
  for (const auto& [v, n] : names)
    if (v == e) return n;
  return "?";
}

template <typename E, size_t N>
std::optional<E> parseName(const Names<E, N>& names, std::string_view s) {
  for (const auto& [v, n] : names)
    if (n == s) return v;
  return std::nullopt;
}

}  // namespace

std::string_view toString(NoiseType t) { return nameOf(kNoiseNames, t); }
std::string_view toString(CellularReturn r) { return nameOf(kReturnNames, r); }
std::string_view toString(DistanceMetric m) { return nameOf(kMetricNames, m); }
std::optional<NoiseType> parseNoiseType(std::string_view s) { return parseName(kNoiseNames, s); }
std::optional<CellularReturn> parseCellularReturn(std::string_view s) { return parseName(kReturnNames, s); }
std::optional<DistanceMetric> parseDistanceMetric(std::string_view s) { return parseName(kMetricNames, s); }

}  // namespace litholattice::procgen
