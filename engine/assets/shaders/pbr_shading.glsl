// PBR shading shared by the phase-2 GL/Vulkan chunk programs (PBR phase 5).
//
// WHAT: the exact Cook-Torrance formulas from pbr_brdf.h, transliterated to GLSL, plus
//   the PbrMaterial struct (registry row layout), the many-lights-hybrid fragment
//   compose (flood-fill irradiance + K analytic lights), and linear->sRGB output.
// WHY: GPU shaders must mirror the CPU reference in pbr_brdf.h / pbr_software_shaders.h
//   term for term; this file is the single shared copy both backends include.
// HOW: no #version line and no binding declarations - the including program (GL 330 or
//   Vulkan 450) declares those. Pure functions only; light arrays arrive as parameters
//   so the file compiles under both GLSL dialects. The storage-buffer / uniform
//   declarations and the material lookup live in the including program - see
//   chunk_pbr.frag for the GL 330 reference wiring.
//
// Approximations (deliberate, documented - identical to pbr_brdf.h):
//   1. Fresnel is Schlick's approximation, not the exact Fresnel equations (< 2% error
//      vs exact for dielectrics with IOR ~1.5 below ~75 deg incidence).
//   2. Roughness remaps alpha = roughness^2 (Disney/Epic), clamped to >= 1e-3 so a
//      perfect mirror yields a tight lobe instead of a division by zero.
//   3. Geometry is the Smith joint-G as G1(V)*G1(L) with Schlick-GGX and
//      k = (roughness+1)^2/8 (Epic direct-lighting form), not the exact Smith-GGX
//      integral. Exact at NdotV = NdotL = 1.
//   4. The diffuse Fresnel factor is evaluated at the view angle; per-channel
//      (1 - F) * (1 - metalness) keeps combined outgoing energy within incoming.
//
// PHASE-2 SEAM (Worker 3, 2026-10-11 - GL backend wired; Vulkan still open):
//   1. assets/shaders/chunk_pbr.vert / chunk_pbr.frag: ACTIVE GL 330 programs, built as
//      "chunk_pbr" by the gl33 backend (kProgramNames in gl33_renderer.cpp). The GL
//      transport for the material table is a buffer texture (3.1+ core; SSBOs need
//      4.3 and the vendored loader is 3.3-only): the frag declares
//      `uniform samplerBuffer uPbrMaterials` and unpacks the 48-byte rows below via
//      texelFetch - the same rows a Vulkan backend would bind as a real std430 SSBO.
//      vk/chunk_pbr.vert/.frag are still to write (Vulkan SPIR-V is built in at
//      compile time; set/binding layout differs from GL).
//   2. src/render/gl33/gl33_renderer.cpp: setPbrMaterials() uploads the material table
//      (pbr_gl_upload.h packs the exact 48-byte rows; re-uploaded when the registry's
//      generation() moves); per frame, the analytic-light uniforms come from the
//      FrameParams::pbrLights array, which the game fills from a LightCollector fed
//      by ChunkEmissiveProvider (pbr_emissive_provider.h).
//   3. Mesher (src/render/mesher.cpp and friends): the BinaryQuad path stamps the
//      compact material ID per quad (BinaryQuad::material -> column palette ->
//      ChunkPalette::globalId, resolved in pbr_quad_mesh.h's expansion) and the
//      flood-fill light levels (coarse game-supplied values until the fill is wired).
//      JD's locked coexistence decision (2026-10-11) resolved the old vertex-format
//      question: PackedVertex is NEVER extended - the BinaryQuad path carries
//      material IDs natively through its own PbrQuadVertex layout, the PackedVertex
//      path keeps the classic chunk programs untouched.
//   4. chunk_shading.glsl: today's wire/lamp/palette modes stay on the classic chunk
//      program; chunk_pbr coexists per mesher option (classic program serves the
//      PackedVertex path, PBR programs serve the BinaryQuad path).
//
// Registry row layout (std430 - VERIFIED against glslang's SPIR-V Offset decorations,
// 2026-10-11; the earlier 64-byte claim was wrong): vec3 fields are 16-byte ALIGNED but
// 12 bytes wide, so the struct is 48 bytes: albedo[0..12), roughness[12..16),
// emissive[16..28) (16-aligned), emissiveIntensity[28..32), metalness[32..36),
// ior[36..40), alpha[40..44), pad[44..48). Array stride is 48.
// The CPU upload side must pack this exact layout - see render/pbr_gl_upload.h
// (packPbrMaterialRow), which is tested headless against these offsets.
struct PbrMaterial {
  vec3 albedo;             // linear-space base colour
  float roughness;         // 0 = mirror, 1 = fully diffuse
  vec3 emissive;           // linear-space emission colour
  float emissiveIntensity; // emission multiplier (0 = no emission)
  float metalness;         // 0 = dielectric, 1 = metal
  float ior;               // index of refraction (dielectrics)
  float alpha;             // 0..1; < 1 means transparent
  float _pad;              // 48-byte stride padding (std430 rounds the struct size up)
};

const float PBR_PI = 3.14159265358979323846;
const float PBR_DIELECTRIC_F0 = 0.04;
const float PBR_MIN_ALPHA = 1e-3;  // roughness->alpha clamp (pbr_brdf.h kMinAlpha)
const float PBR_DENOM_EPS = 1e-4;  // specular denominator guard (pbr_brdf.h kDenomEps)
const int PBR_MAX_LIGHTS = 8;      // mirrors LightCollector's default K
const float PBR_MAX_LIGHT_LEVEL = 64.0;  // flood-fill range (pbr_lights.h kMaxLight)

// Fresnel-Schlick: F0 + (1 - F0) * (1 - cosTheta)^5.
vec3 pbrFresnelSchlick(vec3 f0, float cosTheta) {
  float c = clamp(cosTheta, 0.0, 1.0);
  float t = 1.0 - c;
  float t5 = t * t * t * t * t;
  return f0 + (vec3(1.0) - f0) * t5;
}

// GGX normal distribution D(N,H) (Walter et al.). Exact NDF.
float pbrDistributionGgx(float nDotH, float roughness) {
  float alpha = max(roughness * roughness, PBR_MIN_ALPHA);
  float a2 = alpha * alpha;
  float c = clamp(nDotH, 0.0, 1.0);
  float denom = c * c * (a2 - 1.0) + 1.0;
  return a2 / (PBR_PI * denom * denom);
}

// Smith geometry G(V,L) as G1(V)*G1(L), Schlick-GGX with k = (r+1)^2/8.
float pbrGeometrySmith(float nDotV, float nDotL, float roughness) {
  float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
  float g1v = nDotV / (nDotV * (1.0 - k) + k);
  float g1l = nDotL / (nDotL * (1.0 - k) + k);
  return g1v * g1l;
}

// Specular BRDF value F * D * G / (4 * NdotV * NdotL), no light multiply.
// Split out of pbrShade (below) so the Ultra refraction path can reuse the exact term;
// pbrShade's output is unchanged (term-for-term identical - see the energy sweep in
// testPbrBrdf, which still passes against this refactor).
vec3 pbrSpecularBrdf(vec3 f0, float roughness, vec3 n, vec3 v, vec3 l) {
  float nDotV = dot(n, v);
  float nDotL = dot(n, l);
  if (nDotV <= 0.0 || nDotL <= 0.0) return vec3(0.0);
  vec3 f = pbrFresnelSchlick(f0, nDotV);
  vec3 h = normalize(v + l);
  float d = pbrDistributionGgx(dot(n, h), roughness);
  float g = pbrGeometrySmith(nDotV, nDotL, roughness);
  float denom = max(4.0 * nDotV * nDotL, PBR_DENOM_EPS);
  return f * (d * g / denom);
}

// Combined outgoing radiance for one light:
//   Lo = (kD * albedo / pi + D * F * G / (4 * NdotV * NdotL)) * lightColor * NdotL
// with F0 = mix(0.04, albedo, metalness) and kD = (1 - F) * (1 - metalness).
// All inputs unit-length, world space, linear RGB. Returns black for NdotL <= 0 or
// NdotV <= 0. Term-for-term identical to CookTorrance::shade in pbr_brdf.h.
vec3 pbrShade(vec3 albedo, float metalness, float roughness,
              vec3 n, vec3 v, vec3 l, vec3 lightColor) {
  float nDotV = dot(n, v);
  float nDotL = dot(n, l);
  if (nDotV <= 0.0 || nDotL <= 0.0) return vec3(0.0);

  float m = clamp(metalness, 0.0, 1.0);
  vec3 f0 = vec3(PBR_DIELECTRIC_F0) * (1.0 - m) + albedo * m;
  vec3 f = pbrFresnelSchlick(f0, nDotV);
  vec3 spec = pbrSpecularBrdf(f0, roughness, n, v, l);

  vec3 kD = max(vec3(1.0) - f, vec3(0.0)) * (1.0 - m);
  vec3 diff = albedo * kD / PBR_PI;

  return (diff + spec) * lightColor * nDotL;
}

// Incoming radiance of one analytic light: rgb * intensity * (1 / (1 + d^2)).
// Inverse-square with a +1 softener; mirrors attenuatedRadiance in pbr_lights.h.
// These lights drive specular only, never diffuse (JD's locked design).
vec3 pbrAttenuatedRadiance(vec3 color, float intensity, float dist) {
  return color * (intensity / (1.0 + dist * dist));
}

// Normalized irradiance from the flood-fill light level (0-64) and sky (0-64).
// x = diffuse = max(block, sky) / 64, y = ambient = sky / 64.
// Mirrors irradianceFromLightLevel in pbr_lights.h.
vec2 pbrIrradiance(float blockLevel, float skyLevel) {
  return vec2(max(blockLevel, skyLevel) / PBR_MAX_LIGHT_LEVEL, skyLevel / PBR_MAX_LIGHT_LEVEL);
}

// Mode-aware ambient (JD-locked 2026-10-11): ambientMode 0 = Fraction (the above),
// 1 = NormalizedSun — the sky term scaled by the hemisphere factor against the
// normalized sun direction. Pure function (no uniforms): the including program
// passes its uAmbientMode/uSunDir.
vec2 pbrIrradianceMode(float blockLevel, float skyLevel, vec3 n, int ambientMode, vec3 sunDir) {
  vec2 irr = pbrIrradiance(blockLevel, skyLevel);
  if (ambientMode == 1) {
    vec3 sun = sunDir / max(length(sunDir), 1e-6);
    irr.y *= 0.5 + 0.5 * clamp(dot(n, sun), -1.0, 1.0);
  }
  return irr;
}

// Linear (0..1) -> sRGB, piecewise transfer function. Mirrors pbrLinearToSrgb.
vec3 pbrLinearToSrgb(vec3 c) {
  vec3 x = clamp(c, vec3(0.0), vec3(1.0));
  vec3 lo = 12.92 * x;
  vec3 hi = 1.055 * pow(x, vec3(1.0 / 2.4)) - vec3(0.055);
  return mix(lo, hi, step(vec3(0.0031308), x));
}

// Full many-lights-hybrid fragment compose (mirrors pbrFragmentShader):
// flood-fill irradiance handles diffuse; the K nearest analytic lights drive specular
// through the exact Cook-Torrance core; the registry emissive term is added straight.
// lightPos[i]: xyz = world pos, w = intensity. lightColor[i]: linear emission colour.
// lightCount <= PBR_MAX_LIGHTS. Returns LINEAR rgb; the caller converts to sRGB.
vec3 pbrShadeFragment(PbrMaterial mat, vec3 n, vec3 v, vec3 fragPos,
                      vec4 lightPos[PBR_MAX_LIGHTS], vec3 lightColor[PBR_MAX_LIGHTS],
                      int lightCount, float blockLight, float skyLevel,
                      int ambientMode, vec3 sunDir) {
  float metal = clamp(mat.metalness, 0.0, 1.0);
  vec2 irr = pbrIrradianceMode(blockLight, skyLevel, n, ambientMode, sunDir);
  vec3 lo = mat.albedo * (irr.x + irr.y) * (1.0 - metal);
  for (int i = 0; i < PBR_MAX_LIGHTS; ++i) {
    if (i >= lightCount) break;
    vec3 toL = lightPos[i].xyz - fragPos;
    float d2 = dot(toL, toL);
    if (d2 <= 1e-12) continue;
    float dist = sqrt(d2);
    vec3 l = toL / dist;
    vec3 lc = pbrAttenuatedRadiance(lightColor[i], lightPos[i].w, dist);
    lo += pbrShade(mat.albedo, metal, mat.roughness, n, v, l, lc);
  }
  lo += mat.emissive * mat.emissiveIntensity;
  return lo;
}

// ---------------------------------------------------------------------------
// TRUE REFRACTION (Ultra tier) helpers - PBR phase 2, Worker 4.
//
// The GL33 backend (chunk_pbr.frag) calls pbrRefractSample for refractive fragments
// (tier 3, pass 1); the CPU mirror lives in pbr_transparency_tiers.h
// (snellRefractEta / refractionScreenOffset / resolveTrueRefraction) and is
// engine-tested with known answers. Energy discipline (phase 1): the final compose is
//   final = specular(F-weighted) + (1 - F) * transmitted + emissive
// so the reflected/transmitted weights sum to 1 by construction - no energy creation.
// The small diffuse term is folded out for transmissive dielectrics (it belongs to
// light scattered inside the medium; for thin glass it is negligible, and keeping it
// would double-count the (1 - F) weight against the transmitted sample).
// Approximations (documented, same class as the BRDF section above):
//   1. Single-interface model: the entry face only (air -> material, eta = 1/ior,
//      the phase-1 assumption). Back faces and inside views use the same math;
//      a nested transmissive surface behind another resolves to the nearest layer
//      (the scene-color copy holds the opaque composite only).
//   2. The transmitted sample is a screen-space projection of one probe point
//      (PBR_REFRACTION_PROBE_DIST along the ray), not a depth-aware march - no
//      thickness, no volumetric absorption (Beer-Lambert is future work).
//   3. Roughness blurs via the mip chain (textureLod), not a multi-tap gather.
// ---------------------------------------------------------------------------
const float PBR_REFRACTION_PROBE_DIST = 1.0;   // world units along the ray (voxel scale)
const float PBR_REFRACTION_MAX_OFFSET_PX = 96.0;  // screen-space offset cap (pixels)
const float PBR_REFRACTION_MAX_LOD = 4.0;      // roughness 1 -> ~1/16-res mip level

// sRGB (0..1) -> linear, the inverse of pbrLinearToSrgb. The scene-color copy holds
// sRGB-encoded framebuffer output, so the transmitted sample must come back to linear
// before the Fresnel split. CPU mirror: srgbToLinear in pbr_transparency_tiers.h.
vec3 pbrSrgbToLinear(vec3 c) {
  vec3 x = clamp(c, vec3(0.0), vec3(1.0));
  vec3 lo = x / 12.92;
  vec3 hi = pow((x + vec3(0.055)) / 1.055, vec3(2.4));
  return mix(lo, hi, step(vec3(0.04045), x));
}

// Snell refraction with explicit TIR handling. GLSL refract() returns vec3(0) when
// total internal reflection occurs, which would sample a garbage UV - so do it by
// hand: on TIR the ray mirror-reflects (real physics, and the same rule as the CPU
// snellRefractEta). v is fragment -> camera (unit), n the unit surface normal.
vec3 pbrSnellRefract(vec3 v, vec3 n, float eta, out bool tir) {
  float cosI = clamp(dot(v, n), -1.0, 1.0);
  float sin2T = eta * eta * (1.0 - cosI * cosI);
  tir = sin2T > 1.0;
  if (tir) return reflect(-v, n);
  float cosT = sqrt(max(1.0 - sin2T, 0.0));
  return eta * -v + (eta * cosI - cosT) * n;
}

// Screen-space UV offset for the refracted ray: project the surface point and a probe
// point PBR_REFRACTION_PROBE_DIST along the ray, take the UV difference, clamp the
// pixel length to PBR_REFRACTION_MAX_OFFSET_PX. Returns (0,0) when either projected
// point is at/behind the camera. CPU mirror: refractionScreenOffset.
vec2 pbrRefractionUvOffset(vec3 worldPos, vec3 refr, mat4 vp, vec2 sceneSize) {
  vec4 c0 = vp * vec4(worldPos, 1.0);
  if (c0.w <= 1e-6) return vec2(0.0);
  vec4 c1 = vp * vec4(worldPos + refr * PBR_REFRACTION_PROBE_DIST, 1.0);
  if (c1.w <= 1e-6) return vec2(0.0);
  vec2 uv0 = c0.xy / c0.w * 0.5 + 0.5;
  vec2 uv1 = c1.xy / c1.w * 0.5 + 0.5;
  vec2 offPx = (uv1 - uv0) * sceneSize;
  float len = length(offPx);
  if (len > PBR_REFRACTION_MAX_OFFSET_PX) offPx *= PBR_REFRACTION_MAX_OFFSET_PX / len;
  return offPx / sceneSize;
}

// Full Ultra-tier refraction resolve for one fragment. sceneColor is the opaque
// composite (sRGB-encoded, mipmapped); sceneVP projects the refracted ray to UV.
// Returns LINEAR rgb; the caller converts to sRGB.
vec3 pbrRefractSample(PbrMaterial mat, vec3 n, vec3 v, vec3 fragPos, mat4 sceneVP,
                      vec4 lightPos[PBR_MAX_LIGHTS], vec3 lightColor[PBR_MAX_LIGHTS],
                      int lightCount, sampler2D sceneColor, vec2 sceneSize) {
  float metal = clamp(mat.metalness, 0.0, 1.0);
  float nDotV = clamp(dot(n, v), 0.0, 1.0);
  vec3 f0 = vec3(PBR_DIELECTRIC_F0) * (1.0 - metal) + mat.albedo * metal;
  vec3 F = pbrFresnelSchlick(f0, nDotV);
  // Reflected half: the F-weighted specular term only (diffuse folded out, see above).
  vec3 spec = vec3(0.0);
  for (int i = 0; i < PBR_MAX_LIGHTS; ++i) {
    if (i >= lightCount) break;
    vec3 toL = lightPos[i].xyz - fragPos;
    float d2 = dot(toL, toL);
    if (d2 <= 1e-12) continue;
    float dist = sqrt(d2);
    vec3 l = toL / dist;
    vec3 lc = pbrAttenuatedRadiance(lightColor[i], lightPos[i].w, dist);
    spec += pbrSpecularBrdf(f0, mat.roughness, n, v, l) * lc * max(dot(n, l), 0.0);
  }
  // Transmitted half: scene-color sample along the Snell ray (TIR -> mirror ray),
  // roughness-blurred through the mip chain.
  bool tir;
  vec3 refr = pbrSnellRefract(v, n, 1.0 / max(mat.ior, 1e-3), tir);
  vec2 uv = clamp(gl_FragCoord.xy / sceneSize + pbrRefractionUvOffset(fragPos, refr, sceneVP, sceneSize),
                  vec2(0.0), vec2(1.0));
  float lod = clamp(mat.roughness, 0.0, 1.0) * PBR_REFRACTION_MAX_LOD;
  vec3 transmitted = pbrSrgbToLinear(textureLod(sceneColor, uv, lod).rgb);
  return spec + (vec3(1.0) - F) * transmitted + mat.emissive * mat.emissiveIntensity;
}
