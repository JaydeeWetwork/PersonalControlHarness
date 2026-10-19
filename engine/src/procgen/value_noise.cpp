// Value noise: random values on the integer lattice, smoothstep-interpolated (C1). 2D, 3D, 4D.
#include "noise_impl.h"

namespace litholattice::procgen::detail {
namespace {

class ValueNoise final : public INoise {
 public:
  explicit ValueNoise(uint64_t seed) : seed_(seed) {}

  float sample2(float x, float y) const override {
    const int32_t xi = fastFloor(x), yi = fastFloor(y);
    const float u = smooth(x - float(xi)), v = smooth(y - float(yi));
    return lerp(lerp(at(xi, yi, 0, 0), at(xi + 1, yi, 0, 0), u), lerp(at(xi, yi + 1, 0, 0), at(xi + 1, yi + 1, 0, 0), u), v);
  }

  float sample3(float x, float y, float z) const override { return sample4Impl(x, y, z, 0.f, 3); }
  float sample4(float x, float y, float z, float w) const override { return sample4Impl(x, y, z, w, 4); }

 private:
  float at(int32_t x, int32_t y, int32_t z, int32_t w) const { return signedUnitFloat(latticeHash(seed_, x, y, z, w)); }

  // Multilinear blend over the 2^dims corners, collapsing one axis at a time.
  float sample4Impl(float x, float y, float z, float w, int dims) const {
    const float p[4] = {x, y, z, w};
    int32_t b[4];
    float u[4];
    for (int k = 0; k < 4; ++k) {
      b[k] = k < dims ? fastFloor(p[k]) : 0;
      u[k] = k < dims ? smooth(p[k] - float(b[k])) : 0.f;
    }
    float c[16];
    const int corners = 1 << dims;
    for (int i = 0; i < corners; ++i)
      c[i] = at(b[0] + (i & 1), b[1] + ((i >> 1) & 1), b[2] + ((i >> 2) & 1), b[3] + ((i >> 3) & 1));
    for (int axis = 0, n = corners; axis < dims; ++axis, n /= 2)
      for (int i = 0; i < n / 2; ++i) c[i] = lerp(c[2 * i], c[2 * i + 1], u[axis]);
    return c[0];
  }

  uint64_t seed_;
};

}  // namespace

std::unique_ptr<INoise> makeValue(uint64_t seed) { return std::make_unique<ValueNoise>(seed); }

}  // namespace litholattice::procgen::detail
