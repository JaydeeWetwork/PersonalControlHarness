// UI value types: points, rectangles, colours and edge insets, in framebuffer pixels (origin
// top-left, y down).
#pragma once
#include <algorithm>

namespace litholattice::ui {

struct Vec2 {
  float x = 0, y = 0;
  friend Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
  friend Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
  friend bool operator==(Vec2, Vec2) = default;
};

/// Insets on each side (padding, margins, borders).
struct Edges {
  float left = 0, top = 0, right = 0, bottom = 0;
  static constexpr Edges all(float v) { return {v, v, v, v}; }
  static constexpr Edges symmetric(float h, float v) { return {h, v, h, v}; }
  constexpr float horizontal() const { return left + right; }
  constexpr float vertical() const { return top + bottom; }
  friend bool operator==(const Edges&, const Edges&) = default;
};

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  constexpr float right() const { return x + w; }
  constexpr float bottom() const { return y + h; }
  constexpr Vec2 pos() const { return {x, y}; }
  constexpr Vec2 size() const { return {w, h}; }
  constexpr bool empty() const { return w <= 0 || h <= 0; }
  constexpr bool contains(Vec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
  /// This rectangle shrunk by `e` (never below zero size).
  constexpr Rect inset(const Edges& e) const {
    return {x + e.left, y + e.top, std::max(0.f, w - e.horizontal()), std::max(0.f, h - e.vertical())};
  }
  constexpr Rect intersect(const Rect& o) const {
    const float x0 = std::max(x, o.x), y0 = std::max(y, o.y);
    const float x1 = std::min(right(), o.right()), y1 = std::min(bottom(), o.bottom());
    return {x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0)};
  }
  friend bool operator==(const Rect&, const Rect&) = default;
};

/// Straight-alpha RGBA, 0..1.
struct Color {
  float r = 1, g = 1, b = 1, a = 1;
  constexpr Color withAlpha(float alpha) const { return {r, g, b, alpha}; }
  friend bool operator==(const Color&, const Color&) = default;
};

}  // namespace litholattice::ui
