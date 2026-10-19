// Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
// concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
// from the mathematical definition; no third-party code referenced. Underlying patent
// expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
// ambition of No Man's Sky (Hello Games).

// Self-test for the 2D Litho Formula (outline / SVG / texture / presets).
// Conventions follow litho_formula_selftest.cpp: a local Checker printing
// "  [PASS]/[FAIL]" lines. The core contract under test is the identity:
// the outline is lithoR sampled, nothing else — expectations are computed
// from lithoR itself plus hand-derived closed forms, never from a second
// shape implementation.
#include "litholattice/procgen/litho_formula_2d.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace litholattice::procgen::litho_formula {
namespace {

class Checker {
 public:
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }

 private:
  int fails_ = 0;
};

constexpr double kTwoPi = 6.28318530717958647692;

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

bool nearPx(const uint8_t* px, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  auto close = [](uint8_t v, uint8_t want) {
    return std::abs(static_cast<int>(v) - static_cast<int>(want)) <= 2;
  };
  return close(px[0], r) && close(px[1], g) && close(px[2], b) && px[3] == a;
}

double outlineArea(const std::vector<Vec2>& pts) {
  double sum = 0.0;
  for (size_t i = 0; i < pts.size(); ++i) {
    const Vec2& a = pts[i];
    const Vec2& b = pts[(i + 1) % pts.size()];
    sum += a.x * b.y - b.x * a.y;
  }
  return 0.5 * sum;
}

/// The identity, for one (params, segments) pair: point count, radius ==
/// lithoR(phi), direction == (cos phi, sin phi), counter-clockwise order.
bool identityHolds(Checker& check, const LithoParams& p, int segments, const char* label) {
  const std::vector<Vec2> pts = lithoOutline2D(p, segments);
  bool ok = pts.size() == static_cast<size_t>(segments);
  bool ccw = true;
  for (int i = 0; i < segments && ok; ++i) {
    const double phi = kTwoPi * i / segments;
    const double r = lithoR(phi, p);
    const double got = std::hypot(pts[i].x, pts[i].y);
    if (!near(got, r, 1e-9 * std::max(1.0, r))) ok = false;
    if (r > 0.0) {
      if (!near(pts[i].x / r, std::cos(phi), 1e-9) || !near(pts[i].y / r, std::sin(phi), 1e-9))
        ok = false;
      const Vec2& a = pts[i];
      const Vec2& b = pts[(i + 1) % pts.size()];
      if (a.x * b.y - b.x * a.y <= 0.0) ccw = false;  // cross <= 0 breaks CCW order
    }
  }
  check(ok, std::string("identity: outline is lithoR sampled (") + label + ")");
  check(ccw, std::string("identity: angles monotone CCW (") + label + ")");
  return ok;
}

}  // namespace

int runLithoFormula2DSelfTest() {
  Checker check;
  std::printf("litho_formula_2d selftest:\n");

  // -- Identity: the outline IS lithoR, sampled -----------------------------
  identityHolds(check, LithoParams{1.0, 1.0, 4.0, 2.0, 2.0, 2.0}, 360, "circle anchor, 360");
  identityHolds(check, LithoParams{1.0, 1.0, 5.0, 1.0, 0.5, 0.5}, 5, "star5 params, 5");
  identityHolds(check, LithoParams{1.3, 0.7, 7.0, 2.5, 0.8, 3.1}, 64, "asymmetric set, 64");
  identityHolds(check, LithoParams{1.8, 1.0, 4.0, 0.6, 0.6, 0.6}, 3, "leaf params, 3");
  identityHolds(check, LithoParams{2.0, 2.0, 0.0, 2.0, 2.0, 2.0}, 128, "m=0 set, 128");

  // -- Known shape: m = 0 collapses to a constant radius --------------------
  // base = |cos(0)/2|^n2 + 0 = 2^-n2; r = base^(-1/n1) = 2^(n2/n1) = 2
  // whenever n1 == n2, regardless of the (irrelevant) n3.
  for (const double n : {1.0, 2.0, 4.0}) {
    const std::vector<Vec2> pts = lithoOutline2D(LithoParams{2.0, 2.0, 0.0, n, n, 0.25}, 64);
    bool ok = pts.size() == 64;
    for (const Vec2& q : pts)
      if (!near(std::hypot(q.x, q.y), 2.0, 1e-12)) ok = false;
    check(ok, "known shape: m=0, a=b=2, n1==n2 -> every radius == 2");
  }

  // -- Segment clamping + degenerate params ---------------------------------
  check(lithoOutline2D(LithoParams{}, 0).size() == 3, "segments: 0 clamps to 3");
  check(lithoOutline2D(LithoParams{}, 100000).size() == 8192, "segments: huge clamps to 8192");
  {
    LithoParams bad;
    bad.a = std::numeric_limits<double>::quiet_NaN();
    const std::vector<Vec2> pts = lithoOutline2D(bad, 32);
    bool ok = pts.size() == 32;
    for (const Vec2& q : pts)
      if (!std::isfinite(q.x) || !std::isfinite(q.y) || q.x != 0.0 || q.y != 0.0) ok = false;
    check(ok, "degenerate: NaN param collapses outline to the origin, all finite");
  }

  // -- SVG document ----------------------------------------------------------
  {
    const LithoParams star = *lithoPreset2D("star5");
    const std::string svg = lithoSvg2D(star, LithoStyle2D{}, 96);
    check(svg.rfind("<svg", 0) == 0, "svg: standalone document");
    size_t lCount = 0, zCount = 0;
    for (char c : svg) {
      if (c == 'L') ++lCount;
      if (c == 'Z') ++zCount;
    }
    check(lCount == 95 && zCount == 1, "svg: one closed path, segment count of points");
    check(svg.find("#A970FF") != std::string::npos, "svg: default fill color present");
    check(svg == lithoSvg2D(star, LithoStyle2D{}, 96), "svg: deterministic (identical bytes)");

    LithoStyle2D styled;
    styled.fill = {255, 140, 26, 255};
    styled.stroke = {16, 16, 24, 255};
    styled.strokeWidth = 0.06;
    styled.hasBackground = true;
    styled.background = {10, 10, 18, 255};
    const std::string svg2 = lithoSvg2D(star, styled, 48);
    check(svg2.find("#FF8C1A") != std::string::npos && svg2.find("#101018") != std::string::npos &&
              svg2.find("<rect") != std::string::npos && svg2.find("#0A0A12") != std::string::npos,
          "svg: custom fill/stroke/background colors present");
  }

  // -- Raster textures ---------------------------------------------------------
  {
    const LithoParams star = *lithoPreset2D("star5");
    std::string err;
    const auto img = lithoTexture2D(star, LithoStyle2D{}, 128, 128, 256, &err);
    check(img.has_value(), "texture: star5 rasterizes");
    if (img) {
      check(img->width() == 128 && img->height() == 128, "texture: dimensions honored");
      check(nearPx(img->pixel(64, 64), 169, 112, 255, 255), "texture: star center is fill color");
      check(img->pixel(0, 0)[3] == 0 && img->pixel(127, 0)[3] == 0 &&
                img->pixel(0, 127)[3] == 0 && img->pixel(127, 127)[3] == 0,
            "texture: star corners transparent");
    }
    // Background style: corners take the background color instead. The
    // circle's viewBox is square, so decodeSvg's aspect fit fills the
    // whole image with it (a non-square viewBox would leave transparent
    // fit-padding at the image corners — decodeSvg semantics, by design).
    LithoStyle2D bgStyle;
    bgStyle.hasBackground = true;
    bgStyle.background = {10, 10, 18, 255};
    const auto imgBg = lithoTexture2D(*lithoPreset2D("circle"), bgStyle, 64, 64, 128, &err);
    check(imgBg && nearPx(imgBg->pixel(0, 0), 10, 10, 18, 255),
          "texture: background style fills corners");
  }
  {
    const LithoParams circle = *lithoPreset2D("circle");
    LithoStyle2D orange;
    orange.fill = {255, 140, 26, 255};
    const auto img = lithoTexture2D(circle, orange, 128, 128, 256, nullptr);
    check(img.has_value(), "texture: circle rasterizes");
    if (img) {
      const bool row = nearPx(img->pixel(32, 64), 255, 140, 26, 255) &&
                       nearPx(img->pixel(64, 64), 255, 140, 26, 255) &&
                       nearPx(img->pixel(96, 64), 255, 140, 26, 255);
      check(row, "texture: circle center row filled across the span");
      check(img->pixel(0, 0)[3] == 0, "texture: circle corner transparent");
    }
  }

  // -- Presets -------------------------------------------------------------------
  {
    const char* required[] = {"circle", "square", "diamond", "star5",
                              "flower", "gear",   "leaf",    "blob"};
    bool allFound = lithoPresets2D().size() >= 8;
    for (const char* name : required)
      if (!lithoPreset2D(name)) allFound = false;
    check(allFound, "presets: all 8 named presets present");
    check(lithoPreset2D("no_such_preset") == nullptr, "presets: unknown name -> nullptr");
    for (const LithoPreset2D& preset : lithoPresets2D()) {
      const std::vector<Vec2> pts = lithoOutline2D(preset.params, 256);
      bool finite = pts.size() == 256;
      for (const Vec2& q : pts)
        if (!std::isfinite(q.x) || !std::isfinite(q.y)) finite = false;
      check(finite, std::string("preset ") + std::string(preset.name) + ": finite outline");
      check(outlineArea(pts) > 0.05,
            std::string("preset ") + std::string(preset.name) + ": nonzero area");
      const auto img = lithoTexture2D(preset.params, LithoStyle2D{}, 64, 64, 128, nullptr);
      size_t opaque = 0;
      if (img)
        for (size_t i = 3; i < img->pixels().size(); i += 4)
          if (img->pixels()[i] > 0) ++opaque;
      check(img && opaque > 0,
            std::string("preset ") + std::string(preset.name) + ": texture has opaque pixels");
    }
  }

  std::printf("litho_formula_2d selftest: %s (%d failure%s)\n", check.fails() ? "FAILED" : "OK",
              check.fails(), check.fails() == 1 ? "" : "s");
  return check.fails();
}

}  // namespace litholattice::procgen::litho_formula
