// Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
// concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
// from the mathematical definition; no third-party code referenced. Underlying patent
// expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
// ambition of No Man's Sky (Hello Games).

/// The 2D Litho Formula: the 3D sampler's radial core (litho_formula.h),
/// flattened. A 2D shape IS one evaluator's outline — sample lithoR(phi)
/// over phi in [0, 2pi) and the radius becomes a closed flat silhouette:
///
///   outline(phi) = (lithoR(phi) * cos(phi), lithoR(phi) * sin(phi))
///
/// That identity is the design: leaves, decals, icons and emblems grown
/// from the same parametric DNA as the 3D supershapes, rendered as SVG
/// (vector master) or rasterized textures through the assets module's
/// SVG rasterizer (Image::decodeSvg). Pure and deterministic: identical
/// inputs produce identical outlines and identical SVG bytes.
///
/// Degenerate rails are inherited from lithoR (never NaN/inf): non-finite
/// parameters collapse every radius to 0, so the outline degenerates to
/// the origin point instead of poisoning the document.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "litholattice/assets/image.h"
#include "litholattice/procgen/litho_formula.h"

namespace litholattice::procgen::litho_formula {

/// Plain 2D point in shape space (mathematical orientation: y up).
struct Vec2 {
  double x = 0.0;
  double y = 0.0;
};

/// Rendering style for a 2D Litho shape. Style is data — colors are RGBA8
/// (straight alpha), stroke width is in viewBox (shape-space) units.
/// A fill with alpha 0 renders as no fill; a stroke width <= 0 (or a
/// stroke with alpha 0) renders as no stroke. The background is
/// transparent unless hasBackground is set.
struct LithoStyle2D {
  std::array<uint8_t, 4> fill{169, 112, 255, 255};    ///< JD violet (#A970FF)
  std::array<uint8_t, 4> stroke{0, 0, 0, 255};
  double strokeWidth = 0.0;                          ///< viewBox units; 0 = no stroke
  bool hasBackground = false;
  std::array<uint8_t, 4> background{0, 0, 0, 0};
};

/// The closed outline of a 2D Litho shape: `segments` points at
/// phi_i = 2*pi*i/segments, each exactly (lithoR(phi_i) cos, lithoR(phi_i)
/// sin). Counter-clockwise, y up. `segments` is clamped to [3, 8192].
/// The polygon is star-shaped w.r.t. the origin (radii are >= 0), so it
/// never self-intersects.
std::vector<Vec2> lithoOutline2D(const LithoParams& p, int segments = 128);

/// A complete standalone SVG document for the shape: viewBox fitted to
/// the outline's bounding box plus a small margin, one closed path
/// through the outline points, fill/stroke/background per `style`.
/// Shape y is negated so the document renders in mathematical
/// orientation (y up) under SVG's y-down coordinates. Deterministic:
/// identical inputs produce identical bytes.
std::string lithoSvg2D(const LithoParams& p, const LithoStyle2D& style, int segments = 128);

/// The shape as a raster texture: lithoSvg2D rasterized at width x
/// height through Image::decodeSvg (fit, centered, transparent padding).
/// nullopt + `err` when the rasterizer rejects the document.
std::optional<litholattice::Image> lithoTexture2D(const LithoParams& p, const LithoStyle2D& style,
                                                  int width, int height, int segments = 256,
                                                  std::string* err = nullptr);

/// One named entry of the 2D preset table.
struct LithoPreset2D {
  std::string_view name;
  LithoParams params;
};

/// The named 2D presets (circle, square, diamond, star5, flower, gear,
/// leaf, blob). Stable storage: pointers into this table never dangle.
std::span<const LithoPreset2D> lithoPresets2D();

/// Find a preset's params by name; nullptr when unknown (same
/// find-or-null convention as IStat::statDef / IVitals::vitalDef).
const LithoParams* lithoPreset2D(std::string_view name);

/// Self-test entry point (litho_formula_2d_selftest.cpp). Returns failure count.
int runLithoFormula2DSelfTest();

}  // namespace litholattice::procgen::litho_formula
