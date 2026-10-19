// Cellular (Worley) noise: one feature point per unit cell (jittered from the cell centre),
// distances to the nearest (F1) and second nearest (F2) feature point, with Euclidean, Manhattan
// or Chebyshev distance. The search covers the 3^dims neighbouring cells and widens only when a
// farther cell could still hold a closer point, so F1 / F2 are exact (and continuous). White noise: one random value per unit cell.
#include <limits>
#include "noise_impl.h"

namespace litholattice::procgen::detail {
namespace {

class CellularNoise final : public INoise {
 public:
  explicit CellularNoise(const NoiseSettings& s)
      : seed_(s.seed), ret_(s.cellularReturn), metric_(s.metric), jitter_(std::clamp(s.jitter, 0.f, 1.f)) {}

  float sample2(float x, float y) const override { return sampleN(x, y, 0, 0, 2); }
  float sample3(float x, float y, float z) const override { return sampleN(x, y, z, 0, 3); }
  float sample4(float x, float y, float z, float w) const override { return sampleN(x, y, z, w, 4); }

  // Cell identity (2026-10-13): the lattice hash of the winning (F1) feature cell.
  uint64_t cellId2(float x, float y) const override { return cellIdN(x, y, 0, 0, 2); }
  uint64_t cellId3(float x, float y, float z) const override { return cellIdN(x, y, z, 0, 3); }
  uint64_t cellId4(float x, float y, float z, float w) const override { return cellIdN(x, y, z, w, 4); }

  NoiseRange range() const override {
    // The own cell's point bounds F1 by one cell diagonal and a neighbour's bounds F2 by two
    // (upper bounds over every dimension count).
    const float diag = maxDistance(4);
    return {0.f, ret_ == CellularReturn::F1 ? diag : 2.f * diag};
  }

 private:
  float maxDistance(int dims) const {
    switch (metric_) {
      case DistanceMetric::Manhattan: return float(dims);
      case DistanceMetric::Chebyshev: return 1.f;
      default: return std::sqrt(float(dims));
    }
  }

  float distance(const float d[4], int dims) const {
    float r = 0;
    for (int k = 0; k < dims; ++k) {
      const float a = std::abs(d[k]);
      switch (metric_) {
        case DistanceMetric::Manhattan: r += a; break;
        case DistanceMetric::Chebyshev: r = std::max(r, a); break;
        default: r += a * a; break;
      }
    }
    return metric_ == DistanceMetric::Euclidean ? std::sqrt(r) : r;
  }

  uint64_t cellIdN(float x, float y, float z, float w, int dims) const {
    int32_t win[4] = {0, 0, 0, 0};
    (void)sampleN(x, y, z, w, dims, win);
    return uint64_t(latticeHash(seed_, win[0], win[1], win[2], win[3]));
  }

  float sampleN(float x, float y, float z, float w, int dims, int32_t* winCell = nullptr) const {
    const float p[4] = {x, y, z, w};
    int32_t base[4] = {0, 0, 0, 0};
    float edge = 1.f;  // distance from p to its cell's nearest face
    for (int k = 0; k < dims; ++k) {
      base[k] = fastFloor(p[k]);
      const float t = p[k] - float(base[k]);
      edge = std::min(edge, std::min(t, 1.f - t));
    }
    float f1 = std::numeric_limits<float>::max(), f2 = f1;
    // Visit rings of cells (Chebyshev cell offset r) outward. A point in ring r lies at least
    // r - 1 + edge + (1 - jitter) / 2 away along one axis (any metric is >= that gap), so stop
    // once that bound exceeds the distance still being searched for (F1 or F2).
    for (int r = 0;; ++r) {
      const float bound = float(r) - 1.f + edge + 0.5f * (1.f - jitter_);
      if (r > 1 && bound >= (ret_ == CellularReturn::F1 ? f1 : f2)) break;
      visitRing(p, base, dims, r, f1, f2, winCell);
    }
    switch (ret_) {
      case CellularReturn::F2: return f2;
      case CellularReturn::F2MinusF1: return f2 - f1;
      default: return f1;
    }
  }

  /// Updates f1 / f2 with the feature points of every cell at Chebyshev offset exactly r.
  void visitRing(const float p[4], const int32_t base[4], int dims, int r, float& f1, float& f2,
                 int32_t* winCell) const {
    const int side = 2 * r + 1;
    int cells = 1;
    for (int k = 0; k < dims; ++k) cells *= side;
    for (int c = 0; c < cells; ++c) {
      int32_t cell[4] = {base[0], base[1], base[2], base[3]};
      int ring = 0;
      for (int k = 0, rest = c; k < dims; ++k, rest /= side) {
        const int o = rest % side - r;
        cell[k] += o;
        ring = std::max(ring, std::abs(o));
      }
      if (ring != r) continue;
      const uint32_t h = latticeHash(seed_, cell[0], cell[1], cell[2], cell[3]);
      float d[4] = {0, 0, 0, 0};
      for (int k = 0; k < dims; ++k) {
        const float j = unitFloat(uint32_t(mix64(uint64_t(h) * 4u + uint64_t(k))));  // per-axis jitter
        d[k] = p[k] - (float(cell[k]) + 0.5f + jitter_ * (j - 0.5f));
      }
      const float dist = distance(d, dims);
      if (dist < f1) {
        f2 = f1;
        f1 = dist;
        if (winCell)
          for (int k = 0; k < 4; ++k) winCell[k] = cell[k];
      } else if (dist < f2) {
        f2 = dist;
      }
    }
  }

  uint64_t seed_;
  CellularReturn ret_;
  DistanceMetric metric_;
  float jitter_;
};

class WhiteNoise final : public INoise {
 public:
  explicit WhiteNoise(uint64_t seed) : seed_(seed) {}
  float sample2(float x, float y) const override { return signedUnitFloat(latticeHash(seed_, fastFloor(x), fastFloor(y))); }
  float sample3(float x, float y, float z) const override {
    return signedUnitFloat(latticeHash(seed_, fastFloor(x), fastFloor(y), fastFloor(z)));
  }
  float sample4(float x, float y, float z, float w) const override {
    return signedUnitFloat(latticeHash(seed_, fastFloor(x), fastFloor(y), fastFloor(z), fastFloor(w)));
  }

 private:
  uint64_t seed_;
};

}  // namespace

std::unique_ptr<INoise> makeCellular(const NoiseSettings& s) { return std::make_unique<CellularNoise>(s); }
std::unique_ptr<INoise> makeWhite(uint64_t seed) { return std::make_unique<WhiteNoise>(seed); }

}  // namespace litholattice::procgen::detail
