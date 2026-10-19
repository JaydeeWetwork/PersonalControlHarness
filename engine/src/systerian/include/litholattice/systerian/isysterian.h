// Systerian: the engine's certificate-signing script standard.
//
// Systerian is LithoKraft's signing script — the glyph language the
// certificate table renders. (The name is in-fiction; the historical system
// it adapts is the 13th-century Cistercian monastic numeral cipher,
// vertical-stave form.) A value 0..9999 is ONE glyph: a vertical stave plus
// up to four digit strokes — units upper-right, tens upper-left (mirrored),
// hundreds lower-right (flipped), thousands lower-left (mirrored + flipped).
// Zero is a bare stave. Every vertex of the stroke graph — junctions,
// crossings, corners, free ends — carries a rivet dot (JD's rivet rule).
//
// The codec is purely generative: geometry is derived from the 9 unit
// strokes plus the mirror/flip place transforms, so no per-glyph tables are
// baked and no glyph asset files are needed at runtime. Appearance is data
// (SysterianStyle), never hardcoded in the geometry path, so a future
// creator tool can restyle colors/widths freely. Rendering reproduces JD's
// reference generator (generate.py) exactly: a halo pass (same geometry,
// drawn larger, in the halo color) beneath the main pass, transparent
// background.
//
// ASCII layer: a printable ASCII char (32..126) maps to the glyph of its
// own code point ('A' = 65 -> glyph 65) — certificate text that players can
// decode once they realize the glyphs ARE the codes.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "litholattice/assets/image.h"

namespace litholattice::systerian {

/// Native glyph grid (the generator's viewBox): 100 x 140 units.
inline constexpr double kGridWidth = 100.0;
inline constexpr double kGridHeight = 140.0;
/// Horizontal gap between glyph cells in text composition.
inline constexpr double kTextGap = 16.0;
/// Largest value one glyph can hold (four decimal places).
inline constexpr uint32_t kMaxValue = 9999;

/// Decimal place of a digit stroke within a composed glyph.
enum class SysterianPlace : uint8_t { Units = 0, Tens = 1, Hundreds = 2, Thousands = 3 };

struct SysterianPoint {
  double x = 0;
  double y = 0;
  friend bool operator==(const SysterianPoint&, const SysterianPoint&) = default;
};

struct SysterianSegment {
  SysterianPoint a;
  SysterianPoint b;
  friend bool operator==(const SysterianSegment&, const SysterianSegment&) = default;
};

/// One glyph's full stroke graph, in the 100x140 grid space.
/// `segments` is stave-first, then digit strokes in place order
/// (units, tens, hundreds, thousands) — the generator's draw order.
/// `rivets` is the deduplicated vertex set (JD's rivet rule), in
/// first-occurrence order: segment endpoints, then pairwise crossings.
struct SysterianGlyph {
  std::vector<SysterianSegment> segments;
  std::vector<SysterianPoint> rivets;
};

/// A value split into its four decimal digits.
struct SysterianDigits {
  int units = 0;
  int tens = 0;
  int hundreds = 0;
  int thousands = 0;
  friend bool operator==(const SysterianDigits&, const SysterianDigits&) = default;
};

/// Split a value into decimal digits. Only the low four digits are
/// representable (the script is a 4-place system): value is taken mod 10000.
SysterianDigits decomposeValue(uint32_t value);
/// Inverse of decomposeValue (digits are taken mod 10 each).
uint32_t composeValue(SysterianDigits digits);
/// The ASCII rule: a character's glyph value is its own code point.
uint32_t valueForAscii(char c);

/// The component glyph for one (place, digit): stave + that digit's stroke.
/// Digit 0 (or out of range) yields the bare stave — an empty place.
SysterianGlyph componentGlyph(SysterianPlace place, int digit);
/// The composed glyph for a value 0..9999 (taken mod 10000): one stave plus
/// every nonzero place's stroke, rivets deduplicated across the whole graph.
SysterianGlyph composedGlyph(uint32_t value);

/// Straight-alpha RGBA color, 8-bit channels.
struct SysterianColor {
  uint8_t r = 0, g = 0, b = 0, a = 255;
  /// Build from a 0xRRGGBB value (alpha = opaque).
  static constexpr SysterianColor fromHex(uint32_t rgb) {
    return {static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8),
            static_cast<uint8_t>(rgb), 255};
  }
  /// "#RRGGBB", uppercase — the form the SVG masters use.
  std::string hex() const;
  friend bool operator==(const SysterianColor&, const SysterianColor&) = default;
};

/// Everything about a glyph's appearance that is NOT geometry. Style is
/// data: presets cover the locked colorways, and a creator tool can build
/// any other style by filling the fields in.
struct SysterianStyle {
  SysterianColor lineColor;  ///< digit strokes + stave
  SysterianColor dotColor;   ///< rivet dots
  SysterianColor haloColor;  ///< the under-drawn outline pass
  double strokeWidth = 6.0;  ///< main pass stroke width (grid units)
  double dotRadius = 5.4;    ///< rivet radius (grid units)
  double haloWidth = 2.0;    ///< halo grows strokes by 2x and dots by 1x this

  /// Violet lines #A970FF, orange rivets #FF6F00, trace-red halo #FF3B30
  /// (the dark-background variant of JD's locked primary colorway).
  static SysterianStyle violetOnDark();
  /// Violet on light: trace-blue halo #2F7CF6.
  static SysterianStyle violetOnLight();
  /// White lines #FFFFFF (neutral, for tinting) on dark: trace-red halo.
  static SysterianStyle whiteOnDark();
  /// White lines on light: trace-blue halo.
  static SysterianStyle whiteOnLight();
  friend bool operator==(const SysterianStyle&, const SysterianStyle&) = default;
};

/// The certificate-signing script interface. Implementations are stateless
/// and deterministic: same input, same output, every call.
class ISysterian {
 public:
  virtual ~ISysterian() = default;

  /// A character's glyph value (its own code point; 'A' -> 65).
  virtual uint32_t valueForChar(char c) const = 0;

  /// Stroke graph for a composed value, or for one (place, digit)
  /// component. Points are in the native 100x140 grid space.
  virtual SysterianGlyph glyphForValue(uint32_t value) const = 0;
  virtual SysterianGlyph glyphForComponent(SysterianPlace place, int digit) const = 0;

  /// A complete standalone SVG document for one value / one character /
  /// a line of text (one glyph cell per character, side by side at the
  /// native 100x140 with kTextGap between cells). Transparent background;
  /// halo pass beneath the main pass, exactly as the reference generator
  /// renders. Characters map by the ASCII rule.
  virtual std::string svgForValue(uint32_t value, const SysterianStyle& style) const = 0;
  virtual std::string svgForChar(char c, const SysterianStyle& style) const = 0;
  virtual std::string svgForText(std::string_view text, const SysterianStyle& style) const = 0;

  /// Rasterized RGBA pixels for one value / one character / a line of
  /// text, via the engine's SVG rasterizer (Image::decodeSvg): directly
  /// usable as a texture. width/height <= 0 selects the SVG's intrinsic
  /// size; otherwise the glyph is fit (aspect kept) and centered with
  /// transparent padding. nullopt + `err` on failure.
  virtual std::optional<Image> pngForValue(uint32_t value, const SysterianStyle& style,
                                           int width, int height, std::string* err) const = 0;
  virtual std::optional<Image> pngForChar(char c, const SysterianStyle& style, int width,
                                          int height, std::string* err) const = 0;
  virtual std::optional<Image> pngForText(std::string_view text, const SysterianStyle& style,
                                          int width, int height, std::string* err) const = 0;
};

/// The concrete ISysterian: a pure codec over the free geometry functions
/// above. Holds no state.
class Systerian final : public ISysterian {
 public:
  uint32_t valueForChar(char c) const override { return valueForAscii(c); }
  SysterianGlyph glyphForValue(uint32_t value) const override { return composedGlyph(value); }
  SysterianGlyph glyphForComponent(SysterianPlace place, int digit) const override {
    return componentGlyph(place, digit);
  }
  std::string svgForValue(uint32_t value, const SysterianStyle& style) const override;
  std::string svgForChar(char c, const SysterianStyle& style) const override;
  std::string svgForText(std::string_view text, const SysterianStyle& style) const override;
  std::optional<Image> pngForValue(uint32_t value, const SysterianStyle& style, int width,
                                   int height, std::string* err) const override;
  std::optional<Image> pngForChar(char c, const SysterianStyle& style, int width, int height,
                                  std::string* err) const override;
  std::optional<Image> pngForText(std::string_view text, const SysterianStyle& style, int width,
                                  int height, std::string* err) const override;
};

}  // namespace litholattice::systerian
