// PBR transparency tiers (phase 1): user-selectable quality ladder for transparent shading.
//
// WHAT: PbrTransparencyTier { Simple, ScreenDoor, PbrReflections, TrueRefraction } — the four
//   user-selectable transparent tiers in JD's locked order — plus TransparencyTierConfig
//   (runtime-settable selection), a per-fragment CPU entry point shadeTransparent(), and the
//   TrueRefraction stub seam (TrueRefractionRequest) that phase 2's Vulkan path will fill in.
// WHY: JD's locked rule — tier = global quality ladder ("just works" -> Ultra), material
//   method = per-material behavior. The ladder is user-selectable at runtime; the engine
//   default is Simple so glass "just works" on any GPU. TrueRefraction needs a scene-color
//   copy: phase 2 (Worker 4) implemented it on the GL33 path (two-pass draw +
//   glCopyTexSubImage2D copy + mip-chain roughness blur). The CPU reference math
//   (refractionScreenOffset / resolveTrueRefraction) mirrors the GLSL term for term and
//   is engine-tested. shadeTransparent()'s TrueRefraction branch still returns the
//   request (black color, refractionRequested) because the reference context carries no
//   scene buffer - the renderer resolves it via the helpers above.
// HOW: header-only, renderer-agnostic CPU reference (mirrors the pbr_lights.h pattern).
//   shadeTransparent() composes MaterialDef params with the existing transparency
//   machinery (screenDoorTest, sortTransparent, oitWeight from transparency.h) and the
//   Cook-Torrance core (CookTorrance::shade from pbr_brdf.h) for the PBR tiers. GPU
//   shaders mirror these exact formulas.
//
// BACKEND AVAILABILITY (exact claim, P3-3 Appendix D M7 — do not widen):
//   - GL33 + BinaryQuad (PbrChunkMesh): ALL FOUR tiers implemented
//     (Simple/ScreenDoor/PbrReflections via the uPbrTier uniform;
//     TrueRefraction as the two-pass scene-color path, single refractive
//     layer — see the phase-2 seam note below).
//   - CPU shadeTransparent() (this file): Simple/ScreenDoor/PbrReflections
//     per fragment; TrueRefraction returns a REQUEST (refractionRequested,
//     black color) for a renderer to resolve — it does not refract.
//   - Software rasterizer: NO tier ladder (PBR fragments shade opaque;
//     pbr_software_shaders.h ULTRA TIER GAP).
//   - Vulkan: no PBR chunk path at all.
//   Any doc or header saying "tiers 1..4 implemented" without the
//   GL33+BinaryQuad qualifier is overstated.
//
// TIER vs MATERIAL METHOD (locked relationship):
//   - Tier is global and user-selectable (this file): the quality ladder.
//   - MaterialDef::transparency (TransparencyMethod in transparency.h) is per-material:
//     how THAT material behaves (dithered glass, sorted alpha water, unsorted weighted
//     water, additive glow).
//   - They compose via tierTransparencyMethod(): the tier refines which method the
//     renderer actually uses for a material. Simple honors per-material methods exactly
//     (today's behavior); ScreenDoor funnels blend methods into the PBR + 4x4 Bayer
//     opaque pipeline; PbrReflections keeps each method but shades through the PBR
//     core; TrueRefraction routes dielectric methods to the refraction-request path.
//   - Additive is tier-independent: glow/particles never refract and never dither — the
//     renderer keeps its additive blend state at every tier.
// SORTING per tier (the renderer's job, not this file's):
//   - Simple: sortTransparent() back-to-front for AlphaBlend materials; WeightedOIT and
//     Additive are unsorted by design.
//   - ScreenDoor: no sorting (opaque pipeline + discard).
//   - PbrReflections: back-to-front sort for AlphaBlend-method materials so the simple
//     alpha transmission stacks correctly; WeightedOIT remains the unsorted alternative.
//   - TrueRefraction (phase 2): refraction requests resolve FRONT-to-back — nearest
//     layer first, each sampling the scene-color copy that already contains the nearer
//     layers. Phase 2 must decide whether refractive surfaces draw in a separate
//     front-to-back pass or reuse the sortTransparent list reversed.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include "litholattice/render/pbr_brdf.h"
#include "litholattice/render/pbr_materials.h"
#include "litholattice/render/transparency_method.h"

namespace litholattice {

/// User-selectable transparent-shading quality ladder, in JD's locked order.
/// Enum order is the quality order; tests assert Simple < ScreenDoor < PbrReflections <
/// TrueRefraction.
enum class PbrTransparencyTier {
  Simple,          ///< classic alpha blend via existing machinery, no PBR — "just works" default
  ScreenDoor,      ///< full PBR (opaque pipeline) + 4x4 Bayer discard — PBR glass, no sorting
  PbrReflections,  ///< PBR specular reflection + simple alpha transmission (no refraction)
  TrueRefraction,  ///< refraction via scene-color sampling — implemented on GL33+BinaryQuad
                   ///< (two-pass, single layer); request-stub in the CPU helper; absent on
                   ///< the software and Vulkan paths (see BACKEND AVAILABILITY above)
};

/// Runtime-settable tier selection. Default is Simple ("just works").
struct TransparencyTierConfig {
  PbrTransparencyTier tier = PbrTransparencyTier::Simple;  ///< user-selected tier
  void setTier(PbrTransparencyTier t) { tier = t; }
  PbrTransparencyTier getTier() const { return tier; }
};

/// Display/log name for a tier (UI dropdown, config files).
inline const char* pbrTransparencyTierName(PbrTransparencyTier tier) {
  switch (tier) {
    case PbrTransparencyTier::Simple: return "Simple";
    case PbrTransparencyTier::ScreenDoor: return "ScreenDoor";
    case PbrTransparencyTier::PbrReflections: return "PbrReflections";
    case PbrTransparencyTier::TrueRefraction: return "TrueRefraction";
  }
  return "Unknown";
}

/// Maps (tier, per-material method) -> the method the renderer actually uses.
/// Documents how the global quality ladder composes with per-material behavior.
/// Additive is tier-independent (glow/particles); opaque materials never reach here.
inline TransparencyMethod tierTransparencyMethod(PbrTransparencyTier tier,
                                                 TransparencyMethod materialMethod) {
  switch (tier) {
    case PbrTransparencyTier::Simple:
      return materialMethod;  // honor per-material methods exactly (today's behavior)
    case PbrTransparencyTier::ScreenDoor:
      // Everything except glow goes through the PBR + Bayer opaque pipeline.
      return materialMethod == TransparencyMethod::Additive ? TransparencyMethod::Additive
                                                            : TransparencyMethod::ScreenDoor;
    case PbrTransparencyTier::PbrReflections:
      // Each method keeps its blend strategy; the shading itself goes through the PBR core.
      return materialMethod;
    case PbrTransparencyTier::TrueRefraction:
      // Phase 1: dielectric methods route to the refraction-request path; glow never refracts.
      return materialMethod;
  }
  return materialMethod;
}

/// Per-fragment context a renderer supplies to shadeTransparent(). All triples are
/// linear-space RGB; normal/viewDir/lightDir must be unit length, world space.
struct TransparentShadingContext {
  std::array<float, 3> normal{0.f, 1.f, 0.f};     ///< unit surface normal (world)
  std::array<float, 3> viewDir{0.f, 1.f, 0.f};    ///< unit fragment -> camera (world)
  std::array<float, 3> lightDir{0.f, 1.f, 0.f};   ///< unit fragment -> light (world)
  std::array<float, 3> lightColor{1.f, 1.f, 1.f};  ///< incoming radiance * attenuation/shadow
  std::array<float, 3> background{0.f, 0.f, 0.f};  ///< scene color behind the surface
                                                  ///< (for alpha transmission)
  int pixelX = 0;                                 ///< screen x (dither / refraction sampling)
  int pixelY = 0;                                 ///< screen y (dither / refraction sampling)
};

// ---------------------------------------------------------------------------
// TRUE REFRACTION — PHASE 2 SEAM.
//
// Phase 1 (this file) only BUILT the request: shaded surface point + refracted ray
// direction from Snell's law (real math, entry assumption air -> material). Phase 2
// (Worker 4) implemented the GL33 path for real: the scene-color copy + sampling live
// in chunk_pbr.frag (uniform `uSceneColor`) and gl33_renderer.cpp (two-pass draw,
// glCopyTexSubImage2D copy, mip chain for the roughness blur); the CPU reference math
// (snellRefractEta / refractionScreenOffset / resolveTrueRefraction) mirrors the GLSL
// term for term and is covered by engine tests. What remains Vulkan-side:
//   1. vk/chunk_pbr.vert/.frag still to write (SPIR-V built in at compile time).
//   2. The software rasterizer has no PbrTransparencyTier selection and no two-pass
//      scene-color copy, so it cannot run the Ultra tier yet (documented gap in
//      pbr_software_shaders.h; the CPU reference above is the tested contract the
//      follow-up wires in).
// Sorting interaction (unchanged from phase 1): requests resolve FRONT-to-back -
// nearest refractive layer first, each sampling the scene-color copy that already
// contains the nearer layers. The GL33 implementation takes the standard real-time
// compromise: a SINGLE refractive layer (the copy holds the opaque composite; nested
// transmissive surfaces resolve to the nearest layer via depth testing).
// ---------------------------------------------------------------------------

/// Stub request the Vulkan path resolves in phase 2. Everything it needs is here:
/// where the surface is, which way the refracted ray goes, and which pixel to write.
struct TrueRefractionRequest {
  std::array<float, 3> surfacePoint{0.f, 0.f, 0.f};  ///< shaded point, world space
  std::array<float, 3> refractDir{0.f, 0.f, -1.f};    ///< normalized Snell refracted ray
  float ior = 1.5f;                                  ///< material IOR used for the ray
  bool totalInternalReflection = false;              ///< true if Snell failed (see snellRefract)
  int pixelX = 0;                                    ///< screen x of the requesting fragment
  int pixelY = 0;                                    ///< screen y of the requesting fragment
};

/// Result of one transparent fragment: either a final color, a discard, or a stub request.
struct TransparencyResult {
  std::array<float, 3> color{0.f, 0.f, 0.f};  ///< final linear RGB (valid unless discarded
                                              ///< or refractionRequested)
  bool discarded = false;                     ///< screen-door dropped the fragment
  bool refractionRequested = false;           ///< TrueRefraction tier: color is NOT valid;
                                              ///< the renderer must resolve `refraction`
  TrueRefractionRequest refraction;           ///< valid only when refractionRequested
};

/// Eta-explicit Snell refraction: the TIR-capable core. `snellRefract` below is the
/// air-entry wrapper (eta = 1/ior); tests drive the TIR branch through this entry
/// point with eta > 1 (the exit case, e.g. glass -> air). GLSL mirror:
/// pbrSnellRefract in pbr_shading.glsl (same formula, same TIR -> mirror rule).
inline std::array<float, 3> snellRefractEta(const std::array<float, 3>& viewDir,
                                            const std::array<float, 3>& normal, float eta,
                                            bool* totalInternalReflection = nullptr) {
  float cosI = viewDir[0] * normal[0] + viewDir[1] * normal[1] + viewDir[2] * normal[2];
  cosI = cosI < -1.f ? -1.f : (cosI > 1.f ? 1.f : cosI);
  const float sin2T = eta * eta * (1.0f - cosI * cosI);
  const bool tir = sin2T > 1.0f;
  if (totalInternalReflection) *totalInternalReflection = tir;
  std::array<float, 3> dir{};
  if (tir) {
    // Mirror reflection of the incident ray (-viewDir) about the normal.
    for (int i = 0; i < 3; ++i) dir[i] = -viewDir[i] + 2.0f * cosI * normal[i];
  } else {
    const float cosT = std::sqrt(1.0f - sin2T);  // sin2T <= 1 here (TIR handled above)
    for (int i = 0; i < 3; ++i) dir[i] = eta * -viewDir[i] + (eta * cosI - cosT) * normal[i];
  }
  return dir;
}

/// Snell refraction for the stub request: air (ior ~ 1) -> material with index `ior`.
/// viewDir is fragment -> camera (unit), normal is the unit surface normal (world).
/// Returns the normalized refracted ray; sets *totalInternalReflection when sin^2(T) > 1,
/// in which case the returned direction is the mirror reflection instead (real math, not
/// a placeholder: TIR reflects, and phase 2's sampler must handle the TIR flag).
/// NOTE: with the air-entry assumption (eta = 1/ior < 1) TIR is unreachable; the TIR
/// branch exists for the eta > 1 exit case via snellRefractEta (tests + the GL33
/// sampler share it).
inline std::array<float, 3> snellRefract(const std::array<float, 3>& viewDir,
                                         const std::array<float, 3>& normal, float ior,
                                         bool* totalInternalReflection = nullptr) {
  const float eta = 1.0f / (ior > 1e-3f ? ior : 1e-3f);
  return snellRefractEta(viewDir, normal, eta, totalInternalReflection);
}

/// Dielectric normal-incidence reflectance F0 = ((ior - 1) / (ior + 1))^2.
/// IOR 1.52 (glass) -> ~0.0426, the "0.04" of the PBR shorthand.
inline float dielectricF0(float ior) {
  const float r = (ior - 1.0f) / (ior + 1.0f);
  return r * r;
}

// ---------------------------------------------------------------------------
// TRUE REFRACTION — PHASE 2 CPU REFERENCE (Worker 4, 2026-10-11).
//
// The GL33 backend implements this path for real (scene-color copy + sampling in
// chunk_pbr.frag, copy pass in gl33_renderer.cpp). These helpers are the CPU mirror
// of the GLSL (pbrSnellRefract / pbrRefractionUvOffset / pbrRefractSample in
// pbr_shading.glsl), tested headless with known answers. The software rasterizer
// does NOT run them yet: it has no PbrTransparencyTier selection and no two-pass
// scene-color copy (documented gap, see pbr_software_shaders.h) - that is the
// follow-up, not a silent divergence.
// ---------------------------------------------------------------------------

/// Scene-color sampling constants (mirrors PBR_REFRACTION_* in pbr_shading.glsl).
constexpr float kRefractionProbeDist = 1.0f;    ///< world units along the ray (voxel scale)
constexpr float kRefractionMaxOffsetPx = 96.0f; ///< screen-space offset cap (pixels)
constexpr float kRefractionMaxLod = 4.0f;       ///< roughness 1 -> ~1/16-res mip level

/// Screen-space UV offset for a refracted ray: project the surface point and a probe
/// point kRefractionProbeDist along the ray, take the UV difference, clamp the pixel
/// length to kRefractionMaxOffsetPx. viewProj is column-major 4x4 (the GL convention,
/// as passed to glUniformMatrix4fv with transpose = false). Returns (0,0) when either
/// projected point is at/behind the camera (no sane offset; never invent one).
/// GLSL mirror: pbrRefractionUvOffset in pbr_shading.glsl.
inline std::array<float, 2> refractionScreenOffset(const std::array<float, 3>& worldPos,
                                                   const std::array<float, 3>& refrDir,
                                                   const std::array<float, 16>& viewProj,
                                                   float widthPx, float heightPx) {
  // Project (x, y, z): clip = M * (x,y,z,1); returns ndc xy and clip w.
  auto project = [&](float x, float y, float z, float& ndcX, float& ndcY, float& w) {
    const float cx = viewProj[0] * x + viewProj[4] * y + viewProj[8] * z + viewProj[12];
    const float cy = viewProj[1] * x + viewProj[5] * y + viewProj[9] * z + viewProj[13];
    w = viewProj[3] * x + viewProj[7] * y + viewProj[11] * z + viewProj[15];
    ndcX = cx;
    ndcY = cy;
  };
  float x0, y0, w0, x1, y1, w1;
  project(worldPos[0], worldPos[1], worldPos[2], x0, y0, w0);
  project(worldPos[0] + refrDir[0] * kRefractionProbeDist,
          worldPos[1] + refrDir[1] * kRefractionProbeDist,
          worldPos[2] + refrDir[2] * kRefractionProbeDist, x1, y1, w1);
  if (w0 <= 1e-6f || w1 <= 1e-6f) return {0.0f, 0.0f};
  const float u0 = x0 / w0 * 0.5f + 0.5f, v0 = y0 / w0 * 0.5f + 0.5f;
  const float u1 = x1 / w1 * 0.5f + 0.5f, v1 = y1 / w1 * 0.5f + 0.5f;
  float duPx = (u1 - u0) * widthPx, dvPx = (v1 - v0) * heightPx;
  const float len = std::sqrt(duPx * duPx + dvPx * dvPx);
  if (len > kRefractionMaxOffsetPx && len > 1e-9f) {
    const float s = kRefractionMaxOffsetPx / len;
    duPx *= s;
    dvPx *= s;
  }
  return {duPx / widthPx, dvPx / heightPx};
}

/// sRGB (0..1) -> linear, the inverse of the pbrLinearToSrgb transfer function.
/// GLSL mirror: pbrSrgbToLinear in pbr_shading.glsl.
inline float srgbToLinear(float c) {
  const float x = c < 0.f ? 0.f : (c > 1.f ? 1.f : c);
  return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}

/// Ultra-tier refraction resolve: the Fresnel energy split over a scene-color sample.
///   final = specular(F-weighted) + (1 - F) * transmittedLinear + emissive
/// `specular` is the F-weighted specular radiance (mirrors the GLSL, which accumulates
/// pbrSpecularBrdf per analytic light); `transmittedLinear` is the sRGB->linear scene
/// sample along the Snell ray. Energy: the F/(1-F) weights sum to 1 by construction
/// (phase-1 discipline); the small diffuse term is folded out for transmissive
/// dielectrics (documented in pbr_shading.glsl).
inline std::array<float, 3> resolveTrueRefraction(const std::array<float, 3>& f0, float nDotV,
                                                 const std::array<float, 3>& specular,
                                                 const std::array<float, 3>& transmittedLinear,
                                                 const std::array<float, 3>& emissive) {
  const std::array<float, 3> F = CookTorrance::fresnelSchlick(f0, nDotV);
  std::array<float, 3> out{};
  for (int i = 0; i < 3; ++i)
    out[i] = specular[i] + (1.0f - F[i]) * transmittedLinear[i] + emissive[i];
  return out;
}

/// Two-pass plumbing predicate (mirrors the uRefractionPass discard logic in
/// chunk_pbr.frag): pass 0 builds the scene-color copy from opaque fragments only;
/// pass 1 resolves refractive fragments only. Opaque (alpha >= 1) fragments never
/// refract; refractive fragments never land in the copy.
inline bool refractionPassKeepsFragment(float alpha, int pass) {
  return pass == 0 ? alpha >= 1.0f : alpha < 1.0f;
}

/// Reflection/transmission energy split for the PbrReflections tier.
///   reflected   = F            (Schlick Fresnel at the view angle; F0 = mix(0.04, albedo,
///                               metalness), same F0 convention as CookTorrance::shade)
///   transmitted = (1 - F) * alpha   (simple alpha transmission, NO refraction bend)
/// Energy-conserving by construction: reflected + transmitted <= 1, and the BRDF side
/// (pbr_brdf.h) never emits more than it receives. The Ultra tier will put the Fresnel
/// term on the transmitted side too (refraction); this tier deliberately does not.
struct ReflectionTransmissionSplit {
  float reflected = 0.04f;    ///< Schlick Fresnel fraction at this view angle
  float transmitted = 0.96f;  ///< (1 - reflected) * alpha
};

inline ReflectionTransmissionSplit splitReflectionTransmission(float f0, float nDotV, float alpha) {
  const float f = CookTorrance::fresnelSchlick({f0, f0, f0}, nDotV)[0];
  ReflectionTransmissionSplit s;
  s.reflected = f;
  s.transmitted = (1.0f - f) * alpha;
  return s;
}

/// Material alpha (0..1 float) -> the 0..255 byte screenDoorTest expects.
inline uint8_t alphaToByte(float alpha) {
  const float c = alpha < 0.f ? 0.f : (alpha > 1.f ? 1.f : alpha);
  return static_cast<uint8_t>(c * 255.0f);
}

/// Single entry point: shade one transparent fragment under the selected tier.
///
/// - Opaque materials (alpha >= 1) bypass the framework at every tier: full PBR shade,
///   never discarded, never a stub request.
/// - Additive materials are tier-independent: flat (non-PBR) shade; the renderer keeps
///   its additive blend state (additive glow never dithers and never refracts).
/// - Simple: classic alpha blend — albedo * alpha + background * (1 - alpha) — no PBR.
///   The renderer still orders fragments with sortTransparent() for AlphaBlend materials.
/// - ScreenDoor: full PBR shade (CookTorrance::shade + emissive) + 4x4 Bayer discard via
///   screenDoorTest. No sorting needed (opaque pipeline).
/// - PbrReflections: PBR reflection (CookTorrance::shade + emissive) + simple alpha
///   transmission of the background: color = reflection + background * (1 - alpha).
///   No refraction bend — that is the Ultra tier's job.
/// - TrueRefraction: STUB — returns refractionRequested = true with the request filled in
///   (surface point, Snell refracted ray, IOR, pixel). Color is black; nothing here
///   pretends to refract. Additive materials fall back to the flat shade instead.
inline TransparencyResult shadeTransparent(const MaterialDef& mat, const TransparencyTierConfig& cfg,
                                           const TransparentShadingContext& ctx,
                                           const std::array<float, 3>& surfacePoint) {
  TransparencyResult out;
  const PbrTransparencyTier tier = cfg.tier;

  auto pbrSurface = [&]() {
    std::array<float, 3> c = CookTorrance::shade(mat.albedo, mat.metalness, mat.roughness,
                                                 ctx.normal, ctx.viewDir, ctx.lightDir,
                                                 ctx.lightColor);
    for (int i = 0; i < 3; ++i) c[i] += mat.emissive[i] * mat.emissiveIntensity;
    return c;
  };
  auto flatSurface = [&]() {
    std::array<float, 3> c = mat.albedo;
    for (int i = 0; i < 3; ++i) c[i] += mat.emissive[i] * mat.emissiveIntensity;
    return c;
  };

  // Opaque materials never enter the transparency framework.
  if (mat.opaque()) {
    out.color = pbrSurface();
    return out;
  }
  // Additive glow is tier-independent.
  if (mat.transparency == TransparencyMethod::Additive) {
    out.color = flatSurface();
    return out;
  }

  switch (tier) {
    case PbrTransparencyTier::Simple: {
      // Classic alpha blend, no PBR. Renderer orders via sortTransparent().
      const std::array<float, 3> surf = flatSurface();
      for (int i = 0; i < 3; ++i)
        out.color[i] = surf[i] * mat.alpha + ctx.background[i] * (1.0f - mat.alpha);
      break;
    }
    case PbrTransparencyTier::ScreenDoor: {
      // Full PBR through the opaque pipeline, then the 4x4 Bayer discard. No sorting.
      out.color = pbrSurface();
      out.discarded = !screenDoorTest(ctx.pixelX, ctx.pixelY, alphaToByte(mat.alpha));
      break;
    }
    case PbrTransparencyTier::PbrReflections: {
      // PBR reflection + simple alpha transmission (no refraction bend).
      // Mirrors the GPU branch in linear: refl*alpha + background*(1-alpha).
      // (Fix 2026-10-11: was refl + background*(1-alpha) — energy gain.)
      const std::array<float, 3> refl = pbrSurface();
      for (int i = 0; i < 3; ++i)
        out.color[i] = refl[i] * mat.alpha + ctx.background[i] * (1.0f - mat.alpha);
      break;
    }
    case PbrTransparencyTier::TrueRefraction: {
      // STUB: hand the Vulkan path everything it needs; do not invent a color.
      out.refractionRequested = true;
      out.refraction.surfacePoint = surfacePoint;
      out.refraction.refractDir =
          snellRefract(ctx.viewDir, ctx.normal, mat.ior, &out.refraction.totalInternalReflection);
      out.refraction.ior = mat.ior;
      out.refraction.pixelX = ctx.pixelX;
      out.refraction.pixelY = ctx.pixelY;
      break;
    }
  }
  return out;
}

}  // namespace litholattice
