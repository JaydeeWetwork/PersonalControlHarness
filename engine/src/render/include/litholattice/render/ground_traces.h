// Wafer ground traces: animated energy packets flowing along trace lines on the
// silicon surface. The wafer thinking out loud. Render/emissive-level only -
// the sim tick budget is untouched.
//
// WHAT: a pure function of (world time, world position, blended trace params)
//   -> additive emissive RGB for the silicon surface. Manhattan-routed trace
//   LINES (orthogonal sets, like real circuits) with bright PACKETS travelling
//   along them - dashes of energy, some lines flowing opposite directions.
//   No per-frame state, no RNG: the pattern math is hashes, fracts and a few
//   smoothsteps (~60 flops per shaded fragment, ~free next to the texture
//   fetch and lighting the shader already does; untagged quads pay one
//   uniform-branch, coherent across the frame).
// WHY: JD 2026-10-11 - extend the ground pulse: render actual trace lines on
//   the ground with energy animating along them, per-biome patterns. It should
//   look like the wafer is thinking.
// HOW: composes with the ground pulse (render/ground_pulse.h), it does not
//   replace it. Same biome ids (GroundPulseBiome), same per-column weight
//   vector, same mesh-time blend: the game hands the mesher a (chunkX, chunkZ)
//   -> blended-params function (MeshContext::traceParams); each ChunkMesh
//   carries its column's params as two vec4s (t0 = density/speed/amplitude/
//   dirMix, t1 = tint.rgb) and the backends push them per draw call (GL33: two
//   uniforms; Vulkan: push constants). The chunk shader (chunk_shading.glsl,
//   shared by the GL33 and Vulkan classic programs) adds groundTraceColor() -
//   the GLSL mirror of groundTraceEmissive() below - for kShadeGroundPulse
//   quads, scaled by the uGroundTraceOn uniform (cvar `render.ground_traces`,
//   live toggle, no remesh, independent of `render.ground_pulse`). Both effects
//   add into the same fragment: pulse + traces compose additively.
//   LANDED end-to-end 2026-10-13 (P1-RENDER H1; before that this feature was
//   mesher-baked but rendered nothing on every backend): GLSL mirror in
//   chunk_shading.glsl, Vulkan Frame-block + push wiring, the cvar, the
//   game's MeshContext::traceParams + FrameParams wiring, and the software
//   backend consuming groundTraceEmissive() directly in its per-pixel path
//   (the CPU mirror's first production consumer).
//   The two implementations intentionally mirror each other (same tunings, same
//   line/packet shapes); only the line hash differs (integer hash here, the
//   classic sin-hash in GLSL) - cross-implementation line equality is not
//   required, same-inputs-same-outputs within each implementation is.
// DETERMINISM: no RNG, no state. Time comes from the sim's world-tick counter
//   (groundPulseTimeSec - the same clock the pulse uses); pausing the sim
//   freezes worldTick, so the packets freeze too. Never wall-clock.
// COST: called once per shaded fragment of a tagged quad only; untagged quads
//   pay one uniform-branch (`uGroundTraceOn > 0.5`, coherent across the frame).
//   ~60 flops/fragment on tagged quads (2 sin-hashes, 6 smoothsteps, ~20
//   mul/add) - documented, not measured; the self-test asserts the function
//   stays allocation-free and fast (see testGroundTraces cost sanity).
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include "litholattice/render/ground_pulse.h"  // GroundPulseBiome ids + the shared sim clock

namespace litholattice {

/// Per-biome trace-line tunings. Proposed by the traces workstream 2026-10-11
/// (JD locks tunings; the pulse's table is the model). Mirrored in
/// assets/shaders/chunk_shading.glsl groundTraceColor() - keep in sync.
struct GroundTraceParams {
  float density = 0.25f;   ///< trace lines per block (0.5 = a line every 2 blocks)
  float speed = 1.0f;      ///< packets per second travelling along a line
  float amplitude = 0.20f; ///< peak additive glow (0..1-ish linear)
  float dirMix = 0.5f;     ///< 0 = lines run along X only, 1 = along Z only, 0.5 = grid
  float tintR = 0.45f, tintG = 0.62f, tintB = 1.0f;  ///< trace colour tint
};

/// The tuning table. None (Dead Zones) is the zero vector: it contributes
/// nothing to a blend, so the traces fade out smoothly approaching a Dead Zone
/// instead of cutting off - the lines go dark.
inline constexpr GroundTraceParams groundTraceParams(GroundPulseBiome biome) {
  switch (biome) {
    case GroundPulseBiome::None:
      return {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};  // unpowered: lines go dark
    case GroundPulseBiome::TraceForest:
      return {0.50f, 1.6f, 0.35f, 0.35f, 0.30f, 0.50f, 1.00f};  // dense blue flow
    case GroundPulseBiome::DicingPlains:
      return {0.125f, 0.5f, 0.10f, 0.50f, 0.70f, 0.75f, 0.85f};  // sparse, grid-aligned
    case GroundPulseBiome::CapacitorGroves:
      return {0.25f, 0.35f, 0.22f, 0.65f, 1.00f, 0.55f, 0.25f};  // warm, slow
    case GroundPulseBiome::ResonatorSpires:
      return {0.50f, 3.0f, 0.50f, 0.50f, 0.75f, 0.90f, 1.00f};  // bright, quick
    case GroundPulseBiome::SiliconFlats:
    default:
      return {0.20f, 0.8f, 0.15f, 0.50f, 0.55f, 0.65f, 0.95f};  // default surface traces
  }
}

/// Weight-blended trace params for one chunk column - the same blend contract
/// as blendPulseParams: `weights[i]` is the biome weight for GroundPulseBiome
/// i, every field blends by weight, renormalized by the weight sum (same
/// semantics as the procgen sibling's blendLerp); all-zero weights fall back to
/// the Silicon Flats default. Dead Zones (id 0) contribute zero, so density,
/// speed, amplitude and tint fade out smoothly across the boundary instead of
/// cutting off. Pure function of the weights: deterministic.
inline GroundTraceParams blendTraceParams(const float weights[kGroundPulseBiomeCount]) {
  float wsum = 0.f;
  for (uint8_t i = 0; i < kGroundPulseBiomeCount; ++i) wsum += weights[i];
  if (!(wsum > 0.f)) return groundTraceParams(GroundPulseBiome::SiliconFlats);
  GroundTraceParams out{0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
  const float inv = 1.f / wsum;
  for (uint8_t i = 0; i < kGroundPulseBiomeCount; ++i) {
    const GroundTraceParams p = groundTraceParams(GroundPulseBiome(i));
    const float w = weights[i] * inv;
    out.density += w * p.density;
    out.speed += w * p.speed;
    out.amplitude += w * p.amplitude;
    out.dirMix += w * p.dirMix;
    out.tintR += w * p.tintR;
    out.tintG += w * p.tintG;
    out.tintB += w * p.tintB;
  }
  return out;
}

/// Per-line hash in [0,1): integer hash of the line index + seed. Same line ->
/// same value, every run, no state. Drives per-line brightness, packet phase
/// offset and flow direction.
inline float groundTraceHash01(int32_t line, uint32_t seed) {
  uint32_t h = uint32_t(line) * 374761393u + seed * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return float(h & 0xFFFFFFu) / float(0x1000000u);
}

inline float groundTraceSmoothstep(float a, float b, float x) {
  const float t = std::fmin(1.f, std::fmax(0.f, (x - a) / (b - a)));
  return t * t * (3.f - 2.f * t);
}

/// The traces: additive emissive RGB at world position (worldX, worldZ) at
/// sim-time `tSec`, with already-blended params (see blendTraceParams).
/// Two orthogonal line sets (Manhattan routing): the H-set runs along X at
/// z = n / density, the V-set runs along Z at x = n / density; dirMix weights
/// the sets. Each line carries a faint constant glow plus bright packet dashes
/// travelling along it (direction per line from the hash - some lines flow
/// opposite). Zero density or zero amplitude (e.g. a fully Dead-Zone blend) is
/// exactly zero at all times. Output is bounded by amplitude (tint <= 1) and
/// non-negative.
inline std::array<float, 3> groundTraceEmissive(float tSec, float worldX, float worldZ,
                                               const GroundTraceParams& p) {
  if (p.amplitude <= 0.f || p.density <= 0.f) return {0.f, 0.f, 0.f};
  const float pk = p.density * 0.25f;  // packets per block along a line
  const float hMix = 1.f - p.dirMix, vMix = p.dirMix;
  // H-set: lines run along X at z = n / density.
  const float zn = worldZ * p.density;
  const int32_t lz = int32_t(std::floor(zn));
  const float fz = zn - float(lz);
  const float h1 = groundTraceHash01(lz, 1u);
  const float lineH = 0.35f + 0.65f * h1;
  const float dl = std::fmin(fz, 1.f - fz);
  const float wH = 1.f - groundTraceSmoothstep(0.02f, 0.08f, dl);
  const float sgnH = h1 < 0.5f ? 1.f : -1.f;
  const float pu = sgnH * worldX * pk - tSec * p.speed + h1;
  const float puF = pu - std::floor(pu);
  const float packH = groundTraceSmoothstep(0.f, 0.05f, puF) *
                      (1.f - groundTraceSmoothstep(0.05f, 0.28f, puF));
  // V-set: lines run along Z at x = n / density.
  const float xn = worldX * p.density;
  const int32_t lx = int32_t(std::floor(xn));
  const float fx = xn - float(lx);
  const float h2 = groundTraceHash01(lx, 2u);
  const float lineV = 0.35f + 0.65f * h2;
  const float dv = std::fmin(fx, 1.f - fx);
  const float wV = 1.f - groundTraceSmoothstep(0.02f, 0.08f, dv);
  const float sgnV = h2 < 0.5f ? 1.f : -1.f;
  const float pv = sgnV * worldZ * pk - tSec * p.speed + h2;
  const float pvF = pv - std::floor(pv);
  const float packV = groundTraceSmoothstep(0.f, 0.05f, pvF) *
                      (1.f - groundTraceSmoothstep(0.05f, 0.28f, pvF));
  const float lineGlow = hMix * wH * lineH + vMix * wV * lineV;
  const float packGlow = hMix * wH * lineH * packH + vMix * wV * lineV * packV;
  const float a = p.amplitude * (0.30f * lineGlow + 0.70f * packGlow);
  return {p.tintR * a, p.tintG * a, p.tintB * a};
}

/// Convenience: unblended single-biome traces. None (Dead Zones) is zero.
inline std::array<float, 3> groundTraceEmissive(float tSec, float worldX, float worldZ,
                                               GroundPulseBiome biome) {
  if (biome == GroundPulseBiome::None) return {0.f, 0.f, 0.f};
  return groundTraceEmissive(tSec, worldX, worldZ, groundTraceParams(biome));
}

}  // namespace litholattice
