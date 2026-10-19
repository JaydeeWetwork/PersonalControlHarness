// Model dithering: near-camera screen-door fade (JD 2026-10-13).
//
// Geometry whose view depth falls inside the fade band [near, far] is
// discarded per-pixel against a deterministic threshold, so close-up
// cameras (free cams, MCP/agent cameras, Daemon-vision feeds) are not
// swallowed by a wall of voxels, while geometry beyond `far` renders
// untouched and geometry closer than `near` is fully faded out.
// Purely presentational: no sim state, no RNG state, no remeshing — the
// test runs in the shading stage from the fragment's view depth.
//
// Threshold pattern: interleaved gradient noise (IGN, Jimenez 2014) —
// a hash of the integer pixel position ONLY. No frame counter, no time,
// no per-object seed: for a static camera the pattern is bit-stable
// across frames, and the CPU and GLSL forms below use the identical
// formula so backends agree pixel-for-pixel on the discard decision.
//
// Survey note (2026-10-13, see Model-Dithering-2026-10-13.md): Bayer
// 4x4/8x8 (the engine's existing glass/ScreenDoor pattern, kBayer4x4)
// tiles visibly at the large on-screen scale of near geometry; a
// void-and-cluster table has the best spatial distribution but needs a
// 64x64 rank table; IGN is table-free, aperiodic, and is the pattern
// shipped games use for exactly this dithered-fade job.
#pragma once

#include <cstdint>

namespace litholattice {

/// Interleaved gradient noise threshold for pixel (x, y), in [0, 1).
/// Integer inputs only; the arithmetic is plain IEEE float (exact for
/// these ops), identical in the GLSL twin (modelDitherIgn in the chunk
/// shaders). Deterministic across runs and platforms for the same pixel.
inline float modelDitherIgn(int x, int y) {
  const float fx = float(x), fy = float(y);
  const float t = 0.06711056f * fx + 0.00583715f * fy;
  const float frac = t - float(int64_t(t));  // fract() for t >= 0 (pixel coords are)
  float v = 52.9829189f * frac;
  v = v - float(int64_t(v));
  return v;
}

/// Fade factor for a fragment at view depth `viewDepth` (blocks):
/// 1 at >= far (fully kept), 0 at <= near (fully faded), linear between.
/// near/far are the render.model_dither_near / _far cvars.
inline float modelDitherFade(float viewDepth, float near, float far) {
  if (far <= near) return viewDepth <= near ? 0.0f : 1.0f;
  const float f = (viewDepth - near) / (far - near);
  return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

/// The discard test: keep the fragment iff its IGN threshold falls
/// below the depth fade factor.
inline bool modelDitherKeep(int x, int y, float viewDepth, float near, float far) {
  const float f = modelDitherFade(viewDepth, near, far);
  if (f >= 1.0f) return true;
  if (f <= 0.0f) return false;
  return modelDitherIgn(x, y) < f;
}

}  // namespace litholattice
