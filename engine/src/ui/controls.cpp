#include "litholattice/ui/controls.h"
#include <algorithm>
#include <cmath>
#include <string_view>

namespace litholattice::ui {
namespace {

bool isActivateKey(const UiEvent& e) { return e.key == Key::Enter || e.key == Key::Space; }

/// Line count and widest line of '\n'-separated text.
Vec2 textBlock(std::string_view text) {
  float lines = 1, widest = 0, cur = 0;
  for (const char c : text) {
    if (c == '\n') {
      ++lines;
      cur = 0;
    } else {
      widest = std::max(widest, ++cur);
    }
  }
  return {widest, text.empty() ? 0.f : lines};
}

}  // namespace

// ---- Label

Label::Label(std::string text, std::string styleClass) : Widget(std::move(styleClass)), text_(std::move(text)) {}

Vec2 Label::measure(const Theme& theme) const {
  const Style& s = theme.style(styleClass());
  const Vec2 g = theme.glyphSize(s.textScale), b = textBlock(text_);
  return {b.x * g.x + s.padding.horizontal(), b.y * g.y + s.padding.vertical()};
}

Widget* Label::hitTest(Vec2 p) { return tooltip().empty() ? nullptr : Widget::hitTest(p); }

void Label::paint(Painter& p) const {
  Widget::paint(p);
  const Style& s = style();
  const Rect c = contentRect();
  const Vec2 g = p.theme().glyphSize(s.textScale);
  const Color col = hasColor_ ? color_ : (interactive() ? s.text : s.textDisabled);
  float y = c.y;
  size_t start = 0;
  while (start <= text_.size()) {
    const size_t end = std::min(text_.find('\n', start), text_.size());
    const std::string_view line(text_.data() + start, end - start);
    const float x = centered_ ? c.x + (c.w - float(line.size()) * g.x) * 0.5f : c.x;
    p.text({x, y}, line, col, s.textScale);
    y += g.y;
    start = end + 1;
  }
}

// ---- Button

Button::Button(std::string text, std::string styleClass) : Widget(std::move(styleClass)), text_(std::move(text)) {
  setFocusable(true);
}

Vec2 Button::measure(const Theme& theme) const {
  const Style& s = theme.style(styleClass());
  const Vec2 g = theme.glyphSize(s.textScale);
  return {float(text_.size()) * g.x + s.padding.horizontal() + 8, g.y + s.padding.vertical()};
}

bool Button::onPointerDown(const UiEvent& e) { return e.button == PointerButton::Left; }

void Button::onPointerUp(const UiEvent& e) {
  if (rect().contains(e.pos)) emit("click");
}

bool Button::onKey(const UiEvent& e) {
  if (!isActivateKey(e)) return false;
  emit("click");
  return true;
}

void Button::paint(Painter& p) const {
  const Style& s = style();
  p.fill(rect(), stateBackground());
  p.frame(rect(), s.border, s.borderWidth);
  p.textIn(contentRect(), text_, interactive() ? s.text : s.textDisabled, true, s.textScale);
}

// ---- Image

Image::Image(std::string image, Vec2 size) : Widget("image"), image_(std::move(image)), size_(size) {}

void Image::paint(Painter& p) const {
  if (!image_.empty()) p.image(rect(), image_, tint_);
}

// ---- Checkbox

Checkbox::Checkbox(std::string text, bool checked, bool toggle)
    : Widget("checkbox"), text_(std::move(text)), checked_(checked), toggle_(toggle) {
  setFocusable(true);
}

void Checkbox::setChecked(bool c, bool notify) {
  if (c == checked_) return;
  checked_ = c;
  if (notify) emit("change");
}

Vec2 Checkbox::measure(const Theme& theme) const {
  const Style& s = theme.style(styleClass());
  const Vec2 g = theme.glyphSize(s.textScale);
  const float box = toggle_ ? g.y * 2 : g.y;
  return {box + g.x + float(text_.size()) * g.x + s.padding.horizontal(), g.y + s.padding.vertical()};
}

bool Checkbox::onPointerDown(const UiEvent& e) { return e.button == PointerButton::Left; }

void Checkbox::onPointerUp(const UiEvent& e) {
  if (rect().contains(e.pos)) setChecked(!checked_, true);
}

bool Checkbox::onKey(const UiEvent& e) {
  if (!isActivateKey(e)) return false;
  setChecked(!checked_, true);
  return true;
}

void Checkbox::paint(Painter& p) const {
  const Style& s = style();
  const Rect c = contentRect();
  const Vec2 g = p.theme().glyphSize(s.textScale);
  const float side = g.y;
  const Rect box{c.x, c.y + (c.h - side) * 0.5f, toggle_ ? side * 2 : side, side};
  p.fill(box, stateBackground());
  p.frame(box, s.border, s.borderWidth);
  if (toggle_) {
    const Rect knob{checked_ ? box.right() - side : box.x, box.y, side, side};
    if (checked_) p.fill(box.inset(Edges::all(2)), s.accent.withAlpha(0.5f));
    p.fill(knob.inset(Edges::all(2)), checked_ ? s.accent : s.text);
  } else if (checked_) {
    p.fill(box.inset(Edges::all(side * 0.25f)), s.accent);
  }
  p.textIn({box.right() + g.x, c.y, c.w, c.h}, text_, interactive() ? s.text : s.textDisabled, false, s.textScale);
}

// ---- ProgressBar

ProgressBar& ProgressBar::setValue(float v) {
  value_ = std::clamp(v, 0.f, 1.f);
  return *this;
}

Vec2 ProgressBar::measure(const Theme& theme) const {
  const Style& s = theme.style(styleClass());
  return {120, theme.glyphSize(s.textScale).y + s.padding.vertical()};
}

void ProgressBar::paint(Painter& p) const {
  const Style& s = style();
  p.fill(rect(), s.background);
  const Rect c = rect().inset(Edges::all(s.borderWidth + 1));
  p.fill({c.x, c.y, c.w * value_, c.h}, s.accent);
  p.frame(rect(), s.border, s.borderWidth);
  p.textIn(rect(), text_, s.text, true, s.textScale);
}

// ---- Slider

Slider::Slider(float min, float max, float value, float step)
    : Widget("slider"), min_(min), max_(std::max(min, max)), value_(min), step_(step) {
  setFocusable(true);
  setValue(value);
}

void Slider::setValue(float v, bool notify) {
  v = std::clamp(v, min_, max_);
  if (step_ > 0) v = std::clamp(min_ + std::round((v - min_) / step_) * step_, min_, max_);
  if (v == value_) return;
  value_ = v;
  if (notify) emit("change");
}

Vec2 Slider::measure(const Theme& theme) const {
  const Style& s = theme.style(styleClass());
  return {140, theme.glyphSize(s.textScale).y + s.padding.vertical()};
}

void Slider::setFromX(float x) {
  const Rect c = contentRect();
  const float t = c.w > 0 ? std::clamp((x - c.x) / c.w, 0.f, 1.f) : 0.f;
  setValue(min_ + t * (max_ - min_), true);
}

bool Slider::onPointerDown(const UiEvent& e) {
  if (e.button != PointerButton::Left) return false;
  setFromX(e.pos.x);
  return true;
}

void Slider::onPointerMove(const UiEvent& e) {
  if (pressed()) setFromX(e.pos.x);
}

bool Slider::onWheel(const UiEvent& e) {
  const float unit = step_ > 0 ? step_ : (max_ - min_) / 20.f;
  setValue(value_ + (e.wheel > 0 ? unit : -unit), true);
  return true;
}

bool Slider::onKey(const UiEvent& e) {
  const float unit = step_ > 0 ? step_ : (max_ - min_) / 20.f;
  if (e.key == Key::Left || e.key == Key::Down) setValue(value_ - unit, true);
  else if (e.key == Key::Right || e.key == Key::Up) setValue(value_ + unit, true);
  else if (e.key == Key::Home) setValue(min_, true);
  else if (e.key == Key::End) setValue(max_, true);
  else return false;
  return true;
}

void Slider::paint(Painter& p) const {
  const Style& s = style();
  const Rect c = contentRect();
  const float t = max_ > min_ ? (value_ - min_) / (max_ - min_) : 0.f;
  const Rect track{c.x, c.y + c.h * 0.5f - 2, c.w, 4};
  p.fill(track, s.background);
  p.fill({track.x, track.y, track.w * t, track.h}, s.accent);
  const float knob = c.h;
  p.fill({c.x + t * c.w - knob * 0.25f, c.y, knob * 0.5f, knob}, pressed() || hovered() ? s.pressed : s.text);
}

// ---- NumberField

NumberField::NumberField(int min, int max, int value) : Widget("textbox"), min_(min), max_(std::max(min, max)), value_(min) {
  setFocusable(true);
  setValue(value);
}

void NumberField::setValue(int v, bool notify) {
  v = std::clamp(v, min_, max_);
  if (v == value_) return;
  value_ = v;
  if (notify) emit("change");
}

Vec2 NumberField::measure(const Theme& theme) const {
  const Style& s = theme.style(styleClass());
  const Vec2 g = theme.glyphSize(s.textScale);
  const int digits = int(std::max(std::to_string(min_).size(), std::to_string(max_).size()));
  return {float(digits + 1) * g.x + 2 * (g.y + 4) + s.padding.horizontal(), g.y + s.padding.vertical()};
}

Rect NumberField::minusRect() const { return {rect().x, rect().y, rect().h, rect().h}; }
Rect NumberField::plusRect() const { return {rect().right() - rect().h, rect().y, rect().h, rect().h}; }

void NumberField::commitTyping() {
  if (typing_.empty()) return;
  const int v = std::stoi(typing_);
  typing_.clear();
  setValue(v, true);
}

bool NumberField::onPointerDown(const UiEvent& e) {
  if (e.button != PointerButton::Left) return false;
  commitTyping();
  if (minusRect().contains(e.pos)) setValue(value_ - 1, true);
  else if (plusRect().contains(e.pos)) setValue(value_ + 1, true);
  return true;
}

bool NumberField::onWheel(const UiEvent& e) {
  commitTyping();
  setValue(value_ + (e.wheel > 0 ? 1 : -1), true);
  return true;
}

bool NumberField::onKey(const UiEvent& e) {
  const int step = e.shift() ? 8 : 1;
  switch (e.key) {
    case Key::Up: commitTyping(); setValue(value_ + step, true); return true;
    case Key::Down: commitTyping(); setValue(value_ - step, true); return true;
    case Key::PageUp: commitTyping(); setValue(max_, true); return true;
    case Key::PageDown: commitTyping(); setValue(min_, true); return true;
    case Key::Enter: commitTyping(); emit("submit"); return true;
    case Key::Backspace:
      if (!typing_.empty()) typing_.pop_back();
      return true;
    case Key::Escape:
      if (typing_.empty()) return false;
      typing_.clear();
      return true;
    default: return false;
  }
}

bool NumberField::onText(char32_t c) {
  if (c == U'-' && typing_.empty() && min_ < 0) {
    typing_ = "-";
    return true;
  }
  if (c < U'0' || c > U'9' || typing_.size() >= 9) return c >= U' ';  // swallow other printable keys
  typing_ += char(c);
  if (typing_ != "-" && std::stoll(typing_) > max_) typing_ = std::to_string(max_);  // can't type past max
  return true;
}

void NumberField::onFocusChanged(bool focused) {
  if (!focused) commitTyping();
}

void NumberField::paint(Painter& p) const {
  const Style& s = style();
  p.fill(rect(), s.background);
  p.frame(rect(), focused() ? s.accent : s.border, s.borderWidth);
  const Rect m = minusRect(), pl = plusRect();
  p.fill(m.inset(Edges::all(2)), s.hover);
  p.fill(pl.inset(Edges::all(2)), s.hover);
  p.textIn(m, "-", s.text, true, s.textScale);
  p.textIn(pl, "+", s.text, true, s.textScale);
  const std::string shown = typing_.empty() ? std::to_string(value_) : typing_ + "_";
  p.textIn({m.right(), rect().y, pl.x - m.right(), rect().h}, shown, typing_.empty() ? s.text : s.accent, true, s.textScale);
}

// ---- Slot

Slot::Slot(Vec2 size) : Widget("slot"), size_(size) { setFocusable(true); }

bool Slot::onPointerDown(const UiEvent& e) {
  if (e.button == PointerButton::Left) emit("click");
  else if (e.button == PointerButton::Right) emit("rightclick");
  else return false;
  return true;
}

bool Slot::onKey(const UiEvent& e) {
  if (!isActivateKey(e)) return false;
  emit("click");
  return true;
}

void Slot::paint(Painter& p) const {
  const Style& s = style();
  const float pad = std::max(2.f, size_.x * 0.15f);
  if (selected_) p.fill(rect().inset(Edges::all(-3)), s.accent);
  p.fill(rect(), hovered() ? s.hover : s.background);
  if (!icon_.empty()) p.image(rect().inset(Edges::all(pad)), icon_, tint_);
  if (!caption_.empty()) p.text({rect().x + 2, rect().y + 1}, caption_, s.text, s.textScale);
}

Divider::Divider(Orientation o, float thickness) : Widget("divider"), orientation_(o), thickness_(std::max(1.f, thickness)) {}

Vec2 Divider::measure(const Theme& theme) const {
  (void)theme;
  return orientation_ == Orientation::Horizontal ? Vec2{0, thickness_} : Vec2{thickness_, 0};
}

void Divider::paint(Painter& p) const {
  const Style& s = style();
  const Rect r = rect();
  if (orientation_ == Orientation::Horizontal) {
    const float y = r.y + (r.h - thickness_) * 0.5f;
    p.fill({r.x, y, r.w, thickness_}, s.accent);
  } else {
    const float x = r.x + (r.w - thickness_) * 0.5f;
    p.fill({x, r.y, thickness_, r.h}, s.accent);
  }
}

}  // namespace litholattice::ui
