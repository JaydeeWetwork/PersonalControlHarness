#include "litholattice/ui/painter.h"

namespace litholattice::ui {

void Painter::frame(const Rect& r, Color c, float w) {
  if (c.a <= 0 || w <= 0 || r.empty()) return;
  fill({r.x, r.y, r.w, w}, c);
  fill({r.x, r.bottom() - w, r.w, w}, c);
  fill({r.x, r.y + w, w, r.h - 2 * w}, c);
  fill({r.right() - w, r.y + w, w, r.h - 2 * w}, c);
}

void Painter::textIn(const Rect& r, std::string_view s, Color c, bool center, float textScale) {
  const Vec2 g = theme_.glyphSize(textScale);
  const float w = float(s.size()) * g.x;
  const float x = center ? r.x + (r.w - w) * 0.5f : r.x;
  text({x, r.y + (r.h - g.y) * 0.5f}, s, c, textScale);
}

}  // namespace litholattice::ui
