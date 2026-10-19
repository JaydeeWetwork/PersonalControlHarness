// UI theme: per-widget-class styles (colours, padding, border) and the font metrics, loaded
// from JSON (ui/theme.json in the pack stack). Missing values inherit from the "default" style,
// so a pack can recolour one widget class with a few lines.
#pragma once
#include <map>
#include <string>
#include <string_view>
#include "litholattice/core/json.h"
#include "litholattice/ui/geometry.h"

namespace litholattice::ui {

/// Colours and spacing of one widget class ("button", "panel", ...).
struct Style {
  Color background{0.12f, 0.12f, 0.14f, 0.92f};
  Color hover{0.22f, 0.22f, 0.26f, 0.95f};
  Color pressed{0.32f, 0.34f, 0.42f, 1.f};
  Color disabled{0.10f, 0.10f, 0.10f, 0.6f};
  Color text{0.92f, 0.92f, 0.92f, 1.f};
  Color textDisabled{0.5f, 0.5f, 0.5f, 1.f};
  Color accent{0.35f, 0.62f, 1.f, 1.f};     ///< focus ring, selection, fill of bars / checks
  Color border{0.f, 0.f, 0.f, 0.6f};
  float borderWidth = 1.f;
  Edges padding = Edges::all(4.f);
  float textScale = 1.f;
};

class Theme {
 public:
  /// Built-in look (used when no pack provides ui/theme.json).
  static Theme defaults();
  /// Parses `{"glyph": [w, h], "scale": s, "styles": {"default": {...}, "button": {...}}}`.
  /// Colours are [r, g, b] or [r, g, b, a] (0..1) or "#rrggbb[aa]". False + `err` on error.
  static bool parse(const Json& json, Theme& out, std::string& err);
  static bool parse(const std::string& text, Theme& out, std::string& err);

  /// Style for a widget class; falls back to "default".
  const Style& style(std::string_view widgetClass) const;
  bool hasStyle(std::string_view widgetClass) const { return styles_.find(widgetClass) != styles_.end(); }

  float scale() const { return scale_; }
  void setScale(float s) { scale_ = s; }
  /// Glyph cell at scale 1 (the font is monospaced).
  Vec2 glyph() const { return glyph_; }
  /// Glyph cell in pixels at `textScale` * theme scale.
  Vec2 glyphSize(float textScale = 1.f) const { return {glyph_.x * textScale * scale_, glyph_.y * textScale * scale_}; }
  float textWidth(std::string_view text, float textScale = 1.f) const { return float(text.size()) * glyphSize(textScale).x; }

 private:
  std::map<std::string, Style, std::less<>> styles_;
  Vec2 glyph_{8.f, 16.f};
  float scale_ = 1.f;
};

}  // namespace litholattice::ui
