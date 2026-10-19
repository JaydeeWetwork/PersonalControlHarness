// OpenSimplex2-style gradient noise, our own implementation of the published approach (no code
// ported from any reference):
//  - 2D: the triangular (simplex) lattice. F sums the 3 vertices of the containing triangle with
//    a radius^2 0.5 falloff; S sums every vertex within radius^2 2/3 (smoother, more overlap).
//  - 3D: the body-centred-cubic lattice (two interleaved cubic lattices offset by 1/2) in a
//    rotated frame (x+y+z along the main diagonal) to hide axis-aligned artifacts. Each lattice
//    contributes the corners of its containing cube; radius^2 0.6 (F) or 0.75 (S).
//  - 4D: the 4D simplex lattice. F sums the 5 vertices of the containing simplex (radius^2 0.6);
//    S sums every vertex within radius^2 0.75 from the surrounding 4^4 lattice block.
// Kernel (r^2 - d^2)^4 * dot(gradient, d); every vertex that can reach the sample is included,
// so the result is continuous. Simplex-noise patents (US 6,867,776) expired in 2022; the 3D
// BCC lattice avoids them regardless.
#include <array>
#include <numbers>
#include "noise_impl.h"

namespace litholattice::procgen::detail {
namespace {

// Output scales measured so the extremes land near +-1 (then clamped).
constexpr float kScale2F = 1.f / 0.0100f, kScale2S = 1.f / 0.0548f;
constexpr float kScale3F = 1.f / 0.0250f, kScale3S = 1.f / 0.0862f;
constexpr float kScale4F = 1.f / 0.0212f, kScale4S = 1.f / 0.0726f;

struct Grad2 { float x, y; };
struct Grad3 { float x, y, z; };
struct Grad4 { float x, y, z, w; };

/// 24 unit vectors evenly spread around the circle.
const std::array<Grad2, 24>& gradients2() {
  static const std::array<Grad2, 24> g = [] {
    std::array<Grad2, 24> a{};
    for (size_t i = 0; i < a.size(); ++i) {
      const double t = (double(i) + 0.5) * 2.0 * std::numbers::pi / double(a.size());
      a[i] = {float(std::cos(t)), float(std::sin(t))};
    }
    return a;
  }();
  return g;
}

/// The 12 cube-edge directions and 8 corner directions, normalized (20 directions).
const std::array<Grad3, 20>& gradients3() {
  static const std::array<Grad3, 20> g = [] {
    std::array<Grad3, 20> a{};
    size_t n = 0;
    const float e = 1.f / std::sqrt(2.f), c = 1.f / std::sqrt(3.f);
    for (int s1 : {-1, 1})
      for (int s2 : {-1, 1}) {
        a[n++] = {s1 * e, s2 * e, 0};
        a[n++] = {s1 * e, 0, s2 * e};
        a[n++] = {0, s1 * e, s2 * e};
      }
    for (int sx : {-1, 1})
      for (int sy : {-1, 1})
        for (int sz : {-1, 1}) a[n++] = {sx * c, sy * c, sz * c};
    return a;
  }();
  return g;
}

/// The 32 edge directions of the 4-cube (one zero coordinate), normalized.
const std::array<Grad4, 32>& gradients4() {
  static const std::array<Grad4, 32> g = [] {
    std::array<Grad4, 32> a{};
    size_t n = 0;
    const float k = 1.f / std::sqrt(3.f);
    for (int zero = 0; zero < 4; ++zero)
      for (int s = 0; s < 8; ++s) {
        float v[4];
        for (int i = 0, b = 0; i < 4; ++i) v[i] = i == zero ? 0.f : ((s >> b++) & 1 ? -k : k);
        a[n++] = {v[0], v[1], v[2], v[3]};
      }
    return a;
  }();
  return g;
}

inline float falloff(float r2, float d2) {
  const float t = r2 - d2;
  return t > 0 ? (t * t) * (t * t) : 0.f;
}

constexpr float kF2 = 0.36602540378f;  // (sqrt(3) - 1) / 2
constexpr float kG2 = 0.21132486540f;  // (3 - sqrt(3)) / 6
constexpr float kF4 = 0.30901699437f;  // (sqrt(5) - 1) / 4
constexpr float kG4 = 0.13819660113f;  // (5 - sqrt(5)) / 20

class OpenSimplex2 final : public INoise {
 public:
  OpenSimplex2(uint64_t seed, bool smooth) : seed_(seed), smooth_(smooth) {}

  float sample2(float x, float y) const override {
    const float s = (x + y) * kF2;
    const int32_t i = fastFloor(x + s), j = fastFloor(y + s);
    float sum = 0;
    const auto vertex = [&](int32_t vi, int32_t vj, float r2) {
      const float t = float(vi + vj) * kG2;
      const float dx = x - (float(vi) - t), dy = y - (float(vj) - t);
      const float f = falloff(r2, dx * dx + dy * dy);
      if (f == 0.f) return;
      const Grad2& g = gradients2()[latticeHash(seed_, vi, vj) % 24];
      sum += f * (g.x * dx + g.y * dy);
    };
    if (smooth_) {
      for (int32_t dj = -1; dj <= 2; ++dj)
        for (int32_t di = -1; di <= 2; ++di) vertex(i + di, j + dj, 2.f / 3.f);
      return clampUnit(sum * kScale2S);
    }
    const float t = float(i + j) * kG2, x0 = x - (float(i) - t), y0 = y - (float(j) - t);
    vertex(i, j, 0.5f);
    if (x0 > y0) vertex(i + 1, j, 0.5f);
    else vertex(i, j + 1, 0.5f);
    vertex(i + 1, j + 1, 0.5f);
    return clampUnit(sum * kScale2F);
  }

  float sample3(float x, float y, float z) const override {
    // Rotate so the lattice's main diagonal maps to the y axis (orthonormal transform).
    const float r = (x + y + z) * (2.f / 3.f);
    const float px = r - x, py = r - y, pz = r - z;
    const float r2 = smooth_ ? 0.75f : 0.6f;
    float sum = 0;
    for (int lattice = 0; lattice < 2; ++lattice) {
      const float off = lattice ? 0.5f : 0.f;
      const int32_t bx = fastFloor(px - off), by = fastFloor(py - off), bz = fastFloor(pz - off);
      for (int c = 0; c < 8; ++c) {
        const int32_t vx = bx + (c & 1), vy = by + ((c >> 1) & 1), vz = bz + ((c >> 2) & 1);
        const float dx = px - (float(vx) + off), dy = py - (float(vy) + off), dz = pz - (float(vz) + off);
        const float f = falloff(r2, dx * dx + dy * dy + dz * dz);
        if (f == 0.f) continue;
        const Grad3& g = gradients3()[latticeHash(seed_, vx, vy, vz, lattice) % 20];
        sum += f * (g.x * dx + g.y * dy + g.z * dz);
      }
    }
    return clampUnit(sum * (smooth_ ? kScale3S : kScale3F));
  }

  float sample4(float x, float y, float z, float w) const override {
    const float s = (x + y + z + w) * kF4;
    const int32_t b[4] = {fastFloor(x + s), fastFloor(y + s), fastFloor(z + s), fastFloor(w + s)};
    const float p[4] = {x, y, z, w};
    float sum = 0;
    const auto vertex = [&](const int32_t v[4], float r2) {
      const float t = float(v[0] + v[1] + v[2] + v[3]) * kG4;
      float d[4], d2 = 0;
      for (int k = 0; k < 4; ++k) {
        d[k] = p[k] - (float(v[k]) - t);
        d2 += d[k] * d[k];
      }
      const float f = falloff(r2, d2);
      if (f == 0.f) return;
      const Grad4& g = gradients4()[latticeHash(seed_, v[0], v[1], v[2], v[3]) % 32];
      sum += f * (g.x * d[0] + g.y * d[1] + g.z * d[2] + g.w * d[3]);
    };
    if (smooth_) {
      for (int c = 0; c < 256; ++c) {
        const int32_t v[4] = {b[0] + (c & 3) - 1, b[1] + ((c >> 2) & 3) - 1, b[2] + ((c >> 4) & 3) - 1,
                              b[3] + ((c >> 6) & 3) - 1};
        vertex(v, 0.75f);
      }
      return clampUnit(sum * kScale4S);
    }
    // Containing simplex: walk from the base vertex along the axes in order of decreasing
    // offset (the classic rank ordering), one unit step per vertex.
    const float t = float(b[0] + b[1] + b[2] + b[3]) * kG4;
    float o[4];
    for (int k = 0; k < 4; ++k) o[k] = p[k] - (float(b[k]) - t);
    int order[4] = {0, 1, 2, 3};
    std::sort(order, order + 4, [&](int a, int c) { return o[a] > o[c] || (o[a] == o[c] && a < c); });
    int32_t v[4] = {b[0], b[1], b[2], b[3]};
    vertex(v, 0.6f);
    for (const int axis : order) {
      ++v[axis];
      vertex(v, 0.6f);
    }
    return clampUnit(sum * kScale4F);
  }

 private:
  uint64_t seed_;
  bool smooth_;
};

}  // namespace

std::unique_ptr<INoise> makeOpenSimplex2(uint64_t seed, bool smooth) {
  return std::make_unique<OpenSimplex2>(seed, smooth);
}

}  // namespace litholattice::procgen::detail
