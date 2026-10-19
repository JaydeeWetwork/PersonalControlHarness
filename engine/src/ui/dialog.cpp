#include "litholattice/ui/dialog.h"
#include "litholattice/ui/ui_root.h"

namespace litholattice::ui {

Dialog::Dialog(std::string title, Vec2 size) : Widget("panel"), title_(std::move(title)) {
  layout() = Layout::at({0.5f, 0.5f}, {0.5f, 0.5f}, {}, size);
  body_ = &add<StackPanel>(Orientation::Vertical, 6.f);
  buttons_ = &add<StackPanel>(Orientation::Horizontal, 6.f);
  buttons_->setStretch(false);
}

Dialog& Dialog::show(UiRoot& ui, std::string title, Vec2 size) {
  return static_cast<Dialog&>(ui.pushModal(std::make_unique<Dialog>(std::move(title), size)));
}

Button& Dialog::addButton(std::string text, std::function<void()> onClick, bool closes) {
  Button& b = buttons_->add<Button>(std::move(text));
  b.on("click", [this, onClick = std::move(onClick), closes](Widget&) {
    if (onClick) onClick();
    if (closes) close();
  });
  return b;
}

void Dialog::close() {
  emit("close");
  if (ui()) ui()->closeModal(*this);
}

float Dialog::titleHeight() const { return theme().glyphSize(style().textScale).y + 10; }

Vec2 Dialog::measure(const Theme& theme) const {
  const Edges& pad = theme.style(styleClass()).padding;
  const Vec2 b = childSize(*body_, theme), r = childSize(*buttons_, theme);
  const float title = theme.textWidth(title_) + 40;
  return {std::max({b.x, r.x, title}) + pad.horizontal(),
          theme.glyphSize().y + 10 + b.y + (buttons_->children().empty() ? 0 : r.y + 8) + pad.vertical()};
}

void Dialog::arrange(const Theme& theme) {
  const Rect c = contentRect();
  const float top = c.y + titleHeight();
  const bool hasButtons = !buttons_->children().empty();
  const float rowH = hasButtons ? childSize(*buttons_, theme).y : 0.f;
  const float bodyBottom = c.bottom() - (hasButtons ? rowH + 8 : 0.f);
  body_->place({c.x, top, c.w, std::max(0.f, bodyBottom - top)}, theme);
  const float rowW = childSize(*buttons_, theme).x;
  buttons_->place({c.right() - rowW, c.bottom() - rowH, rowW, rowH}, theme);
}

bool Dialog::onPointerDown(const UiEvent& e) {
  const Rect bar{rect().x, rect().y, rect().w, titleHeight() + style().padding.top};
  if (e.button != PointerButton::Left || !bar.contains(e.pos)) return false;
  grab_ = e.pos - rect().pos();
  return true;
}

void Dialog::onPointerMove(const UiEvent& e) {
  if (!pressed() || grab_.x < 0) return;
  // Dragged: from now on placed by absolute offset.
  const Vec2 size = rect().size();
  layout() = Layout::at({0, 0}, {0, 0}, e.pos - grab_, size);
  place({e.pos.x - grab_.x, e.pos.y - grab_.y, size.x, size.y}, theme());
}

bool Dialog::onKey(const UiEvent& e) {
  if (e.key != Key::Escape) return false;
  if (!closeGuard_ || closeGuard_()) close();
  return true;
}

void Dialog::paint(Painter& p) const {
  Widget::paint(p);
  const Style& s = style();
  const Rect bar{rect().x, rect().y, rect().w, titleHeight() + s.padding.top};
  p.fill(bar, s.accent.withAlpha(0.35f));
  p.textIn(bar.inset({s.padding.left, s.padding.top * 0.5f, 0, 0}), title_, s.text, false, s.textScale);
}

Vec2 Tooltip::measure(const Theme& theme) const {
  const Style& s = theme.style(styleClass());
  return {theme.textWidth(text_, s.textScale) + s.padding.horizontal(), theme.glyphSize(s.textScale).y + s.padding.vertical()};
}

void Tooltip::paint(Painter& p) const {
  Widget::paint(p);
  const Style& s = style();
  p.text({contentRect().x, contentRect().y}, text_, s.text, s.textScale);
}

}  // namespace litholattice::ui
