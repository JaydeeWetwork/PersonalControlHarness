#include "litholattice/render/overlay_geometry.h"
#include <algorithm>
#include <string>
#include "font_data.h"

namespace litholattice {
namespace {

constexpr int kAtlasW = 16 * font::kCellW, kAtlasH = 6 * font::kCellH;

void quad(std::vector<HudVertex>& out, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
          const std::array<float, 4>& c) {
  const float q[6][4] = {{x, y, u0, v0}, {x + w, y, u1, v0}, {x + w, y + h, u1, v1},
                         {x, y, u0, v0}, {x + w, y + h, u1, v1}, {x, y + h, u0, v1}};
  for (const auto& v : q) out.push_back({v[0], v[1], v[2], v[3], c[0], c[1], c[2], c[3]});
}

void text(std::vector<HudVertex>& out, float x, float y, const std::string& s, const std::array<float, 4>& c,
          float scale) {
  const float gw = font::kCellW * scale, gh = font::kCellH * scale;
  const float du = float(font::kCellW) / kAtlasW, dv = float(font::kCellH) / kAtlasH;
  float cx = x;
  for (const char ch : s) {
    if (ch == '\n') { y += gh; cx = x; continue; }
    const int g = int(static_cast<unsigned char>(ch)) - font::kFirst;
    if (g > 0 && g < font::kCount) {
      const float u0 = float((g % 16) * font::kCellW) / kAtlasW, v0 = float((g / 16) * font::kCellH) / kAtlasH;
      quad(out, cx, y, gw, gh, u0, v0, u0 + du, v0 + dv, c);
    }
    cx += gw;
  }
}

/// Quad clipped to `c` (u, v interpolated); nothing if fully outside.
void clippedQuad(std::vector<HudVertex>& out, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                 const std::array<float, 4>& col, const HudClip& c) {
  const float x0 = std::max(x, c.x0), y0 = std::max(y, c.y0), x1 = std::min(x + w, c.x1), y1 = std::min(y + h, c.y1);
  if (x1 <= x0 || y1 <= y0 || w <= 0 || h <= 0) return;
  const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
  const float tu0 = (x0 - x) / w, tu1 = (x1 - x) / w, tv0 = (y0 - y) / h, tv1 = (y1 - y) / h;
  quad(out, x0, y0, x1 - x0, y1 - y0, lerp(u0, u1, tu0), lerp(v0, v1, tv0), lerp(u0, u1, tu1), lerp(v0, v1, tv1), col);
}

void clippedText(std::vector<HudVertex>& out, const HudDraw& d) {
  const float gw = font::kCellW * d.scale, gh = font::kCellH * d.scale;
  const float du = float(font::kCellW) / kAtlasW, dv = float(font::kCellH) / kAtlasH;
  if (d.y + gh <= d.clip.y0 || d.y >= d.clip.y1) return;
  float cx = d.x;
  for (const char ch : d.text) {
    if (cx >= d.clip.x1) break;
    const int g = int(static_cast<unsigned char>(ch)) - font::kFirst;
    if (g > 0 && g < font::kCount && cx + gw > d.clip.x0) {
      const float u0 = float((g % 16) * font::kCellW) / kAtlasW, v0 = float((g / 16) * font::kCellH) / kAtlasH;
      clippedQuad(out, cx, d.y, gw, gh, u0, v0, u0 + du, v0 + dv, {d.r, d.g, d.b, d.a}, d.clip);
    }
    cx += gw;
  }
}

}  // namespace

HudFontAtlas buildHudFontAtlas() {
  HudFontAtlas a;
  a.width = kAtlasW;
  a.height = kAtlasH;
  a.pixels.assign(size_t(kAtlasW) * kAtlasH, 0);
  for (int g = 0; g < font::kCount; ++g) {
    const int ox = (g % 16) * font::kCellW, oy = (g / 16) * font::kCellH;
    const char* s = font::kGlyphs[g];
    for (int y = 0; y < font::kCellH; ++y)
      for (int x = 0; x < font::kCellW; ++x) {
        const char ch = s[y * font::kCellW + x];
        const int v = ch <= '9' ? ch - '0' : ch - 'a' + 10;
        a.pixels[size_t(oy + y) * kAtlasW + size_t(ox + x)] = uint8_t(v * 17);
      }
  }
  return a;
}

void buildHudVertices(const HudLayer& hud, std::vector<HudVertex>& out) {
  out.clear();
  for (const HudRect& r : hud.rects) quad(out, r.x, r.y, r.w, r.h, -1, -1, -1, -1, {r.r, r.g, r.b, r.a});
  const std::array<float, 4> shadow{0, 0, 0, 0.75f};
  for (const HudText& t : hud.texts) {
    text(out, t.x + hud.scale, t.y + hud.scale, t.text, shadow, hud.scale);  // drop shadow for contrast
    text(out, t.x, t.y, t.text, {t.r, t.g, t.b, t.a}, hud.scale);
  }
  for (const HudDraw& d : hud.draws) {
    if (d.text.empty()) clippedQuad(out, d.x, d.y, d.w, d.h, -1, -1, -1, -1, {d.r, d.g, d.b, d.a}, d.clip);
    else clippedText(out, d);
  }
}

std::array<Vec3, 24> blockOutlineLines(int x, int y, int z) {
  const float e = 0.004f;
  const float x0 = float(x) - e, y0 = float(y) - e, z0 = float(z) - e;
  const float x1 = float(x) + 1 + e, y1 = float(y) + 1 + e, z1 = float(z) + 1 + e;
  return {{{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x1, y0, z1}, {x0, y0, z1}, {x0, y0, z1}, {x0, y0, z0},
           {x0, y1, z0}, {x1, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y1, z1}, {x0, y1, z1}, {x0, y1, z1}, {x0, y1, z0},
           {x0, y0, z0}, {x0, y1, z0}, {x1, y0, z0}, {x1, y1, z0}, {x1, y0, z1}, {x1, y1, z1}, {x0, y0, z1}, {x0, y1, z1}}};
}

std::array<Vec3, 4> crosshairLines(int width, int height) {
  const float s = 0.025f, aspect = height > 0 ? float(width) / float(height) : 1.f;
  return {{{-s / aspect, 0, 0}, {s / aspect, 0, 0}, {0, -s, 0}, {0, s, 0}}};
}

}  // namespace litholattice
