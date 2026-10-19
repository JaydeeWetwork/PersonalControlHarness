#version 330 core
// Chunk PBR fragment program (PBR phase 2 - ACTIVE: built as "chunk_pbr" by the gl33
// backend. See pbr_shading.glsl for the full seam list and the exact BRDF formulas
// this mirrors.)
//
// Per-fragment: material-table lookup by compact ID, flood-fill irradiance for
// diffuse, K analytic lights for specular, emissive straight add.
//
// MATERIAL TABLE TRANSPORT: the GL 3.3 backend has no SSBOs (4.3+; the vendored
// loader is 3.3-only), so the documented 48-byte std430 rows travel in a buffer
// texture: row r, float k = texelFetch(uPbrMaterials, r * 12 + k). The indices below
// mirror render/pbr_gl_upload.h (kPbr*); a Vulkan backend would bind the same rows
// as a real std430 SSBO. Out-of-range IDs clamp to row 0 (the built-in default).
//
// TRANSPARENCY TIERS (uniform int uPbrTier; values match PbrTransparencyTier order):
//   0 Simple        - straight alpha output; the backend enables blending for the
//                     PBR draw. Opaque materials bypass (alpha == 1).
//   1 ScreenDoor    - full PBR + 4x4 Bayer discard for alpha < 1; no blending, no
//                     sorting (the binary mesher only emits opaque cubes today, so
//                     this is the honest default tier).
//   2 PbrReflections- PBR reflection + simple alpha transmission (backend blends).
//   3 TrueRefraction- real refraction: the backend draws the PBR meshes twice. Pass 0
//                     (uRefractionPass == 0) draws opaque fragments only, then copies
//                     the framebuffer to `uSceneColor`; pass 1 draws refractive
//                     fragments only, sampling the copy along the Snell ray with TIR
//                     handling (pbrRefractSample in pbr_shading.glsl). Opaque fragments
//                     shade exactly like the other tiers. The uniform name `uSceneColor`
//                     and the tier value 3 are the contract - do not renumber the tiers.
in vec3 vWorld; in vec3 vNormal; in vec3 vViewDir;
flat in uint vMaterialId;
flat in float vBlockLight; flat in float vSkyLight;
uniform samplerBuffer uPbrMaterials;  // 12 R32F texels per material (48-byte rows)
// K analytic lights from the LightCollector (game fills FrameParams::pbrLights per
// frame; the backend uploads them here). lightCount <= PBR_MAX_LIGHTS (8).
uniform vec4 uPbrLightPos[8];    // xyz = world pos, w = intensity (PBR_MAX_LIGHTS)
uniform vec3 uPbrLightColor[8];  // linear emission colour
uniform int uPbrLightCount;
uniform int uPbrTier;  // 0..3, see above
// Near-camera model dithering (JD 2026-10-13): same IGN screen-door fade
// as the classic program and the CPU path (render/model_dither.h).
uniform float uModelDither;     // 1 = on, 0 = off
uniform vec2 uModelDitherBand;  // (near, far) view depth in blocks
// Flood-fill ambient mode (JD-locked 2026-10-11, cvar `render.ambient_mode`):
// 0 = Fraction (cheap default), 1 = NormalizedSun (direction-aware hemisphere).
uniform int uAmbientMode;
uniform vec3 uSunDir;  // sun direction for the hemisphere term (need not be unit)
// PBR material table row count: pbrMaterialFromTbo clamps IDs into [0, rows),
// falling back to row 0 (fix 2026-10-11: min(id,0xffff) still read past the table
// for stale/foreign IDs from older registry generations).
uniform int uPbrMaterialRows;
// TrueRefraction (Ultra) scene-color pass: the backend binds the opaque composite
// (sRGB-encoded, mipmapped) as `uSceneColor` before the refractive pass. The uniform
// name `uSceneColor` is the phase-1 contract - kept verbatim.
uniform sampler2D uSceneColor;  // opaque composite for the refraction sample
uniform vec2 uSceneSize;        // copy resolution in pixels (for gl_FragCoord -> UV)
uniform mat4 uSceneVP;          // view-projection used when the copy was taken
uniform int uRefractionPass;    // 0 = scene build (opaque fragments only),
                                // 1 = refractive resolve (refractive fragments only)
out vec4 frag;
#include "pbr_shading.glsl"

// 4x4 Bayer matrix (same values and order as kBayer4x4 in transparency_method.h).
// Returns the discard threshold on the 0..255 scale the CPU screenDoorTest uses
// (matrix value * 17), so the GPU discard matches the CPU path exactly.
float pbrBayerThreshold(vec2 p) {
  const float m[16] = float[16](
       0.0,  8.0,  2.0, 10.0,
      12.0,  4.0, 14.0,  6.0,
       3.0, 11.0,  1.0,  9.0,
      15.0,  7.0, 13.0,  5.0);
  ivec2 ij = ivec2(int(mod(p.x, 4.0)), int(mod(p.y, 4.0)));
  return m[ij.y * 4 + ij.x] * 17.0;
}

PbrMaterial pbrMaterialFromTbo(uint id) {
  // Row 0 is the built-in default; out-of-range IDs (stale/foreign, e.g. from an
  // older registry generation) fall back to row 0 instead of reading OOB.
  // uPbrMaterialRows is the live table size (backend uploads it with the table).
  int rows = max(uPbrMaterialRows, 1);
  int r = id < uint(rows) ? int(id) : 0;
  int base = r * 12;
  PbrMaterial mat;
  mat.albedo = vec3(texelFetch(uPbrMaterials, base + 0).x,
                    texelFetch(uPbrMaterials, base + 1).x,
                    texelFetch(uPbrMaterials, base + 2).x);
  mat.roughness = texelFetch(uPbrMaterials, base + 3).x;
  mat.emissive = vec3(texelFetch(uPbrMaterials, base + 4).x,
                      texelFetch(uPbrMaterials, base + 5).x,
                      texelFetch(uPbrMaterials, base + 6).x);
  mat.emissiveIntensity = texelFetch(uPbrMaterials, base + 7).x;
  mat.metalness = texelFetch(uPbrMaterials, base + 8).x;
  mat.ior = texelFetch(uPbrMaterials, base + 9).x;
  mat.alpha = texelFetch(uPbrMaterials, base + 10).x;
  mat._pad = 0.0;
  return mat;
}

void main() {
  if (uModelDither > 0.5) {
    // vViewDir = uCam - vWorld (vertex stage), so its length is the
    // fragment's view depth in blocks.
    float ditherDist = length(vViewDir);
    float ditherFade = clamp((ditherDist - uModelDitherBand.x) /
                                 max(uModelDitherBand.y - uModelDitherBand.x, 0.0001),
                             0.0, 1.0);
    if (ditherFade < 1.0) {
      float ign = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x +
                                           0.00583715 * gl_FragCoord.y));
      if (ign >= ditherFade) discard;
    }
  }
  vec3 n = normalize(vNormal);
  vec3 v = normalize(vViewDir);
  PbrMaterial mat = pbrMaterialFromTbo(vMaterialId);
  vec3 linear = pbrShadeFragment(mat, n, v, vWorld,
                                 uPbrLightPos, uPbrLightColor, uPbrLightCount,
                                 vBlockLight, vSkyLight, uAmbientMode, uSunDir);
  float alpha = mat.alpha;
  if (uPbrTier == 1 && alpha < 1.0) {
    // ScreenDoor: ordered-dither discard, no blending, no sorting. The comparison
    // mirrors screenDoorTest exactly (alpha byte vs matrix*17).
    if (floor(alpha * 255.0) <= pbrBayerThreshold(gl_FragCoord.xy)) discard;
    alpha = 1.0;
  } else if (uPbrTier == 3) {
    // TrueRefraction (Ultra): two-pass scene-color refraction. Pass 0 keeps opaque
    // fragments only (they become the scene-color copy); pass 1 keeps refractive
    // fragments only and resolves them via the Snell-ray scene sample. Opaque
    // fragments shade exactly like the other tiers (linear already holds the PBR
    // shade); the refractive resolve outputs an opaque composite (no blending).
    if (uRefractionPass == 0) {
      if (alpha < 1.0) discard;
    } else {
      if (alpha >= 1.0) discard;
      linear = pbrRefractSample(mat, n, v, vWorld, uSceneVP,
                                uPbrLightPos, uPbrLightColor, uPbrLightCount,
                                uSceneColor, uSceneSize);
      alpha = 1.0;
    }
  }
  // Tier 0 (Simple) and 2 (PbrReflections): straight alpha; the backend enables
  // blending for the PBR draw on those tiers.
  frag = vec4(pbrLinearToSrgb(linear), alpha);
}
