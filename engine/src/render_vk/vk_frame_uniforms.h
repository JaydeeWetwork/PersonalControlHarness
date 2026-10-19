// C++ mirror of the std140 `Frame` uniform block in assets/shaders/vk/frame.glsl, and the push
// constant blocks of the Vulkan shaders. Keep in sync with the GLSL (offsets are asserted).
#pragma once
#include <cstddef>
#include <cstdint>

namespace litholattice::vk {

/// std140 array element: scalars in arrays take a 16-byte slot (explicit padding, no alignas, so
/// the layout is plain and every offset below is asserted).
struct Std140Int { int32_t value = 0; int32_t pad[3]{}; };
struct Std140Float { float value = 0; float pad[3]{}; };

struct FrameUniforms {
  float viewProj[16];
  float cam[3]; float fogEnd;
  float sky[3]; float time;
  int32_t tile, pipMask, cellMask, debug;  // debug: 1 = debug overlay (wire strength numbers)
  Std140Int stripPalette[2], stripRing[2];
  Std140Int tierPalette[3], tierMask[3], tierRim[3];
  Std140Int animStart[8], animLength[8];
  Std140Float animFrameMs[8];
  Std140Int animSequence[64];
  Std140Int stripDot[2];
  int32_t digits, pad1[3];
  float pulseTime = 0, pulseOn = 1, pulsePad[2]{};  ///< wafer ground pulse (JD 2026-10-11)
  float traceTime = 0, traceOn = 1, tracePad[2]{};  ///< wafer ground traces (JD 2026-10-11)
};
static_assert(offsetof(FrameUniforms, cam) == 64 && offsetof(FrameUniforms, fogEnd) == 76);
static_assert(offsetof(FrameUniforms, sky) == 80 && offsetof(FrameUniforms, time) == 92);
static_assert(offsetof(FrameUniforms, tile) == 96 && offsetof(FrameUniforms, cellMask) == 104);
static_assert(offsetof(FrameUniforms, stripPalette) == 112 && offsetof(FrameUniforms, tierPalette) == 176);
static_assert(offsetof(FrameUniforms, animStart) == 320 && offsetof(FrameUniforms, animFrameMs) == 576);
static_assert(offsetof(FrameUniforms, animSequence) == 704 && sizeof(FrameUniforms) == 1808);
// Pulse/trace tails are vec4-shaped (P1-RENDER H1): frame.glsl declares them
// as vec4 uPulse/uTrace at exactly these offsets. (The old float + float[2]
// GLSL spelling strides arrays by 16 under std140 and read the wrong words.)
static_assert(offsetof(FrameUniforms, pulseTime) == 1776 && offsetof(FrameUniforms, traceTime) == 1792);

/// Push constants. One 80-byte range (vertex + fragment) shared by every pipeline.
/// Per-chunk-mesh push (vk/chunk.vert + vk/chunk.frag declare the same block):
/// sub-chunk offset plus the wafer ground-pulse blended params (JD 2026-10-11).
struct ChunkPush {
  float offset[3]; float pad;
  float pulseP0[4];  ///< (freqHz, sharpness, amplitude, 0)
  float pulseP1[4];  ///< (tintR, tintG, tintB, 0)
  float traceP0[4];  ///< wafer ground traces (JD 2026-10-11): (density, speed, amplitude, dirMix)
  float traceP1[4];  ///< (tintR, tintG, tintB, 0)
};
struct LinePush { float matrix[16]; float color[4]; };           ///< vk/line.vert + line.frag
struct SpritePush { float alphaTest; float pad[3]; };              ///< vk/sprite.frag (0.5 cutout, 0 blended)
struct HudPush { float screen[2]; };                             ///< vk/hud.vert
constexpr uint32_t kPushConstantBytes = sizeof(LinePush);
static_assert(sizeof(ChunkPush) <= kPushConstantBytes && sizeof(HudPush) <= kPushConstantBytes);
static_assert(sizeof(SpritePush) <= kPushConstantBytes);

}  // namespace litholattice::vk
