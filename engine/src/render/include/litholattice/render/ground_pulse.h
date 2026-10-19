// Wafer ground pulse: the refined silicon surface breathes trace/zap energy.
// The wafer's heartbeat. Render/emissive-level only - the sim tick budget is untouched.
//
// WHAT: a pure function of (world time, chunk position, biome weights) ->
//   additive emissive RGB for the silicon surface. Per-CHUNK phase offsets
//   (integer hash of the chunk coords -> [0,1)), never per-block timers: the
//   pulse math is a hash, one sin and one pow per shaded fragment (~20 flops,
//   ~free next to the texture fetch and lighting the shader already does).
// WHY: the pulse is the Wafer's visible heartbeat - periodic energy through the
//   surface blocks, pattern varying per biome (JD-locked 2026-10-11), blended
//   across biome boundaries (JD 2026-10-11: the biome system outputs a weight
//   vector per column, Minecraft-style, no hard borders).
// HOW: the biome weights are worldgen-static per chunk column, so the blend
//   happens ONCE AT MESH TIME: the game hands the mesher a (chunkX, chunkZ) ->
//   blended-params function (MeshContext::pulseParams); each ChunkMesh carries
//   its column's params as two vec4s (p0 = freq/sharp/amp, p1 = tint.rgb) and
//   the backends push them per draw call (GL33: two uniforms; Vulkan: push
//   constants). The chunk shader (chunk_shading.glsl, shared by the GL33 and
//   Vulkan classic programs) adds groundPulseColor() - the GLSL mirror of
//   groundPulseEmissive() below - for kShadeGroundPulse quads, scaled by the
//   uGroundPulseOn uniform (cvar `render.ground_pulse`, live toggle, no remesh).
//   No per-frame uploads, no biome texture, no region bookkeeping.
//   The two implementations intentionally mirror each other (same tunings, same
//   waveform shape); only the chunk-phase hash differs (integer hash here, the
//   classic sin-hash in GLSL) - cross-implementation phase equality is not
//   required, same-inputs-same-outputs within each implementation is.
// DETERMINISM: no RNG, no state. Time comes from the sim's world-tick counter
//   (groundPulseTimeSec); pausing the sim freezes worldTick, so the pulse
//   freezes too. Never wall-clock.
// COST: called once per shaded fragment of a tagged quad only; untagged quads
//   pay one uniform-branch (`uGroundPulseOn > 0.5`, coherent across the frame).
#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace litholattice {

/// Wafer surface biomes for the ground pulse. The ids are the contract between
/// the mesher's vertex tag (FaceLook::value), the C++ mirror here and the GLSL
/// mirror in chunk_shading.glsl. The game's procgen owns the chunk -> biome map
/// (see the game's ground_pulse.h seam); until it is wired every chunk reads as
/// SiliconFlats.
enum class GroundPulseBiome : uint8_t {
  None = 0,            ///< Dead Zones: unpowered, NO pulse (the point). Never stamped.
  SiliconFlats = 1,    ///< today's whole Wafer; the default when no biome map is wired
  TraceForest = 2,     ///< blue-dominant slow pulse
  DicingPlains = 3,    ///< subtle
  CapacitorGroves = 4, ///< warm
  ResonatorSpires = 5, ///< bright, sharp
};
constexpr uint8_t kGroundPulseBiomeCount = 6;  ///< ids 0..5

/// Per-biome waveform tunings (JD-locked 2026-10-11). Mirrored in
/// assets/shaders/chunk_shading.glsl groundPulseColor() - keep in sync.
struct GroundPulseParams {
  float freqHz = 0.4f;     ///< pulse frequency
  float sharpness = 1.5f;  ///< pow() applied to the 0..1 wave: >1 sharpens the peak
  float amplitude = 0.14f; ///< peak additive glow (0..1-ish linear)
  float tintR = 0.45f, tintG = 0.62f, tintB = 1.0f;  ///< pulse colour tint
};

/// The JD-locked tuning table. None (Dead Zones) is the zero vector: it
/// contributes nothing to a blend, so the pulse fades out smoothly approaching
/// a Dead Zone instead of cutting off.
inline GroundPulseParams groundPulseParams(GroundPulseBiome biome) {
  switch (biome) {
    case GroundPulseBiome::None:
      return {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};  // unpowered: contributes zero
    case GroundPulseBiome::TraceForest:
      return {0.22f, 1.0f, 0.38f, 0.30f, 0.50f, 1.00f};  // blue-dominant, slow
    case GroundPulseBiome::DicingPlains:
      return {0.50f, 1.0f, 0.07f, 0.70f, 0.75f, 0.85f};  // subtle
    case GroundPulseBiome::CapacitorGroves:
      return {0.80f, 2.0f, 0.24f, 1.00f, 0.55f, 0.25f};  // warm
    case GroundPulseBiome::ResonatorSpires:
      return {2.00f, 6.0f, 0.55f, 0.75f, 0.90f, 1.00f};  // bright, sharp
    case GroundPulseBiome::SiliconFlats:
    default:
      return {0.40f, 1.5f, 0.14f, 0.45f, 0.62f, 1.00f};  // default surface shimmer
  }
}

/// Weight-blended pulse params for one chunk column (JD 2026-10-11: biome
/// blending). `weights[i]` is the biome weight for GroundPulseBiome i (the
/// biome system's per-column weight vector). Every field blends by weight -
/// e.g. 60% Trace Forest + 40% Dicing Plains = the 60/40 mix of their params.
/// Dead Zones (id 0) contribute zero, so amplitude (and tint) fade out smoothly
/// across the boundary instead of cutting off. The blend renormalizes by the
/// weight sum (same semantics as the procgen sibling's blendLerp); all-zero
/// weights fall back to the Silicon Flats default. Pure function of the
/// weights: deterministic.
inline GroundPulseParams blendPulseParams(const float weights[kGroundPulseBiomeCount]) {
  float wsum = 0.f;
  for (uint8_t i = 0; i < kGroundPulseBiomeCount; ++i) wsum += weights[i];
  if (!(wsum > 0.f)) return groundPulseParams(GroundPulseBiome::SiliconFlats);
  GroundPulseParams out{0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
  const float inv = 1.f / wsum;
  for (uint8_t i = 0; i < kGroundPulseBiomeCount; ++i) {
    const GroundPulseParams p = groundPulseParams(GroundPulseBiome(i));
    const float w = weights[i] * inv;
    out.freqHz += w * p.freqHz;
    out.sharpness += w * p.sharpness;
    out.amplitude += w * p.amplitude;
    out.tintR += w * p.tintR;
    out.tintG += w * p.tintG;
    out.tintB += w * p.tintB;
  }
  return out;
}

/// Per-chunk phase in [0,1): integer hash of the chunk coords. Same chunk ->
/// same phase, every run, no state.
inline float groundPulsePhase01(int chunkX, int chunkZ) {
  uint32_t h = uint32_t(chunkX) * 374761393u + uint32_t(chunkZ) * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return float(h & 0xFFFFFFu) / float(0x1000000u);
}

/// The pulse: additive emissive RGB for the surface of chunk (chunkX, chunkZ) at
/// sim-time `tSec`, with already-blended params (see blendPulseParams). Zero
/// amplitude (e.g. a fully Dead-Zone blend) is exactly zero at all times.
inline std::array<float, 3> groundPulseEmissive(float tSec, int chunkX, int chunkZ,
                                               const GroundPulseParams& p) {
  if (p.amplitude <= 0.f) return {0.f, 0.f, 0.f};
  const float phase = groundPulsePhase01(chunkX, chunkZ);
  float w = 0.5f + 0.5f * std::sin(6.28318530718f * (p.freqHz * tSec + phase));
  w = std::pow(w, p.sharpness);
  const float a = p.amplitude * w;
  return {p.tintR * a, p.tintG * a, p.tintB * a};
}

/// Convenience: unblended single-biome pulse. None (Dead Zones) is zero.
inline std::array<float, 3> groundPulseEmissive(float tSec, int chunkX, int chunkZ,
                                               GroundPulseBiome biome) {
  if (biome == GroundPulseBiome::None) return {0.f, 0.f, 0.f};
  return groundPulseEmissive(tSec, chunkX, chunkZ, groundPulseParams(biome));
}

/// Sim ticks -> pulse seconds. Pausing the sim freezes worldTick, so the pulse
/// freezes too (this is the whole determinism story). Computed in double, cast
/// to float for the shader uniform; tps is always a pow2 mode so the division
/// is exact-ish and cheap.
inline float groundPulseTimeSec(uint64_t worldTick, uint32_t ticksPerSec) {
  if (ticksPerSec == 0) return 0.f;
  return float(double(worldTick) / double(ticksPerSec));
}

}  // namespace litholattice
