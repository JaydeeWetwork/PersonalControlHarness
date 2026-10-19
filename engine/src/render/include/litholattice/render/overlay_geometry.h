// CPU-side overlay geometry shared by the render backends: HUD quads (rects + font glyphs), the
// HUD font atlas, the block-highlight outline and the crosshair. Backends only upload and draw.
#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include "litholattice/core/math.h"
#include "litholattice/render/renderer.h"

namespace litholattice {

/// One HUD vertex in pixels (origin top-left). u < 0 marks a solid rectangle (no font lookup).
struct HudVertex { float x, y, u, v, r, g, b, a; };
static_assert(sizeof(HudVertex) == 8 * sizeof(float), "HUD vertex layout");

/// The baked HUD font as a single-channel (R8) atlas: 16 x 6 glyph cells.
struct HudFontAtlas {
  int width = 0, height = 0;
  std::vector<uint8_t> pixels;  ///< width * height coverage values, row-major
};

/// Builds the font atlas from the baked glyph table (font_data.h).
HudFontAtlas buildHudFontAtlas();

/// Appends the HUD as triangles (6 vertices per quad): rects first, then each text with a drop
/// shadow, then the UI draws in order (clipped). Clears `out` first.
void buildHudVertices(const HudLayer& hud, std::vector<HudVertex>& out);

/// Outline of block (x, y, z) as 12 line segments (24 endpoints), slightly inflated.
std::array<Vec3, 24> blockOutlineLines(int x, int y, int z);

/// Crosshair as 2 line segments in normalized device coordinates (y up), aspect corrected.
std::array<Vec3, 4> crosshairLines(int width, int height);

}  // namespace litholattice
