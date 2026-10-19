// CPU-side Cook-Torrance BRDF: implementation. See pbr_brdf.h for the formulas,
// known answers, and documented approximations.
#include "litholattice/render/pbr_brdf.h"

#include <algorithm>
#include <cmath>

namespace litholattice {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kDielectricF0 = 0.04f;
constexpr float kMinAlpha = 1e-3f;  // roughness->alpha clamp: a perfect mirror stays finite
constexpr float kDenomEps = 1e-4f;  // specular denominator guard
}  // namespace

float CookTorrance::dot3(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::array<float, 3> CookTorrance::add3(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

std::array<float, 3> CookTorrance::mul3(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  return {a[0] * b[0], a[1] * b[1], a[2] * b[2]};
}

std::array<float, 3> CookTorrance::scale3(const std::array<float, 3>& a, float s) {
  return {a[0] * s, a[1] * s, a[2] * s};
}

std::array<float, 3> CookTorrance::halfVec(const std::array<float, 3>& v, const std::array<float, 3>& l) {
  std::array<float, 3> h = add3(v, l);
  const float len = std::sqrt(std::max(dot3(h, h), 1e-12f));
  return scale3(h, 1.0f / len);
}

std::array<float, 3> CookTorrance::fresnelSchlick(const std::array<float, 3>& f0, float cosTheta) {
  const float c = std::clamp(cosTheta, 0.0f, 1.0f);
  const float t = 1.0f - c;
  const float t5 = t * t * t * t * t;
  std::array<float, 3> one{1.0f, 1.0f, 1.0f};
  return add3(f0, scale3({one[0] - f0[0], one[1] - f0[1], one[2] - f0[2]}, t5));
}

float CookTorrance::distributionGgx(float nDotH, float roughness) {
  const float alpha = std::max(roughness * roughness, kMinAlpha);
  const float a2 = alpha * alpha;
  const float c = std::clamp(nDotH, 0.0f, 1.0f);
  const float denom = c * c * (a2 - 1.0f) + 1.0f;  // == 1 at c = 1
  return a2 / (kPi * denom * denom);
}

float CookTorrance::geometrySmith(float nDotV, float nDotL, float roughness) {
  const float k = (roughness + 1.0f) * (roughness + 1.0f) / 8.0f;  // Epic direct-lighting k
  const float g1v = nDotV / (nDotV * (1.0f - k) + k);
  const float g1l = nDotL / (nDotL * (1.0f - k) + k);
  return g1v * g1l;
}

std::array<float, 3> CookTorrance::shade(const std::array<float, 3>& albedo, float metalness,
                                         float roughness, const std::array<float, 3>& n,
                                         const std::array<float, 3>& v, const std::array<float, 3>& l,
                                         const std::array<float, 3>& lightColor) {
  const float nDotV = dot3(n, v);
  const float nDotL = dot3(n, l);
  if (nDotV <= 0.0f || nDotL <= 0.0f) return {0.0f, 0.0f, 0.0f};

  // F0: 0.04 for dielectrics, albedo-tinted for metals (albedo IS the specular colour).
  const float m = std::clamp(metalness, 0.0f, 1.0f);
  const std::array<float, 3> f0 = {
      kDielectricF0 * (1.0f - m) + albedo[0] * m,
      kDielectricF0 * (1.0f - m) + albedo[1] * m,
      kDielectricF0 * (1.0f - m) + albedo[2] * m,
  };
  const std::array<float, 3> f = fresnelSchlick(f0, nDotV);

  const std::array<float, 3> h = halfVec(v, l);
  const float nDotH = dot3(n, h);
  const float d = distributionGgx(nDotH, roughness);
  const float g = geometrySmith(nDotV, nDotL, roughness);
  const float denom = std::max(4.0f * nDotV * nDotL, kDenomEps);
  const std::array<float, 3> spec = scale3(mul3(mul3(f, {d, d, d}), {g, g, g}), 1.0f / denom);

  // Metals get zero diffuse; the (1 - F) factor keeps total reflected energy <= incoming.
  const std::array<float, 3> kD = {std::max(1.0f - f[0], 0.0f) * (1.0f - m),
                                  std::max(1.0f - f[1], 0.0f) * (1.0f - m),
                                  std::max(1.0f - f[2], 0.0f) * (1.0f - m)};
  const std::array<float, 3> diff = scale3(mul3(albedo, kD), 1.0f / kPi);

  const std::array<float, 3> brdf = add3(diff, spec);
  return scale3(mul3(brdf, lightColor), nDotL);
}

}  // namespace litholattice
