#include "litholattice/ui/containers.h"
#include <algorithm>
#include <cmath>

namespace litholattice::ui {
namespace {

float axis(Vec2 v, Orientation o) { return o == Orientation::Vertical ? v.y : v.x; }
float cross(Vec2 v, Orientation o) { return o == Orientation::Vertical ? v.x : v.y; }

}  // namespace

// ---- StackPanel

StackPanel::StackPanel(Orientation o, float spacing, std::string styleClass)
    : Widget(std::move(styleClass)), orientation_(o), spacing_(spacing) {}

Vec2 StackPanel::measure(const Theme& theme) const {
  float along = 0, across = 0;
  int n = 0;
  for (const auto& c : children()) {
    if (!c->visible()) continue;
    const Vec2 s = childSize(*c, theme);
    const Edges& m = c->layout().margin;
    const bool v = orientation_ == Orientation::Vertical;
    along += axis(s, orientation_) + (v ? m.vertical() : m.horizontal());
    across = std::max(across, cross(s, orientation_) + (v ? m.horizontal() : m.vertical()));
    ++n;
  }
  along += spacing_ * float(std::max(0, n - 1));
  const Edges& pad = theme.style(styleClass()).padding;
  return orientation_ == Orientation::Vertical ? Vec2{across + pad.horizontal(), along + pad.vertical()}
                                               : Vec2{along + pad.horizontal(), across + pad.vertical()};
}

void StackPanel::arrange(const Theme& theme) {
  const Rect area = contentRect();
  const bool v = orientation_ == Orientation::Vertical;
  float fixed = 0, flex = 0;
  int n = 0;
  for (const auto& c : children()) {
    if (!c->visible()) continue;
    const Edges& m = c->layout().margin;
    fixed += axis(childSize(*c, theme), orientation_) + (v ? m.vertical() : m.horizontal());
    flex += c->layout().flex;
    ++n;
  }
  fixed += spacing_ * float(std::max(0, n - 1));
  const float leftover = std::max(0.f, (v ? area.h : area.w) - fixed);
  float pos = v ? area.y : area.x;
  for (const auto& c : children()) {
    if (!c->visible()) continue;
    const Layout& l = c->layout();
    const Vec2 s = childSize(*c, theme);
    const float len = axis(s, orientation_) + (flex > 0 ? leftover * l.flex / flex : 0.f);
    Rect r;
    if (v) {
      const float w = stretch_ ? area.w - l.margin.horizontal() : std::min(s.x, area.w);
      r = {area.x + l.margin.left, pos + l.margin.top, std::max(0.f, w), len};
      pos += len + l.margin.vertical() + spacing_;
    } else {
      const float h = stretch_ ? area.h - l.margin.vertical() : std::min(s.y, area.h);
      r = {pos + l.margin.left, area.y + l.margin.top, len, std::max(0.f, h)};
      pos += len + l.margin.horizontal() + spacing_;
    }
    c->place(r, theme);
  }
}

// ---- GridPanel

GridPanel::GridPanel(int columns, Vec2 cell, float spacing, std::string styleClass)
    : Widget(std::move(styleClass)), columns_(std::max(1, columns)), cell_(cell), spacing_(spacing) {}

Vec2 GridPanel::measure(const Theme& theme) const {
  const int n = int(std::count_if(children().begin(), children().end(), [](const auto& c) { return c->visible(); }));
  const int cols = std::min(columns_, std::max(1, n)), rows = (n + columns_ - 1) / columns_;
  const Edges& pad = theme.style(styleClass()).padding;
  return {float(cols) * cell_.x + float(std::max(0, cols - 1)) * spacing_ + pad.horizontal(),
          float(rows) * cell_.y + float(std::max(0, rows - 1)) * spacing_ + pad.vertical()};
}

void GridPanel::arrange(const Theme& theme) {
  const Rect area = contentRect();
  int i = 0;
  for (const auto& c : children()) {
    if (!c->visible()) continue;
    const int col = i % columns_, row = i / columns_;
    c->place({area.x + float(col) * (cell_.x + spacing_), area.y + float(row) * (cell_.y + spacing_), cell_.x, cell_.y}, theme);
    ++i;
  }
}

// ---- ScrollBar

ScrollBar::ScrollBar(Orientation o) : Widget("scrollbar"), orientation_(o) {}

void ScrollBar::setRange(float contentSize, float viewSize) {
  content_ = std::max(0.f, contentSize);
  view_ = std::max(0.f, viewSize);
  setOffset(offset_);
}

void ScrollBar::setOffset(float o, bool notify) {
  o = std::clamp(o, 0.f, maxOffset());
  if (o == offset_) return;
  offset_ = o;
  if (notify) emit("change");
}

Rect ScrollBar::thumb() const {
  const Rect& r = rect();
  const bool v = orientation_ == Orientation::Vertical;
  const float track = v ? r.h : r.w;
  const float len = content_ > 0 ? std::max(16.f, track * std::min(1.f, view_ / content_)) : track;
  const float pos = maxOffset() > 0 ? (track - len) * offset_ / maxOffset() : 0.f;
  return v ? Rect{r.x, r.y + pos, r.w, len} : Rect{r.x + pos, r.y, len, r.h};
}

bool ScrollBar::onPointerDown(const UiEvent& e) {
  if (e.button != PointerButton::Left || !needed()) return false;
  const Rect t = thumb();
  if (t.contains(e.pos)) {
    grab_ = along(e.pos) - along(t.pos());
  } else {  // page towards the click
    setOffset(offset_ + (along(e.pos) < along(t.pos()) ? -view_ : view_), true);
    grab_ = -1;
  }
  return true;
}

void ScrollBar::onPointerMove(const UiEvent& e) {
  if (!pressed() || grab_ < 0) return;
  const bool v = orientation_ == Orientation::Vertical;
  const float track = v ? rect().h : rect().w, len = v ? thumb().h : thumb().w;
  const float start = along(e.pos) - grab_ - (v ? rect().y : rect().x);
  if (track > len) setOffset(start / (track - len) * maxOffset(), true);
}

bool ScrollBar::onWheel(const UiEvent& e) {
  setOffset(offset_ - e.wheel * 48.f, true);
  return true;
}

void ScrollBar::paint(Painter& p) const {
  if (!needed()) return;
  const Style& s = style();
  p.fill(rect(), s.background);
  p.fill(thumb().inset(Edges::all(1)), pressed() || hovered() ? s.pressed : s.hover);
}

// ---- ScrollView

ScrollView::ScrollView() : Widget("scrollview") {
  bar_ = &add<ScrollBar>(Orientation::Vertical);
  bar_->on("change", [this](Widget&) { relayout(); });
}

Widget& ScrollView::setContent(std::unique_ptr<Widget> content) {
  if (content_) removeChild(*content_);
  content_ = &addChild(std::move(content));
  // Keep the scroll bar on top (painted and hit-tested last).
  auto bar = removeChild(*bar_);
  bar_ = static_cast<ScrollBar*>(&addChild(std::move(bar)));
  return *content_;
}

Rect ScrollView::viewRect() const {
  const Rect c = contentRect();
  return bar_->needed() ? Rect{c.x, c.y, std::max(0.f, c.w - ScrollBar::kThickness), c.h} : c;
}

Vec2 ScrollView::measure(const Theme& theme) const {
  const Edges& pad = theme.style(styleClass()).padding;
  const Vec2 s = content_ ? childSize(*content_, theme) : Vec2{};
  return {s.x + ScrollBar::kThickness + pad.horizontal(), std::min(s.y, 240.f) + pad.vertical()};
}

void ScrollView::arrange(const Theme& theme) {
  const Rect c = contentRect();
  const float contentH = content_ ? childSize(*content_, theme).y : 0.f;
  bar_->setRange(contentH, c.h);
  const Rect view = viewRect();
  if (content_) content_->place({view.x, view.y - bar_->offset(), view.w, std::max(contentH, view.h)}, theme);
  bar_->place({c.right() - ScrollBar::kThickness, c.y, ScrollBar::kThickness, c.h}, theme);
}

void ScrollView::ensureVisible(const Rect& r) {
  const Rect view = viewRect();
  if (r.y < view.y) bar_->setOffset(bar_->offset() - (view.y - r.y));
  else if (r.bottom() > view.bottom()) bar_->setOffset(bar_->offset() + (r.bottom() - view.bottom()));
  relayout();
}

bool ScrollView::onWheel(const UiEvent& e) {
  if (!bar_->needed()) return false;
  bar_->setOffset(bar_->offset() - e.wheel * 48.f, true);
  return true;
}

Widget* ScrollView::hitTest(Vec2 p) {
  if (!visible() || !rect().contains(p)) return nullptr;
  if (bar_->needed())
    if (Widget* w = bar_->hitTest(p)) return w;
  if (content_ && viewRect().contains(p))
    if (Widget* w = content_->hitTest(p)) return w;
  return enabled() ? this : nullptr;
}

void ScrollView::paintChildren(Painter& p) const {
  if (content_) {
    Painter::ClipScope clip(p, viewRect());
    content_->paintTree(p);
  }
  bar_->paintTree(p);
}

// ---- Splitter

Splitter::Splitter(Orientation o, float ratio) : Widget("splitter"), orientation_(o), ratio_(std::clamp(ratio, 0.05f, 0.95f)) {}

Widget& Splitter::setPane(int index, std::unique_ptr<Widget> pane) {
  const size_t i = index ? 1 : 0;
  if (panes_[i]) removeChild(*panes_[i]);
  panes_[i] = &addChild(std::move(pane));
  return *panes_[i];
}

void Splitter::setRatio(float r) { ratio_ = std::clamp(r, 0.05f, 0.95f); }

Rect Splitter::handleRect() const {
  const Rect c = contentRect();
  return orientation_ == Orientation::Horizontal
             ? Rect{c.x + (c.w - kHandle) * ratio_, c.y, kHandle, c.h}
             : Rect{c.x, c.y + (c.h - kHandle) * ratio_, c.w, kHandle};
}

void Splitter::arrange(const Theme& theme) {
  const Rect c = contentRect(), h = handleRect();
  const bool horiz = orientation_ == Orientation::Horizontal;
  const Rect a = horiz ? Rect{c.x, c.y, h.x - c.x, c.h} : Rect{c.x, c.y, c.w, h.y - c.y};
  const Rect b = horiz ? Rect{h.right(), c.y, c.right() - h.right(), c.h} : Rect{c.x, h.bottom(), c.w, c.bottom() - h.bottom()};
  if (panes_[0]) panes_[0]->place(a, theme);
  if (panes_[1]) panes_[1]->place(b, theme);
}

Widget* Splitter::hitTest(Vec2 p) {
  if (visible() && enabled() && handleRect().contains(p)) return this;
  return Widget::hitTest(p);
}

bool Splitter::onPointerDown(const UiEvent& e) { return e.button == PointerButton::Left && handleRect().contains(e.pos); }

void Splitter::onPointerMove(const UiEvent& e) {
  if (!pressed()) return;
  const Rect c = contentRect();
  const bool horiz = orientation_ == Orientation::Horizontal;
  const float span = (horiz ? c.w : c.h) - kHandle;
  if (span > 0) setRatio(((horiz ? e.pos.x - c.x : e.pos.y - c.y) - kHandle * 0.5f) / span);
  relayout();
}

void Splitter::paint(Painter& p) const {
  const Style& s = style();
  p.fill(handleRect(), pressed() || hovered() ? s.pressed : s.hover);
}

// ---- TabView

TabView::TabView() : Widget("tabs") { setFocusable(true); }

Widget& TabView::addTab(std::string title, std::unique_ptr<Widget> page) {
  titles_.push_back(std::move(title));
  pages_.push_back(&addChild(std::move(page)));
  if (current_ < 0) setCurrent(0);
  else pages_.back()->setVisible(false);
  return *pages_.back();
}

void TabView::setCurrent(int index, bool notify) {
  if (index < 0 || index >= count() || index == current_) return;
  current_ = index;
  for (size_t i = 0; i < pages_.size(); ++i) pages_[i]->setVisible(int(i) == index);
  relayout();
  if (notify) emit("change");
}

float TabView::headerHeight() const {
  return theme().glyphSize(style().textScale).y + 8;
}

Rect TabView::tabRect(int i) const {
  const Theme& t = theme();
  float x = rect().x;
  for (int k = 0; k < i; ++k) x += t.textWidth(titles_[size_t(k)], style().textScale) + 16;
  return {x, rect().y, t.textWidth(titles_[size_t(i)], style().textScale) + 16, headerHeight()};
}

Vec2 TabView::measure(const Theme& theme) const {
  Vec2 s;
  for (const Widget* p : pages_) {
    const Vec2 c = childSize(*p, theme);
    s = {std::max(s.x, c.x), std::max(s.y, c.y)};
  }
  return {s.x, s.y + theme.glyphSize(style().textScale).y + 8};
}

void TabView::arrange(const Theme& theme) {
  const Rect body{rect().x, rect().y + headerHeight(), rect().w, std::max(0.f, rect().h - headerHeight())};
  for (Widget* p : pages_)
    if (p->visible()) p->place(body, theme);
}

bool TabView::onPointerDown(const UiEvent& e) {
  for (int i = 0; i < count(); ++i)
    if (tabRect(i).contains(e.pos)) {
      setCurrent(i, true);
      return true;
    }
  return false;
}

bool TabView::onKey(const UiEvent& e) {
  if (e.key == Key::Left && current_ > 0) setCurrent(current_ - 1, true);
  else if (e.key == Key::Right && current_ + 1 < count()) setCurrent(current_ + 1, true);
  else return false;
  return true;
}

void TabView::paint(Painter& p) const {
  const Style& s = style();
  p.fill(rect(), s.background);
  for (int i = 0; i < count(); ++i) {
    const Rect t = tabRect(i);
    p.fill(t.inset({0, 0, 2, 0}), i == current_ ? s.pressed : s.hover);
    if (i == current_) p.fill({t.x, t.bottom() - 2, t.w - 2, 2}, s.accent);
    p.textIn(t, titles_[size_t(i)], s.text, true, s.textScale);
  }
}

}  // namespace litholattice::ui
