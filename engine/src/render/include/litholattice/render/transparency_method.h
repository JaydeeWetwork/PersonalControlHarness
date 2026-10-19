// Transparency primitives that do not need renderer.h: the TransparencyMethod
// enum plus the screen-door dither utilities. Split out of transparency.h into
// this leaf header (no engine includes) so that headers needing only these —
// e.g. pbr_materials.h, pbr_transparency_tiers.h — do not pull in the full
// transparency system (which needs ShadeMode/PackedVertex from renderer.h)
// and create an include cycle.
#pragma once

#include <cstdint>

namespace litholattice {

/// Transparency method for a material.
enum class TransparencyMethod {
  ScreenDoor,   // kShadeGlass: 4x4 dither, no sorting
  AlphaBlend,   // kShadeAlphaBlend: sorted back-to-front
  WeightedOIT,  // kShadeWeightedOIT: no sorting, weighted blend
  Additive,     // kShadeAdditive: src_alpha, 1; no sorting
};

/// 4x4 Bayer matrix for screen-door dithering.
constexpr uint8_t kBayer4x4[16] = {
   0,  8,  2, 10,
  12,  4, 14,  6,
   3, 11,  1,  9,
  15,  7, 13,  5,
};

/// Screen-door test: returns true if pixel should be drawn.
/// x, y = screen coordinates, alpha = 0-255.
inline bool screenDoorTest(int x, int y, uint8_t alpha) {
  const uint8_t threshold = kBayer4x4[(y & 3) * 4 + (x & 3)] * 17;  // 0-255
  return alpha > threshold;
}

}  // namespace litholattice
