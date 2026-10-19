// CPU-side Cook-Torrance BRDF core (PBR phase 2).
//
// WHAT: a lean, stateless microfacet BRDF evaluator for the PBR material parameters
//   defined in pbr_materials.h (MaterialDef): Fresnel-Schlick + GGX normal distribution
//   + Schlick-GGX (Smith) geometry + Lambertian diffuse, combined into one
//   energy-conserving outgoing-radiance function.
// WHY: the many-lights hybrid in JD's locked PBR design needs specular from the K nearest
//   analytic lights; this is the reference evaluator for that term. It also serves CPU
//   fallback paths (the software renderer's basic-shader tier) and any playtesting or
//   validation tooling. GPU shaders mirror these exact formulas.
// HOW: pure functions on plain float triples (no Vec3 dependency; keeps the render lib's
//   hot path dependency-free). All inputs are assumed unit-length and in world space; the
//   sub-terms (fresnelSchlick / distributionGgx / geometrySmith) are public so tests and
//   shaders can validate each term against its known-answer form.
//
// Approximations (deliberate, documented):
//  1. Fresnel is Schlick's approximation to the exact Fresnel equations, not the full
//     form. For dielectrics with IOR ~1.5 the error vs. exact Fresnel is < 2% for
//     incidence angles below ~75 deg and stays within a few percent up to grazing.
//     (F0 = ((ior-1)/(ior+1))^2 is folded into the caller-provided F0; the dielectric
//     default is 0.04.)
//  2. Roughness is remapped alpha = roughness^2 (Disney/Epic convention), clamped to a
//     minimum of 1e-3 so a perfect mirror (roughness 0) yields a very tight lobe instead
//     of a division by zero. Below the clamp the lobe just plateaus; energy conservation
//     still holds.
//  3. The geometry term uses the Smith joint-G as the product of two Schlick-GGX terms
//     with k = (roughness + 1)^2 / 8 (the Epic direct-lighting form), not the exact
//     Smith-GGX integral. It is exact at NdotV = NdotL = 1 (returns 1.0) and tracks the
//     exact form within a few percent elsewhere.
//  4. The diffuse Fresnel factor is evaluated at the view angle (F(NdotV)) rather than
//     integrated over the lobe; per-channel (1 - F) * (1 - metalness) guarantees the
//     combined outgoing energy stays within the incoming, verified by the hemisphere
//     sweep test.
#pragma once
#include <array>

namespace litholattice {

/// Lean Cook-Torrance BRDF. All triples are linear-space RGB; N, V, L must be unit length.
class CookTorrance {
 public:
  /// Fresnel-Schlick: F0 + (1 - F0) * (1 - cosTheta)^5. Known-answer: cosTheta = 1
  /// returns exactly F0 (0.04 for dielectrics); cosTheta = 0 returns 1 (white at grazing).
  static std::array<float, 3> fresnelSchlick(const std::array<float, 3>& f0, float cosTheta);

  /// GGX normal distribution D(N,H) (Walter et al.). Exact NDF, not an approximation;
  /// cheap (~15 flops). Known-answer: NdotH = 1, roughness = 1 gives 1/pi.
  static float distributionGgx(float nDotH, float roughness);

  /// Smith geometry G(V,L) as G1(V)*G1(L) with Schlick-GGX G1 and k = (r+1)^2/8.
  /// Known-answer: NdotV = NdotL = 1 gives exactly 1.0 for any roughness.
  static float geometrySmith(float nDotV, float nDotL, float roughness);

  /// Combined outgoing radiance for one light:
  ///   Lo = (kD * albedo / pi + D * F * G / (4 * NdotV * NdotL)) * lightColor * NdotL
  /// with F0 = mix(0.04, albedo, metalness) and kD = (1 - F) * (1 - metalness), so metals
  /// get zero diffuse and dielectrics get exactly F0 = 0.04 at normal incidence.
  /// lightColor is the incoming light's radiance * attenuation/shadowing (linear RGB).
  /// Returns black when the light is behind the surface (NdotL <= 0) or the view is
  /// behind it (NdotV <= 0). Clamped to finite values for roughness = 0.
  static std::array<float, 3> shade(const std::array<float, 3>& albedo, float metalness,
                                    float roughness, const std::array<float, 3>& n,
                                    const std::array<float, 3>& v, const std::array<float, 3>& l,
                                    const std::array<float, 3>& lightColor);

 private:
  static float dot3(const std::array<float, 3>& a, const std::array<float, 3>& b);
  static std::array<float, 3> add3(const std::array<float, 3>& a, const std::array<float, 3>& b);
  static std::array<float, 3> mul3(const std::array<float, 3>& a, const std::array<float, 3>& b);
  static std::array<float, 3> scale3(const std::array<float, 3>& a, float s);
  static std::array<float, 3> halfVec(const std::array<float, 3>& v, const std::array<float, 3>& l);
};

}  // namespace litholattice
