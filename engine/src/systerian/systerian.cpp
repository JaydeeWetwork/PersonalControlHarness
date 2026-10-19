// Systerian codec implementation. The geometry and SVG rendering are a
// port of JD's reference generator (generate.py, the parametric model in
// ~/workspace/user/files/cistercian/): 9 unit strokes + mirror/flip place
// transforms, rivets at every stroke-graph vertex, halo pass beneath the
// main pass. The masters that generator produced are the verification
// reference (see systerian_selftest.cpp).
#include "litholattice/systerian/isysterian.h"

#include <cmath>
#include <cstdio>

namespace litholattice::systerian {
namespace {

constexpr SysterianSegment kStave{{50.0, 10.0}, {50.0, 130.0}};

/// The 9 unit strokes (upper-right place), as segment lists — the ONLY
/// baked glyph data in the system; everything else is transforms.
const std::vector<SysterianSegment>& unitStrokes(int digit) {
  static const std::vector<SysterianSegment> kDigits[10] = {
      {},                                                              // 0
      {{{50, 10}, {80, 10}}},                                          // 1
      {{{50, 50}, {80, 50}}},                                          // 2
      {{{50, 10}, {80, 40}}},                                          // 3
      {{{50, 50}, {80, 20}}},                                          // 4
      {{{50, 10}, {80, 10}}, {{80, 10}, {50, 40}}},                    // 5
      {{{78, 12}, {78, 42}}},                                          // 6 (detached dash)
      {{{50, 10}, {80, 10}}, {{80, 10}, {80, 40}}},                    // 7
      {{{50, 50}, {80, 50}}, {{80, 50}, {80, 80}}},                    // 8
      {{{50, 10}, {80, 10}}, {{80, 10}, {80, 40}}, {{80, 40}, {50, 40}}},  // 9
  };
  return kDigits[digit];
}

SysterianSegment mirrored(SysterianSegment s) {  // left-right: x -> 100 - x
  return {{kGridWidth - s.a.x, s.a.y}, {kGridWidth - s.b.x, s.b.y}};
}
SysterianSegment flipped(SysterianSegment s) {  // top-bottom: y -> 140 - y
  return {{s.a.x, kGridHeight - s.a.y}, {s.b.x, kGridHeight - s.b.y}};
}

/// One place's digit strokes (no stave), transformed from the unit form.
std::vector<SysterianSegment> placeStrokes(SysterianPlace place, int digit) {
  std::vector<SysterianSegment> segs = unitStrokes(digit);
  const bool mirror = place == SysterianPlace::Tens || place == SysterianPlace::Thousands;
  const bool flip = place == SysterianPlace::Hundreds || place == SysterianPlace::Thousands;
  if (mirror)
    for (SysterianSegment& s : segs) s = mirrored(s);
  if (flip)
    for (SysterianSegment& s : segs) s = flipped(s);
  return segs;
}

SysterianPoint sub(SysterianPoint a, SysterianPoint b) { return {a.x - b.x, a.y - b.y}; }
double cross(SysterianPoint a, SysterianPoint b) { return a.x * b.y - a.y * b.x; }

/// Segment-segment intersection point, or nullopt (parallel/collinear
/// segments never intersect — same rule as the generator).
std::optional<SysterianPoint> segIntersection(const SysterianSegment& s1,
                                              const SysterianSegment& s2) {
  constexpr double kEps = 1e-9;
  const SysterianPoint p = s1.a, r = sub(s1.b, s1.a);
  const SysterianPoint q = s2.a, s = sub(s2.b, s2.a);
  const double denom = cross(r, s);
  const SysterianPoint qp = sub(q, p);
  if (std::abs(denom) < kEps) return std::nullopt;
  const double t = cross(qp, s) / denom;
  const double u = cross(qp, r) / denom;
  if (t < -kEps || t > 1 + kEps || u < -kEps || u > 1 + kEps) return std::nullopt;
  return SysterianPoint{p.x + t * r.x, p.y + t * r.y};
}

/// JD's rivet rule: every vertex of the stroke graph — all segment
/// endpoints plus all pairwise crossings — deduplicated by coordinate
/// (points closer than 0.5 grid units are the same rivet), in
/// first-occurrence order.
std::vector<SysterianPoint> computeRivets(const std::vector<SysterianSegment>& segs) {
  std::vector<SysterianPoint> pts;
  for (const SysterianSegment& s : segs) {
    pts.push_back(s.a);
    pts.push_back(s.b);
  }
  for (size_t i = 0; i < segs.size(); ++i)
    for (size_t j = i + 1; j < segs.size(); ++j)
      if (const auto p = segIntersection(segs[i], segs[j])) pts.push_back(*p);
  std::vector<SysterianPoint> uniq;
  for (const SysterianPoint& p : pts) {
    bool dup = false;
    for (const SysterianPoint& q : uniq)
      if (std::hypot(p.x - q.x, p.y - q.y) < 0.5) {
        dup = true;
        break;
      }
    if (!dup) uniq.push_back(p);
  }
  return uniq;
}

SysterianGlyph makeGlyph(std::vector<SysterianSegment> segs) {
  SysterianGlyph g;
  g.rivets = computeRivets(segs);
  g.segments = std::move(segs);
  return g;
}

// --- SVG number formatting -------------------------------------------------
// Mirrors the generator's Python f-string output: stroke coordinates print
// as integers when integral ("50"), the stave and the float parameters
// print as Python floats ("50.0", "6.0", "7.4"). Geometry is unaffected by
// the formatting choice; fidelity keeps diffs against the masters clean.

std::string fmtTrimmed(double v) {
  char buf[64];
  if (v == std::trunc(v) && std::abs(v) < 1e15)
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
  else
    std::snprintf(buf, sizeof(buf), "%.6g", v);
  return buf;
}

std::string fmtFloat(double v) {
  char buf[64];
  if (v == std::trunc(v) && std::abs(v) < 1e15)
    std::snprintf(buf, sizeof(buf), "%lld.0", static_cast<long long>(v));
  else
    std::snprintf(buf, sizeof(buf), "%.6g", v);
  return buf;
}

std::string fmtFixed2(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.2f", v);
  return buf;
}

/// One glyph's render body (no <svg> wrapper): halo path, halo rivets,
/// main path, main rivets — the generator's draw order. `dx` shifts the
/// glyph right (text composition); at dx == 0 the number formatting matches
/// the reference masters exactly.
std::string glyphBody(const SysterianGlyph& g, const SysterianStyle& style, double dx) {
  std::string d;
  for (size_t i = 0; i < g.segments.size(); ++i) {
    const SysterianSegment& s = g.segments[i];
    // Segment 0 is the stave, which the generator stores as floats.
    const bool floatStyle = (i == 0 && dx == 0.0);
    auto coord = [&](double base, double off) {
      if (off == 0.0) return floatStyle ? fmtFloat(base) : fmtTrimmed(base);
      return fmtTrimmed(base + off);
    };
    d += 'M' + coord(s.a.x, dx) + ' ' + coord(s.a.y, 0.0);
    d += 'L' + coord(s.b.x, dx) + ' ' + coord(s.b.y, 0.0);
  }
  const std::string halo = style.haloColor.hex();
  std::string out;
  out += "<path d=\"" + d + "\" fill=\"none\" stroke=\"" + halo + "\" stroke-width=\"" +
         fmtFloat(style.strokeWidth + 2 * style.haloWidth) +
         "\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>";
  for (const SysterianPoint& p : g.rivets)
    out += "<circle cx=\"" + fmtFixed2(p.x + dx) + "\" cy=\"" + fmtFixed2(p.y) + "\" r=\"" +
           fmtFloat(style.dotRadius + style.haloWidth) + "\" fill=\"" + halo + "\"/>";
  out += "<path d=\"" + d + "\" fill=\"none\" stroke=\"" + style.lineColor.hex() +
         "\" stroke-width=\"" + fmtFloat(style.strokeWidth) +
         "\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>";
  for (const SysterianPoint& p : g.rivets)
    out += "<circle cx=\"" + fmtFixed2(p.x + dx) + "\" cy=\"" + fmtFixed2(p.y) + "\" r=\"" +
           fmtFloat(style.dotRadius) + "\" fill=\"" + style.dotColor.hex() + "\"/>";
  return out;
}

std::string svgDocument(double width, double height, const std::string& body) {
  return "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " + fmtTrimmed(width) + " " +
         fmtTrimmed(height) + "\">" + body + "</svg>";
}

std::optional<Image> rasterize(const std::string& svg, int width, int height, std::string* err) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(svg.data());
  return Image::decodeSvg(std::span<const uint8_t>(bytes, svg.size()), width, height, err);
}

}  // namespace

// --- Value codec -----------------------------------------------------------

SysterianDigits decomposeValue(uint32_t value) {
  uint32_t n = value % 10000;
  SysterianDigits d;
  d.thousands = static_cast<int>(n / 1000);
  n %= 1000;
  d.hundreds = static_cast<int>(n / 100);
  n %= 100;
  d.tens = static_cast<int>(n / 10);
  d.units = static_cast<int>(n % 10);
  return d;
}

uint32_t composeValue(SysterianDigits d) {
  auto dig = [](int v) { return static_cast<uint32_t>(((v % 10) + 10) % 10); };
  return dig(d.units) + 10 * dig(d.tens) + 100 * dig(d.hundreds) + 1000 * dig(d.thousands);
}

uint32_t valueForAscii(char c) { return static_cast<uint32_t>(static_cast<unsigned char>(c)); }

// --- Geometry ---------------------------------------------------------------

SysterianGlyph componentGlyph(SysterianPlace place, int digit) {
  std::vector<SysterianSegment> segs{kStave};
  if (digit >= 1 && digit <= 9) {
    std::vector<SysterianSegment> strokes = placeStrokes(place, digit);
    segs.insert(segs.end(), strokes.begin(), strokes.end());
  }
  return makeGlyph(std::move(segs));
}

SysterianGlyph composedGlyph(uint32_t value) {
  const SysterianDigits d = decomposeValue(value);
  std::vector<SysterianSegment> segs{kStave};
  // Generator's composition order: units, tens, hundreds, thousands.
  const struct {
    SysterianPlace place;
    int digit;
  } places[4] = {{SysterianPlace::Units, d.units},
                 {SysterianPlace::Tens, d.tens},
                 {SysterianPlace::Hundreds, d.hundreds},
                 {SysterianPlace::Thousands, d.thousands}};
  for (const auto& p : places) {
    if (p.digit == 0) continue;
    std::vector<SysterianSegment> strokes = placeStrokes(p.place, p.digit);
    segs.insert(segs.end(), strokes.begin(), strokes.end());
  }
  return makeGlyph(std::move(segs));
}

// --- Style ------------------------------------------------------------------

std::string SysterianColor::hex() const {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
  return buf;
}

SysterianStyle SysterianStyle::violetOnDark() {
  return {SysterianColor::fromHex(0xA970FF), SysterianColor::fromHex(0xFF6F00),
          SysterianColor::fromHex(0xFF3B30), 6.0, 5.4, 2.0};
}
SysterianStyle SysterianStyle::violetOnLight() {
  return {SysterianColor::fromHex(0xA970FF), SysterianColor::fromHex(0xFF6F00),
          SysterianColor::fromHex(0x2F7CF6), 6.0, 5.4, 2.0};
}
SysterianStyle SysterianStyle::whiteOnDark() {
  return {SysterianColor::fromHex(0xFFFFFF), SysterianColor::fromHex(0xFF6F00),
          SysterianColor::fromHex(0xFF3B30), 6.0, 5.4, 2.0};
}
SysterianStyle SysterianStyle::whiteOnLight() {
  return {SysterianColor::fromHex(0xFFFFFF), SysterianColor::fromHex(0xFF6F00),
          SysterianColor::fromHex(0x2F7CF6), 6.0, 5.4, 2.0};
}

// --- Systerian (concrete ISysterian) ----------------------------------------

std::string Systerian::svgForValue(uint32_t value, const SysterianStyle& style) const {
  return svgDocument(kGridWidth, kGridHeight, glyphBody(composedGlyph(value), style, 0.0));
}

std::string Systerian::svgForChar(char c, const SysterianStyle& style) const {
  return svgForValue(valueForAscii(c), style);
}

std::string Systerian::svgForText(std::string_view text, const SysterianStyle& style) const {
  if (text.empty()) return svgDocument(kGridWidth, kGridHeight, "");
  const double width =
      static_cast<double>(text.size()) * kGridWidth + static_cast<double>(text.size() - 1) * kTextGap;
  std::string body;
  for (size_t i = 0; i < text.size(); ++i)
    body += glyphBody(composedGlyph(valueForAscii(text[i])), style,
                      static_cast<double>(i) * (kGridWidth + kTextGap));
  return svgDocument(width, kGridHeight, body);
}

std::optional<Image> Systerian::pngForValue(uint32_t value, const SysterianStyle& style, int width,
                                            int height, std::string* err) const {
  return rasterize(svgForValue(value, style), width, height, err);
}

std::optional<Image> Systerian::pngForChar(char c, const SysterianStyle& style, int width,
                                           int height, std::string* err) const {
  return rasterize(svgForChar(c, style), width, height, err);
}

std::optional<Image> Systerian::pngForText(std::string_view text, const SysterianStyle& style,
                                           int width, int height, std::string* err) const {
  return rasterize(svgForText(text, style), width, height, err);
}

}  // namespace litholattice::systerian
