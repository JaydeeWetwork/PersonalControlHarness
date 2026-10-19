// Systerian codec self-tests (ctest: engine_systerian). Exit code = failure
// count. The core test parses JD's reference generator masters (the SVG
// files generate.py produced) and compares their stroke endpoints and
// rivet centers against this codec's computed geometry, glyph by glyph —
// the C++ port must provably reproduce the generator.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "litholattice/systerian/isysterian.h"

namespace {

using namespace litholattice::systerian;

int g_fails = 0;
void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fails;
}
void checkf(bool ok, const std::string& what) { check(ok, what.c_str()); }

std::string referenceDir() {
  if (const char* env = std::getenv("SYSTERIAN_REFERENCE_DIR"); env && *env) return env;
#ifdef SYSTERIAN_REFERENCE_DIR
  return SYSTERIAN_REFERENCE_DIR;
#else
  return "";
#endif
}

bool readFile(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

// --- Reference SVG parsing ---------------------------------------------------
// A master SVG holds two <path> elements (halo + main pass, identical `d`)
// whose data is "M x y L x y" per segment, plus one <circle> per rivet per
// pass. Extract the segment endpoint pairs and the circle centers.

std::vector<SysterianSegment> parsePathSegments(const std::string& svg) {
  std::vector<SysterianPoint> pts;
  size_t pos = 0;
  while (true) {
    const size_t begin = svg.find("<path d=\"", pos);
    if (begin == std::string::npos) break;
    const size_t d0 = begin + 9;
    const size_t d1 = svg.find('"', d0);
    const std::string d = svg.substr(d0, d1 - d0);
    size_t i = 0;
    while (i < d.size()) {
      if (d[i] == 'M' || d[i] == 'L') {
        ++i;
        double xy[2];
        for (double& v : xy) {
          while (i < d.size() && d[i] == ' ') ++i;
          const char* begin2 = d.c_str() + i;
          char* end = nullptr;
          v = std::strtod(begin2, &end);
          i += static_cast<size_t>(end - begin2);
        }
        pts.push_back({xy[0], xy[1]});
      } else {
        ++i;
      }
    }
    pos = d1;
  }
  std::vector<SysterianSegment> segs;
  for (size_t i = 0; i + 1 < pts.size(); i += 2) segs.push_back({pts[i], pts[i + 1]});
  return segs;
}

std::vector<SysterianPoint> parseCircleCenters(const std::string& svg) {
  std::vector<SysterianPoint> pts;
  size_t pos = 0;
  while (true) {
    const size_t begin = svg.find("<circle cx=\"", pos);
    if (begin == std::string::npos) break;
    const size_t x0 = begin + 12;
    const size_t x1 = svg.find('"', x0);
    const size_t y0 = svg.find("cy=\"", x1) + 4;
    const size_t y1 = svg.find('"', y0);
    pts.push_back({std::stod(svg.substr(x0, x1 - x0)), std::stod(svg.substr(y0, y1 - y0))});
    pos = y1;
  }
  return pts;
}

// --- Set comparison (epsilon) -------------------------------------------------

bool nearPt(const SysterianPoint& a, const SysterianPoint& b, double eps) {
  return std::hypot(a.x - b.x, a.y - b.y) <= eps;
}

bool segNear(const SysterianSegment& a, const SysterianSegment& b, double eps) {
  return (nearPt(a.a, b.a, eps) && nearPt(a.b, b.b, eps)) ||
         (nearPt(a.a, b.b, eps) && nearPt(a.b, b.a, eps));
}

template <typename T, typename Near>
bool setsMatch(const std::vector<T>& mine, const std::vector<T>& ref, Near nearFn, double eps) {
  if (mine.size() != ref.size()) return false;
  std::vector<bool> used(ref.size(), false);
  for (const T& m : mine) {
    bool found = false;
    for (size_t i = 0; i < ref.size(); ++i)
      if (!used[i] && nearFn(m, ref[i], eps)) {
        used[i] = true;
        found = true;
        break;
      }
    if (!found) return false;
  }
  return true;
}

bool geometryMatches(const SysterianGlyph& g, const std::string& refSvg) {
  constexpr double kEps = 0.01;
  const std::vector<SysterianSegment> refSegs = parsePathSegments(refSvg);
  // The master draws every segment twice (halo + main pass): halve by
  // deduplicating identical segments.
  std::vector<SysterianSegment> uniqSegs;
  for (const SysterianSegment& s : refSegs) {
    bool dup = false;
    for (const SysterianSegment& u : uniqSegs)
      if (segNear(s, u, kEps)) dup = true;
    if (!dup) uniqSegs.push_back(s);
  }
  if (!setsMatch(g.segments, uniqSegs, segNear, kEps)) return false;
  // Same for rivets: two circles per rivet (one per pass), same center.
  const std::vector<SysterianPoint> refPts = parseCircleCenters(refSvg);
  std::vector<SysterianPoint> uniqPts;
  for (const SysterianPoint& p : refPts) {
    bool dup = false;
    for (const SysterianPoint& u : uniqPts)
      if (nearPt(p, u, kEps)) dup = true;
    if (!dup) uniqPts.push_back(p);
  }
  return setsMatch(g.rivets, uniqPts, nearPt, kEps);
}

std::string firstPathData(const std::string& svg) {
  const size_t begin = svg.find("<path d=\"");
  if (begin == std::string::npos) return "";
  const size_t d0 = begin + 9;
  return svg.substr(d0, svg.find('"', d0) - d0);
}

size_t countOccurrences(const std::string& hay, const char* needle) {
  size_t n = 0, pos = 0;
  while ((pos = hay.find(needle, pos)) != std::string::npos) {
    ++n;
    pos += 1;
  }
  return n;
}

}  // namespace

int main() {
  const Systerian codec;
  const ISysterian& api = codec;
  const SysterianStyle dark = SysterianStyle::violetOnDark();

  // -- ASCII rule + value codec ----------------------------------------------
  check(api.valueForChar('A') == 65, "ascii: 'A' -> 65");
  check(api.valueForChar(' ') == 32, "ascii: ' ' -> 32");
  check(api.valueForChar('~') == 126, "ascii: '~' -> 126");
  const SysterianDigits d2026 = decomposeValue(2026);
  check(d2026.units == 6 && d2026.tens == 2 && d2026.hundreds == 0 && d2026.thousands == 2,
        "codec: decompose(2026) = 6/2/0/2");
  check(composeValue(d2026) == 2026, "codec: compose(decompose(2026)) == 2026");
  int sweepBad = 0;
  for (uint32_t v = 0; v <= kMaxValue; ++v)
    if (composeValue(decomposeValue(v)) != v) ++sweepBad;
  check(sweepBad == 0, "codec: compose/decompose round-trips all 0..9999");

  // -- Geometry shape ----------------------------------------------------------
  const SysterianGlyph zero = api.glyphForValue(0);
  check(zero.segments.size() == 1 && zero.rivets.size() == 2,
        "glyph: 0 is the bare stave (1 segment, 2 rivets)");
  const SysterianGlyph nine9k = api.glyphForComponent(SysterianPlace::Thousands, 9);
  check(nine9k.segments.size() == 4 && nine9k.rivets.size() == 5,
        "glyph: thousands-9 component = stave + 3 strokes, 5 rivets");
  // 65 = tens 6 + units 5: composed geometry must equal the merge of its
  // two component glyphs (the composition the ASCII layer relies on).
  {
    const SysterianGlyph g65 = api.glyphForValue(65);
    std::vector<SysterianSegment> merged{api.glyphForComponent(SysterianPlace::Units, 5).segments[0]};
    for (const SysterianPlace place : {SysterianPlace::Units, SysterianPlace::Tens}) {
      const SysterianGlyph comp = api.glyphForComponent(place, place == SysterianPlace::Units ? 5 : 6);
      merged.insert(merged.end(), comp.segments.begin() + 1, comp.segments.end());
    }
    check(setsMatch(g65.segments, merged, segNear, 0.01),
          "glyph: 65 composes from its tens/units components");
  }

  // -- Geometry vs the reference masters ---------------------------------------
  const std::string refDir = referenceDir();
  check(!refDir.empty(), "masters: reference dir configured");
  if (!refDir.empty()) {
    const struct {
      SysterianPlace place;
      const char* name;
      uint32_t mult;
    } kPlaces[4] = {{SysterianPlace::Units, "units", 1},
                    {SysterianPlace::Tens, "tens", 10},
                    {SysterianPlace::Hundreds, "hundreds", 100},
                    {SysterianPlace::Thousands, "thousands", 1000}};
    for (const auto& pl : kPlaces)
      for (int digit = 1; digit <= 9; ++digit) {
        const uint32_t value = static_cast<uint32_t>(digit) * pl.mult;
        const std::string path =
            refDir + "/svg/violet/on_dark/cistercian_" + std::to_string(value) + ".svg";
        std::string svg;
        if (!readFile(path, svg)) {
          checkf(false, "masters: missing file " + path);
          continue;
        }
        checkf(geometryMatches(api.glyphForComponent(pl.place, digit), svg),
               "masters: component " + std::to_string(value) + " (" + pl.name + " " +
                   std::to_string(digit) + ") geometry matches");
      }
    for (const uint32_t value : {1111u, 2026u, 9999u}) {
      const std::string path =
          refDir + "/composed/on_dark/cistercian_" + std::to_string(value) + ".svg";
      std::string svg;
      if (!readFile(path, svg)) {
        checkf(false, "masters: missing file " + path);
        continue;
      }
      checkf(geometryMatches(api.glyphForValue(value), svg),
             "masters: composed " + std::to_string(value) + " geometry matches");
    }
  }

  // -- SVG output ----------------------------------------------------------------
  const std::string svg2026 = api.svgForValue(2026, dark);
  check(svg2026.rfind("<svg", 0) == 0 && svg2026.find("viewBox=\"0 0 100 140\"") != std::string::npos,
        "svg: standalone document on the native grid");
  check(svg2026.find("#A970FF") != std::string::npos && svg2026.find("#FF6F00") != std::string::npos &&
            svg2026.find("#FF3B30") != std::string::npos,
        "svg: violetOnDark carries line, rivet and halo colors");
  check(api.svgForValue(2026, dark) == svg2026, "svg: deterministic (same input, same string)");
  const std::string svgLight = api.svgForValue(2026, SysterianStyle::whiteOnLight());
  check(svgLight.find("#FFFFFF") != std::string::npos &&
            svgLight.find("#2F7CF6") != std::string::npos,
        "svg: whiteOnLight carries white lines and trace-blue halo");
  // A custom style recolors and nothing else: identical path geometry.
  {
    SysterianStyle custom = dark;
    custom.lineColor = SysterianColor::fromHex(0x112233);
    const std::string svgCustom = api.svgForValue(2026, custom);
    check(svgCustom.find("#112233") != std::string::npos &&
              firstPathData(svgCustom) == firstPathData(svg2026),
          "svg: custom style changes colors, not geometry");
  }
  // Text: one cell per char, side by side.
  {
    const std::string text = api.svgForText("AB", dark);
    const size_t expectCircles =
        2 * (api.glyphForValue(65).rivets.size() + api.glyphForValue(66).rivets.size());
    check(text.find("viewBox=\"0 0 216 140\"") != std::string::npos &&
              countOccurrences(text, "<circle") == expectCircles,
          "svg: text composes one 100x140 cell per char (16 gap)");
    check(api.svgForChar('A', dark) == api.svgForValue(65, dark),
          "svg: char renders its code-point glyph");
  }

  // -- Raster ---------------------------------------------------------------------
  {
    std::string err;
    const auto img = api.pngForValue(1, dark, 100, 140, &err);
    check(img.has_value(), "raster: pngForValue(1) decodes");
    if (img) {
      check(img->width() == 100 && img->height() == 140, "raster: requested size honored");
      size_t transparent = 0, opaque = 0;
      bool violetish = false;
      const std::vector<uint8_t>& px = img->pixels();
      for (size_t i = 0; i + 3 < px.size(); i += 4) {
        if (px[i + 3] == 0) ++transparent;
        if (px[i + 3] == 255) {
          ++opaque;
          if (std::abs(px[i] - 0xA9) <= 32 && std::abs(px[i + 1] - 0x70) <= 32 &&
              std::abs(px[i + 2] - 0xFF) <= 32)
            violetish = true;
        }
      }
      check(transparent > 0 && opaque > 0, "raster: transparent background, opaque strokes");
      check(violetish, "raster: violet line color present in pixels");
    }
    const auto big = api.pngForValue(2026, dark, 200, 280, &err);
    check(big && big->width() == 200 && big->height() == 280, "raster: 200x280 size honored");
    const auto textImg = api.pngForText("A", dark, 100, 140, &err);
    check(textImg && textImg->width() == 100 && textImg->height() == 140,
          "raster: text rasterizes");
    const auto charImg = api.pngForChar('A', dark, 0, 0, &err);
    check(charImg && charImg->width() == 100 && charImg->height() == 140,
          "raster: intrinsic size when width/height <= 0");
  }

  std::printf("systerian selftest: %s (%d failure%s)\n", g_fails ? "FAILED" : "OK", g_fails,
              g_fails == 1 ? "" : "s");
  return g_fails;
}
