// Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
// concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
// from the mathematical definition; no third-party code referenced. Underlying patent
// expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
// ambition of No Man's Sky (Hello Games).

// 2D Litho Formula implementation (see litho_formula_2d.h). The outline is
// a direct sampling of lithoR — this file adds no shape math of its own,
// only the flattening (polar -> cartesian), document generation, preset
// parameters, and the hand-off to the assets rasterizer.
#include "litholattice/procgen/litho_formula_2d.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace litholattice::procgen::litho_formula {
namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr int kMinSegments = 3;
constexpr int kMaxSegments = 8192;

/// Deterministic coordinate formatting: fixed 6-decimal precision with
/// trailing zeros trimmed ("0.594604", "1", "-0.25"). -0 normalizes to 0.
std::string fmtNum(double v) {
  if (v == 0.0) return "0";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6f", v);
  std::string s(buf);
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  if (s == "-0") return "0";
  return s;
}

/// "#RRGGBB" (uppercase, the masters' convention).
std::string hexColor(const std::array<uint8_t, 4>& c) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", c[0], c[1], c[2]);
  return buf;
}

/// fill-opacity / stroke-opacity value for a non-opaque alpha.
std::string fmtAlpha(uint8_t a) { return fmtNum(a / 255.0); }

/// Path data for a closed polygon through `pts` (already in SVG y-down
/// coordinates): "M x0 y0 L x1 y2 ... Z".
std::string pathData(const std::vector<Vec2>& pts) {
  std::string d;
  for (size_t i = 0; i < pts.size(); ++i) {
    d += (i == 0) ? "M" : "L";
    d += fmtNum(pts[i].x);
    d += ' ';
    d += fmtNum(pts[i].y);
  }
  d += 'Z';
  return d;
}

}  // namespace

std::vector<Vec2> lithoOutline2D(const LithoParams& p, int segments) {
  const int n = std::clamp(segments, kMinSegments, kMaxSegments);
  std::vector<Vec2> out;
  out.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    const double phi = kTwoPi * static_cast<double>(i) / static_cast<double>(n);
    const double r = lithoR(phi, p);
    out.push_back(Vec2{r * std::cos(phi), r * std::sin(phi)});
  }
  return out;
}

std::string lithoSvg2D(const LithoParams& p, const LithoStyle2D& style, int segments) {
  // SVG renders y-down; negate y so the document shows the shape in its
  // mathematical orientation (y up).
  std::vector<Vec2> pts = lithoOutline2D(p, segments);
  for (Vec2& q : pts) q.y = -q.y;

  double minX = pts[0].x, maxX = pts[0].x, minY = pts[0].y, maxY = pts[0].y;
  for (const Vec2& q : pts) {
    minX = std::min(minX, q.x);
    maxX = std::max(maxX, q.x);
    minY = std::min(minY, q.y);
    maxY = std::max(maxY, q.y);
  }
  double w = maxX - minX, h = maxY - minY;
  // A collapsed dimension (degenerate params, a straight-line shape) gets
  // a unit extent so the viewBox stays valid.
  if (w <= 0.0) { minX -= 0.5; w = 1.0; }
  if (h <= 0.0) { minY -= 0.5; h = 1.0; }
  // Margin: 5% of the larger extent, plus half the stroke so an outline
  // stroke is never clipped by the viewBox edge.
  const double margin = 0.05 * std::max(w, h) + 0.5 * std::max(0.0, style.strokeWidth);
  minX -= margin;
  minY -= margin;
  w += 2.0 * margin;
  h += 2.0 * margin;

  std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"";
  svg += fmtNum(minX) + ' ' + fmtNum(minY) + ' ' + fmtNum(w) + ' ' + fmtNum(h);
  svg += "\">";
  if (style.hasBackground) {
    svg += "<rect x=\"" + fmtNum(minX) + "\" y=\"" + fmtNum(minY) + "\" width=\"" + fmtNum(w) +
           "\" height=\"" + fmtNum(h) + "\" fill=\"" + hexColor(style.background) + "\"";
    if (style.background[3] != 255) svg += " fill-opacity=\"" + fmtAlpha(style.background[3]) + "\"";
    svg += "/>";
  }
  svg += "<path d=\"" + pathData(pts) + "\"";
  if (style.fill[3] == 0) {
    svg += " fill=\"none\"";
  } else {
    svg += " fill=\"" + hexColor(style.fill) + "\"";
    if (style.fill[3] != 255) svg += " fill-opacity=\"" + fmtAlpha(style.fill[3]) + "\"";
  }
  if (style.strokeWidth > 0.0 && style.stroke[3] != 0) {
    svg += " stroke=\"" + hexColor(style.stroke) + "\"";
    if (style.stroke[3] != 255) svg += " stroke-opacity=\"" + fmtAlpha(style.stroke[3]) + "\"";
    svg += " stroke-width=\"" + fmtNum(style.strokeWidth) + "\" stroke-linejoin=\"round\"";
  }
  svg += "/></svg>";
  return svg;
}

std::optional<litholattice::Image> lithoTexture2D(const LithoParams& p, const LithoStyle2D& style,
                                                  int width, int height, int segments,
                                                  std::string* err) {
  const std::string svg = lithoSvg2D(p, style, segments);
  const auto* bytes = reinterpret_cast<const uint8_t*>(svg.data());
  return litholattice::Image::decodeSvg(std::span<const uint8_t>(bytes, svg.size()), width, height,
                                         err);
}

namespace {

// The preset table. Every entry is plain LithoParams — the shapes are the
// formula's, not baked geometry. Notes per preset record what the math
// does, including where a name is a silhouette rather than the literal
// object.
constexpr LithoPreset2D kPresets[] = {
    // m = 0: cos(0)=1, sin(0)=0 — constant radius a^(n2/n1) = 1.
    {"circle", LithoParams{1.0, 1.0, 0.0, 1.0, 1.0, 1.0}},
    // m = 4 with large equal n: the diagonal bulge of the m=4 family
    // sharpens into corners — a square standing on its corners' axes.
    {"square", LithoParams{1.0, 1.0, 4.0, 10.0, 10.0, 10.0}},
    // m = 4 with n < 1: pinched at the diagonals, pointed on the axes.
    {"diamond", LithoParams{1.0, 1.0, 4.0, 0.5, 0.5, 0.5}},
    // m = 5 with n2/n3 < 1: radius dips to ~0.59 between the five points.
    {"star5", LithoParams{1.0, 1.0, 5.0, 1.0, 0.5, 0.5}},
    // m = 6 with n > 2: each lobe's midpoint bulges (~1.41x) — petals.
    {"flower", LithoParams{1.0, 1.0, 6.0, 2.0, 4.0, 4.0}},
    // The square recipe at m = 12: twelve blocky lobes read as teeth.
    {"gear", LithoParams{1.0, 1.0, 12.0, 10.0, 10.0, 10.0}},
    // Leaf silhouette: the diamond family stretched by a != b (radius on
    // the +x axis reaches a^(n2/n1) = 1.8, on +y only b = 1). The bare
    // formula is mirror-symmetric on both axes, so this is a pointed
    // lens/leaf outline — no stem, no midrib asymmetry.
    {"leaf", LithoParams{1.8, 1.0, 4.0, 0.6, 0.6, 0.6}},
    // m = 3 with soft n: a gentle three-lobed wobble (~0.89 pinch).
    {"blob", LithoParams{1.0, 1.0, 3.0, 1.5, 1.5, 1.5}},
};

}  // namespace

std::span<const LithoPreset2D> lithoPresets2D() { return kPresets; }

const LithoParams* lithoPreset2D(std::string_view name) {
  for (const LithoPreset2D& preset : kPresets)
    if (preset.name == name) return &preset.params;
  return nullptr;
}

}  // namespace litholattice::procgen::litho_formula
