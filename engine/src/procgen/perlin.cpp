// Improved Perlin noise (Ken Perlin, "Improving Noise", SIGGRAPH 2002): quintic fade, gradient
// selection from a permutation table. Our own implementation; the permutation is shuffled from
// the seed, and 2D / 4D use the analogous axis-and-diagonal gradient sets.
#include <array>
#include "noise_impl.h"

namespace litholattice::procgen::detail {
namespace {

// Output scales measured so the extremes land near +-1 (then clamped).
constexpr float kScale2 = 1.0f / 1.00f;
constexpr float kScale3 = 1.0f / 0.99f;
constexpr float kScale4 = 1.0f / 1.13f;

float grad2(uint32_t h, float x, float y) {
  switch (h & 7) {
    case 0: return x + y;
    case 1: return -x + y;
    case 2: return x - y;
    case 3: return -x - y;
    case 4: return x;
    case 5: return -x;
    case 6: return y;
    default: return -y;
  }
}

float grad3(uint32_t h, float x, float y, float z) {  // Perlin's 12 edge directions (+4 repeats)
  const uint32_t g = h & 15;
  const float u = g < 8 ? x : y, v = g < 4 ? y : (g == 12 || g == 14 ? x : z);
  return ((g & 1) ? -u : u) + ((g & 2) ? -v : v);
}

float grad4(uint32_t h, float x, float y, float z, float w) {  // 32 edge directions of the 4-cube
  const uint32_t g = h & 31;
  const float a = g < 24 ? x : y, b = g < 16 ? y : z, c = g < 8 ? z : w;
  return ((g & 1) ? -a : a) + ((g & 2) ? -b : b) + ((g & 4) ? -c : c);
}

class Perlin final : public INoise {
 public:
  explicit Perlin(uint64_t seed) {
    std::array<uint8_t, 256> p{};
    for (int i = 0; i < 256; ++i) p[size_t(i)] = uint8_t(i);
    Rng rng(seed);
    for (int i = 255; i > 0; --i) std::swap(p[size_t(i)], p[size_t(rng.range(0, i))]);
    for (size_t i = 0; i < 512; ++i) perm_[i] = p[i & 255];
  }

  float sample2(float x, float y) const override {
    const int32_t xi = fastFloor(x), yi = fastFloor(y);
    const float xf = x - float(xi), yf = y - float(yi), u = quintic(xf), v = quintic(yf);
    const uint32_t X = uint32_t(xi) & 255, Y = uint32_t(yi) & 255;
    const uint32_t a = perm_[X] + Y, b = perm_[X + 1] + Y;
    const float n = lerp(lerp(grad2(perm_[a], xf, yf), grad2(perm_[b], xf - 1, yf), u),
                         lerp(grad2(perm_[a + 1], xf, yf - 1), grad2(perm_[b + 1], xf - 1, yf - 1), u), v);
    return clampUnit(n * kScale2);
  }

  float sample3(float x, float y, float z) const override {
    const int32_t xi = fastFloor(x), yi = fastFloor(y), zi = fastFloor(z);
    const float xf = x - float(xi), yf = y - float(yi), zf = z - float(zi);
    const float u = quintic(xf), v = quintic(yf), w = quintic(zf);
    const uint32_t X = uint32_t(xi) & 255, Y = uint32_t(yi) & 255, Z = uint32_t(zi) & 255;
    const uint32_t A = perm_[X] + Y, AA = perm_[A] + Z, AB = perm_[A + 1] + Z;
    const uint32_t B = perm_[X + 1] + Y, BA = perm_[B] + Z, BB = perm_[B + 1] + Z;
    const float n =
        lerp(lerp(lerp(grad3(perm_[AA], xf, yf, zf), grad3(perm_[BA], xf - 1, yf, zf), u),
                  lerp(grad3(perm_[AB], xf, yf - 1, zf), grad3(perm_[BB], xf - 1, yf - 1, zf), u), v),
             lerp(lerp(grad3(perm_[AA + 1], xf, yf, zf - 1), grad3(perm_[BA + 1], xf - 1, yf, zf - 1), u),
                  lerp(grad3(perm_[AB + 1], xf, yf - 1, zf - 1), grad3(perm_[BB + 1], xf - 1, yf - 1, zf - 1), u), v),
             w);
    return clampUnit(n * kScale3);
  }

  float sample4(float x, float y, float z, float w) const override {
    const int32_t xi = fastFloor(x), yi = fastFloor(y), zi = fastFloor(z), wi = fastFloor(w);
    const float f[4] = {x - float(xi), y - float(yi), z - float(zi), w - float(wi)};
    const uint32_t I[4] = {uint32_t(xi) & 255, uint32_t(yi) & 255, uint32_t(zi) & 255, uint32_t(wi) & 255};
    float corner[16];
    for (int c = 0; c < 16; ++c) {
      const uint32_t dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1, dw = (c >> 3) & 1;
      const uint32_t h = perm_[perm_[perm_[perm_[I[0] + dx] + I[1] + dy] + I[2] + dz] + I[3] + dw];
      corner[c] = grad4(h, f[0] - float(dx), f[1] - float(dy), f[2] - float(dz), f[3] - float(dw));
    }
    const float u[4] = {quintic(f[0]), quintic(f[1]), quintic(f[2]), quintic(f[3])};
    for (int axis = 0, n = 16; axis < 4; ++axis, n /= 2)  // collapse one axis at a time
      for (int c = 0; c < n / 2; ++c) corner[c] = lerp(corner[2 * c], corner[2 * c + 1], u[axis]);
    return clampUnit(corner[0] * kScale4);
  }

 private:
  std::array<uint32_t, 512> perm_{};
};

}  // namespace

std::unique_ptr<INoise> makePerlin(uint64_t seed) { return std::make_unique<Perlin>(seed); }

}  // namespace litholattice::procgen::detail
