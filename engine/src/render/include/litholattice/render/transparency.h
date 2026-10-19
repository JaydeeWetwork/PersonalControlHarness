// Transparency system: four methods, engine-level.
// 1. Screen-door (dithered) — fast, no sorting
// 2. Alpha blending — correct, needs sorting
// 3. Weighted Blended OIT — no sorting, approximate
// 4. Additive — for particles/glow, no sorting
#pragma once
#include "litholattice/render/renderer.h"
#include "litholattice/render/transparency_method.h"
#include <vector>
#include <algorithm>

namespace litholattice {

/// Maps a ShadeMode to its transparency method.
inline TransparencyMethod transparencyFor(ShadeMode mode) {
  switch (mode) {
    case kShadeGlass: return TransparencyMethod::ScreenDoor;
    case kShadeAlphaBlend: return TransparencyMethod::AlphaBlend;
    case kShadeWeightedOIT: return TransparencyMethod::WeightedOIT;
    case kShadeAdditive: return TransparencyMethod::Additive;
    default: return TransparencyMethod::ScreenDoor;  // opaque
  }
}

/// A transparent quad for sorting.
struct TransparentQuad {
  PackedVertex verts[4];
  float depth;  // distance from camera (for sorting)
  ShadeMode mode;
};

/// Sort transparent quads back-to-front (for alpha blending).
inline void sortTransparent(std::vector<TransparentQuad>& quads) {
  std::sort(quads.begin(), quads.end(),
            [](const TransparentQuad& a, const TransparentQuad& b) {
              return a.depth > b.depth;  // far to near
            });
}

/// Weighted OIT: compute blend weight from depth and alpha.
/// Weight function: w = alpha * exp(-depth * k)
/// From McGuire & Bavoil's Weighted Blended OIT paper.
inline float oitWeight(float alpha, float depth) {
  // Clamp to avoid extreme values
  const float k = 0.1f;  // tunable falloff
  float w = alpha;
  // Depth weight: closer = higher weight
  w *= 1.0f / (1.0f + depth * k);
  return w > 0.001f ? w : 0.001f;
}

}  // namespace litholattice
