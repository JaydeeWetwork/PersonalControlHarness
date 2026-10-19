#include "litholattice/render/hud_ui_renderer.h"
#include <algorithm>
#include <string>

namespace litholattice {

void HudUiRenderer::fillRect(const ui::Rect& r, ui::Color c) {
  HudDraw d;
  d.x = r.x;
  d.y = r.y;
  d.w = r.w;
  d.h = r.h;
  d.r = c.r;
  d.g = c.g;
  d.b = c.b;
  d.a = c.a;
  d.clip = clip();
  layer_.draws.push_back(std::move(d));
}

void HudUiRenderer::drawText(ui::Vec2 pos, std::string_view text, ui::Color c, float scale) {
  HudDraw d;
  d.x = pos.x;
  d.y = pos.y;
  d.r = c.r;
  d.g = c.g;
  d.b = c.b;
  d.a = c.a;
  d.scale = scale;
  d.text = std::string(text);
  d.clip = clip();
  layer_.draws.push_back(std::move(d));
}

void HudUiRenderer::drawImage(const ui::Rect& r, std::string_view image, ui::Color tint) {
  if (const Image* icon = icons_ ? icons_(image) : nullptr; icon && !icon->empty()) {
    const float cw = r.w / float(icon->width()), ch = r.h / float(icon->height());
    for (int y = 0; y < icon->height(); ++y)
      for (int x = 0; x < icon->width(); ++x) {
        const uint8_t* p = icon->pixel(x, y);
        if (p[3] < 128) continue;
        fillRect({r.x + float(x) * cw, r.y + float(y) * ch, cw, ch},
                 {p[0] / 255.f * tint.r, p[1] / 255.f * tint.g, p[2] / 255.f * tint.b, tint.a});
      }
    return;
  }
  const std::optional<ui::Color> swatch = images_ ? images_(image) : std::nullopt;
  const ui::Color base = swatch.value_or(ui::Color{1.f, 0.f, 1.f, 1.f});  // magenta: unknown image
  fillRect(r, {base.r * tint.r, base.g * tint.g, base.b * tint.b, base.a * tint.a});
}

void HudUiRenderer::pushClip(const ui::Rect& r) {
  const HudClip cur = clip();
  clips_.push_back({std::max(cur.x0, r.x), std::max(cur.y0, r.y), std::min(cur.x1, r.right()), std::min(cur.y1, r.bottom())});
}

void HudUiRenderer::popClip() {
  if (!clips_.empty()) clips_.pop_back();
}

Image downsampleIcon(const Image& src, int size) {
  if (src.empty() || size <= 0) return {};
  std::vector<uint8_t> out(size_t(size) * size_t(size) * 4);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      const int x0 = x * src.width() / size, x1 = std::max(x0 + 1, (x + 1) * src.width() / size);
      const int y0 = y * src.height() / size, y1 = std::max(y0 + 1, (y + 1) * src.height() / size);
      double c[3] = {0, 0, 0}, a = 0;
      int n = 0;
      for (int sy = y0; sy < y1; ++sy)
        for (int sx = x0; sx < x1; ++sx, ++n) {
          const uint8_t* p = src.pixel(sx, sy);
          for (int k = 0; k < 3; ++k) c[k] += p[k] * double(p[3]);
          a += p[3];
        }
      uint8_t* o = &out[(size_t(y) * size_t(size) + size_t(x)) * 4];
      for (int k = 0; k < 3; ++k) o[k] = uint8_t(a > 0 ? c[k] / a + 0.5 : 0);
      o[3] = uint8_t(n ? a / n + 0.5 : 0);
    }
  return Image(size, size, std::move(out));
}

}  // namespace litholattice
