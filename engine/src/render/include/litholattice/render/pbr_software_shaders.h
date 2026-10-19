// PBR fragment path for the software (CPU) renderer (PBR phase 5).
//
// WHAT: pbrFragmentShader - a FragmentInput-compatible fragment shader that resolves the
//   fragment's compact material ID through the MaterialRegistry, evaluates the exact
//   Cook-Torrance core (pbr_brdf.h) against the K-nearest analytic lights from the
//   LightCollector (pbr_lights.h), adds the flood-fill light-level irradiance, and
//   outputs an sRGB RGBA8 pixel. Plus SoftwareShadingConfig, the tier/config switch that
//   selects pbrFragmentShader vs the existing basicFragmentShader.
// WHY: this is the CPU proof of the whole PBR chain (registry -> BRDF -> lights ->
//   pixel) for GPU-less machines, and the reference the GPU shaders validate against.
// HOW: header-only, mirrors the pbr_lights.h / pbr_transparency_tiers.h pattern. The
//   fragment carries its compact material ID in ShaderVaryings::materialId (flat per
//   quad, copied like `layer`); the extra context (registry, collector, emissive
//   provider, light levels, camera) rides on FragmentInput::pbr. Anything missing -
//   null context, unregistered material - falls back to basicFragmentShader, so the
//   basic path stays intact and the PBR tier degrades gracefully.
//
// EMISSIVE-PROVIDER SEAM (documented stub): LightCollector needs a forEachEmissive
//   callable, but neither ILighting nor LightEngine can enumerate emissive sources
//   (see pbr_lights.h's open question). Until the world side grows a forEachEmissive
//   (chunk emissive scan or a retained light-engine source list), the software path
//   takes an IEmissiveProvider via PbrFragmentContext::emissive; a null provider yields
//   zero analytic lights and the flood-fill irradiance carries the scene. The
//   world-side forEachEmissive replaces this seam without changing this file's
//   contract.
//
// RESOLVED (PBR phase 2): ChunkEmissiveProvider (pbr_emissive_provider.h) is the
//   world-side implementation - cached chunk-material scan with explicit refresh.
//   The null-provider behaviour above is unchanged (graceful degradation).
//
// ULTRA TIER (TrueRefraction) GAP - PBR phase 2, Worker 4 (documented, not silent):
//   the software rasterizer does NOT implement the PbrTransparencyTier ladder at all
//   (this file's Pbr tier shades every fragment as opaque PBR; Simple/ScreenDoor/
//   PbrReflections/TrueRefraction are never selected here), so it cannot run the
//   Ultra tier's two-pass scene-color refraction either. The CPU reference math it
//   needs is implemented and engine-tested in pbr_transparency_tiers.h
//   (snellRefractEta / refractionScreenOffset / resolveTrueRefraction - the exact
//   mirror of the GLSL the GL33 backend runs). The follow-up is a software-renderer
//   rework: a PbrTransparencyTier selection on SoftwareRenderer plus a two-pass
//   rasterizer (opaque -> framebuffer copy -> refractive fragments sampling the
//   copy). Until then the software path stays honestly non-refractive.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>
#include "litholattice/render/software_shaders.h"
#include "litholattice/render/pbr_materials.h"
#include "litholattice/render/pbr_brdf.h"
#include "litholattice/render/pbr_lights.h"

namespace litholattice {

/// Software-renderer shading tier: which fragment shader the per-pixel path runs.
/// Default is Basic ("just works"); Pbr is opt-in until the mesher stamps material IDs.
enum class SoftwareShadingTier {
  Basic,  ///< existing basicFragmentShader: texture + diffuse + ambient
  Pbr,    ///< pbrFragmentShader below (falls back to basic per fragment without material info)
};

/// Runtime-settable tier selection for the software renderer.
struct SoftwareShadingConfig {
  SoftwareShadingTier tier = SoftwareShadingTier::Basic;  ///< user-selected tier
  void setTier(SoftwareShadingTier t) { tier = t; }
  SoftwareShadingTier getTier() const { return tier; }
};

/// Source of emissive candidates for the software PBR path. The sink signature matches
/// LightCollector::collect's forEachEmissive: (x, y, z, r, g, b, intensity), world space,
//   linear-space colour. See the file-header seam note.
struct IEmissiveProvider {
  virtual ~IEmissiveProvider() = default;
  virtual void forEachEmissive(
      const std::function<void(float x, float y, float z, float r, float g, float b,
                               float intensity)>& sink) const = 0;
};

/// Per-pixel context for pbrFragmentShader. The SoftwareRenderer owns one (or none);
//   it must outlive any render call that uses it.
struct PbrFragmentContext {
  const MaterialRegistry* registry = nullptr;  ///< compact ID -> MaterialDef (required)
  LightCollector collector;                    ///< K-nearest reduction; default K = 8
  const IEmissiveProvider* emissive = nullptr;  ///< null = no analytic lights (documented seam)
  uint8_t blockLight = 0;  ///< flood-fill block level at the fragment's block (0-64)
  uint8_t skyLight = 0;    ///< flood-fill sky level at the fragment's block (0-64)
  float camX = 0.f, camY = 8.f, camZ = 0.f;  ///< camera (world) for the view direction
  AmbientMode ambientMode = AmbientMode::Fraction;  ///< flood-fill ambient computation
  float sunX = 0.4f, sunY = 0.85f, sunZ = 0.3f;     ///< sun direction (need not be unit)
  /// Scratch for the per-fragment light collection (fix 2026-10-11: was a heap
  /// allocation per pixel). Mutable because the shader takes a const context;
  /// safe: the software rasterizer is single-threaded.
  mutable std::vector<AnalyticLight> lightScratch;
};

namespace pbr_sw_detail {
inline std::array<float, 3> add3(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
inline float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
}  // namespace pbr_sw_detail

/// Linear (0..1) -> sRGB (0..1), piecewise per the sRGB transfer function.
inline float pbrLinearToSrgb(float c) {
  const float x = pbr_sw_detail::clamp01(c);
  return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

/// PBR fragment shader for the CPU renderer. Many-lights hybrid, exactly JD's locked
//   design: the 1-64 flood-fill light level (irradianceFromLightLevel) handles diffuse
//   for unlimited lights; the K nearest analytic lights drive the specular term through
//   the exact CookTorrance::shade core; the registry's emissive term is added straight.
//   Output is sRGB RGBA8. Falls back to basicFragmentShader when the context, the
//   registry, or the material is missing (graceful degradation, never black).
inline uint32_t pbrFragmentShader(const FragmentInput& in) {
  const PbrFragmentContext* ctx = in.pbr;
  const MaterialDef* def =
      (ctx && ctx->registry) ? ctx->registry->defFor(in.varyings.materialId) : nullptr;
  if (!ctx || !def) return basicFragmentShader(in);

  // Unit surface normal (world); degenerate varyings fall back to up.
  float nx = in.varyings.nx, ny = in.varyings.ny, nz = in.varyings.nz;
  {
    const float l2 = nx * nx + ny * ny + nz * nz;
    if (l2 > 1e-12f) {
      const float s = 1.0f / std::sqrt(l2);
      nx *= s; ny *= s; nz *= s;
    } else {
      nx = 0.f; ny = 1.f; nz = 0.f;
    }
  }
  const std::array<float, 3> n{nx, ny, nz};

  // View direction: fragment -> camera (world).
  const float fx = in.varyings.wx, fy = in.varyings.wy, fz = in.varyings.wz;
  float vx = ctx->camX - fx, vy = ctx->camY - fy, vz = ctx->camZ - fz;
  {
    const float l2 = vx * vx + vy * vy + vz * vz;
    if (l2 > 1e-12f) {
      const float s = 1.0f / std::sqrt(l2);
      vx *= s; vy *= s; vz *= s;
    } else {
      vx = 0.f; vy = 1.f; vz = 0.f;
    }
  }
  const std::array<float, 3> v{vx, vy, vz};

  const float metal = def->metalness < 0.f ? 0.f : (def->metalness > 1.f ? 1.f : def->metalness);

  // Diffuse half: flood-fill irradiance. Metals get no diffuse (consistent with the
  // BRDF's kD term, which zeroes diffuse for metalness = 1).
  const Irradiance irr = irradianceFromLightLevel(ctx->blockLight, ctx->skyLight,
                                                       ctx->ambientMode, nx, ny, nz,
                                                       ctx->sunX, ctx->sunY, ctx->sunZ);
  const float kDfill = 1.0f - metal;
  std::array<float, 3> lo = {
      def->albedo[0] * (irr.diffuse + irr.ambient) * kDfill,
      def->albedo[1] * (irr.diffuse + irr.ambient) * kDfill,
      def->albedo[2] * (irr.diffuse + irr.ambient) * kDfill,
  };

  // Specular half: K nearest analytic lights through the exact Cook-Torrance core.
  // (A null emissive provider - the documented seam - simply yields no lights.)
  if (ctx->emissive) {
    std::vector<AnalyticLight>& lights = ctx->lightScratch;
    ctx->collector.collectInto(fx, fy, fz,
        [&](auto&& sink) { ctx->emissive->forEachEmissive(sink); }, lights);
    for (const AnalyticLight& l : lights) {
      float lx = l.x - fx, ly = l.y - fy, lz = l.z - fz;
      const float l2 = lx * lx + ly * ly + lz * lz;
      if (l2 <= 1e-12f) continue;  // light exactly at the fragment: no direction
      const float s = 1.0f / std::sqrt(l2);
      const std::array<float, 3> ldir{lx * s, ly * s, lz * s};
      lo = pbr_sw_detail::add3(
          lo, CookTorrance::shade(def->albedo, metal, def->roughness, n, v, ldir,
                                  attenuatedRadiance(l)));
    }
  }

  // Emissive term straight from the registry.
  for (int i = 0; i < 3; ++i) lo[i] += def->emissive[i] * def->emissiveIntensity;

  // Clamp, linear -> sRGB, pack RGBA8.
  const uint8_t r = static_cast<uint8_t>(pbrLinearToSrgb(lo[0]) * 255.0f + 0.5f);
  const uint8_t g = static_cast<uint8_t>(pbrLinearToSrgb(lo[1]) * 255.0f + 0.5f);
  const uint8_t b = static_cast<uint8_t>(pbrLinearToSrgb(lo[2]) * 255.0f + 0.5f);
  return 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

/// Tier -> fragment shader for the software per-pixel path. The basic path is untouched:
//   Basic selects basicFragmentShader exactly as before.
inline FragmentShaderFn selectFragmentShader(SoftwareShadingTier tier) {
  return tier == SoftwareShadingTier::Pbr ? &pbrFragmentShader : &basicFragmentShader;
}

}  // namespace litholattice
