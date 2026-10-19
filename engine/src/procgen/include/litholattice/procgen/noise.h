// Noise interface and the base noise types. Every noise is seeded at construction, immutable and
// thread-safe; samples are pure functions of (seed, position). Batch sampling takes positions as
// structure-of-arrays so SIMD paths (SSE / AVX2) can be added per type without API changes;
// today every type uses the scalar loop.
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <optional>

namespace litholattice::procgen {

/// Positions as structure-of-arrays: x[i], y[i], z[i], w[i]. Axes a call does not use may be
/// empty; used axes must all have the same size.
struct PointsView {
  std::span<const float> x, y, z, w;
  size_t size() const { return x.size(); }
};

/// Nominal output interval of a noise (samples stay inside it).
struct NoiseRange {
  float min = -1.f, max = 1.f;
};

/// A seeded, immutable noise field; all methods are const and safe to call from any thread.
class INoise {
 public:
  virtual ~INoise() = default;
  virtual float sample2(float x, float y) const = 0;
  virtual float sample3(float x, float y, float z) const = 0;
  virtual float sample4(float x, float y, float z, float w) const = 0;
  virtual NoiseRange range() const { return {}; }

  /// Identity of the nearest cellular feature cell at this position (cellular noise
  /// only; 0 for every other type): a stable hash of the winning cell's lattice
  /// coordinates under this noise's seed. Two positions inside the same Voronoi cell
  /// return the same id; the id changes exactly at cell borders. Use it for per-cell
  /// randomisation (scatter, per-cell palettes) alongside the distance returns.
  virtual uint64_t cellId2(float x, float y) const { return 0; }
  virtual uint64_t cellId3(float x, float y, float z) const { return 0; }
  virtual uint64_t cellId4(float x, float y, float z, float w) const { return 0; }

  /// out[i] = sample<dims>(points[i]) for dims 2 (x, y), 3 (x, y, z) or 4 (x, y, z, w).
  /// Scalar loop by default; a type may override it with a vectorized path.
  virtual void sampleBatch(int dims, const PointsView& points, std::span<float> out) const;
};

enum class NoiseType { Perlin, OpenSimplex2F, OpenSimplex2S, Value, Cellular, White };
/// Cellular (Worley) output: distance to the nearest feature point, the second nearest, or F2 - F1.
enum class CellularReturn { F1, F2, F2MinusF1 };
enum class DistanceMetric { Euclidean, Manhattan, Chebyshev };

struct NoiseSettings {
  NoiseType type = NoiseType::OpenSimplex2S;
  uint64_t seed = 0;
  CellularReturn cellularReturn = CellularReturn::F1;  ///< cellular only
  DistanceMetric metric = DistanceMetric::Euclidean;   ///< cellular only
  float jitter = 1.f;                                  ///< cellular only: 0 = regular grid, 1 = full cell
};

/// Creates a base noise. Ranges: gradient and value noise [-1, 1]; white [-1, 1); cellular
/// [0, max distance] (see the type's range()).
std::unique_ptr<INoise> createNoise(const NoiseSettings& settings);

/// Names used in JSON ("perlin", "opensimplex2f", "opensimplex2s", "value", "cellular", "white",
/// "f1", "f2", "f2-f1", "euclidean", "manhattan", "chebyshev").
std::string_view toString(NoiseType t);
std::string_view toString(CellularReturn r);
std::string_view toString(DistanceMetric m);
std::optional<NoiseType> parseNoiseType(std::string_view s);
std::optional<CellularReturn> parseCellularReturn(std::string_view s);
std::optional<DistanceMetric> parseDistanceMetric(std::string_view s);

}  // namespace litholattice::procgen
