// Painter: what widgets draw with. Wraps the injected IUiRenderer with the theme and a clip
// stack (ClipScope keeps push / pop balanced).
#pragma once
#include <string_view>
#include <vector>
#include "litholattice/ui/theme.h"
#include "litholattice/ui/ui_renderer.h"

namespace litholattice::ui {

class Painter {
 public:
  Painter(IUiRenderer& renderer, const Theme& theme) : r_(renderer), theme_(theme) {}

  const Theme& theme() const { return theme_; }
  void fill(const Rect& r, Color c) { if (c.a > 0 && !r.empty()) r_.fillRect(r, c); }
  /// Border of width `w` inside `r`.
  void frame(const Rect& r, Color c, float w);
  void text(Vec2 pos, std::string_view s, Color c, float textScale = 1.f) {
    if (!s.empty()) r_.drawText(pos, s, c, textScale * theme_.scale());
  }
  /// Text centred vertically in `r`, left-aligned at r.x (or centred horizontally if `center`).
  void textIn(const Rect& r, std::string_view s, Color c, bool center = false, float textScale = 1.f);
  void image(const Rect& r, std::string_view name, Color tint) { r_.drawImage(r, name, tint); }

  /// Clips drawing to a rectangle for its lifetime.
  class ClipScope {
   public:
    ClipScope(Painter& p, const Rect& r) : p_(p) { p_.r_.pushClip(r); }
    ~ClipScope() { p_.r_.popClip(); }
    ClipScope(const ClipScope&) = delete;
    ClipScope& operator=(const ClipScope&) = delete;

   private:
    Painter& p_;
  };

 private:
  IUiRenderer& r_;
  const Theme& theme_;
};

}  // namespace litholattice::ui
