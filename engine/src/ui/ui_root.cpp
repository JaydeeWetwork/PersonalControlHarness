#include "litholattice/ui/ui_root.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include "litholattice/ui/dialog.h"

namespace litholattice::ui {
namespace {

constexpr float kTooltipDelay = 0.6f;

bool isInside(const Widget* w, const Widget* ancestor) {
  for (; w; w = w->parent())
    if (w == ancestor) return true;
  return false;
}

}  // namespace

UiRoot::UiRoot(Theme theme)
    : theme_(std::move(theme)), root_(std::make_unique<Widget>("root")), tooltip_(std::make_unique<Tooltip>()) {
  root_->attach(this);
  tooltip_->attach(this);
}

UiRoot::~UiRoot() {
  // Destroy layers before the pointers they would report to.
  popups_.clear();
  modals_.clear();
  closing_.clear();
  root_.reset();
  tooltip_.reset();
}

void UiRoot::forget(const Widget* w) {
  if (isInside(focus_, w)) focus_ = nullptr;
  if (isInside(hover_, w)) hover_ = nullptr;
  if (isInside(capture_, w)) capture_ = nullptr;
}

void UiRoot::layout(Vec2 viewport) {
  viewport_ = viewport;
  const Rect screen{0, 0, viewport.x, viewport.y};
  root_->place(screen, theme_);
  // Layers are anchored to the screen like children of the root, then kept on screen.
  for (auto* list : {&modals_, &popups_})
    for (auto& w : *list) {
      Rect r = anchoredRect(w->layout(), screen, Widget::childSize(*w, theme_));
      r.x = std::clamp(r.x, 0.f, std::max(0.f, viewport.x - r.w));
      r.y = std::clamp(r.y, 0.f, std::max(0.f, viewport.y - r.h));
      w->place(r, theme_);
    }
}

void UiRoot::paint(IUiRenderer& renderer) const {
  Painter p(renderer, theme_);
  root_->paintTree(p);
  for (const auto& m : modals_) {
    p.fill({0, 0, viewport_.x, viewport_.y}, {0, 0, 0, 0.45f});  // dims everything below
    m->paintTree(p);
  }
  for (const auto& w : popups_) w->paintTree(p);
  if (hover_ && !hover_->tooltip().empty() && hoverTime_ >= kTooltipDelay && !capture_) {
    tooltip_->setText(hover_->tooltip());
    const Vec2 size = tooltip_->measure(theme_);
    const float x = std::clamp(pointer_.x + 12, 0.f, std::max(0.f, viewport_.x - size.x));
    const float y = pointer_.y + 20 + size.y > viewport_.y ? pointer_.y - size.y - 4 : pointer_.y + 20;
    tooltip_->place({x, y, size.x, size.y}, theme_);
    tooltip_->paintTree(p);
  }
}

void UiRoot::update(float dt) {
  closing_.clear();  // layers closed outside event handling
  time_ += dt;
  hoverTime_ += dt;
}

Widget& UiRoot::activeLayer() const {
  if (!modals_.empty()) return *modals_.back();
  return *root_;
}

Widget* UiRoot::hitTestLayers(Vec2 p) const {
  for (auto it = popups_.rbegin(); it != popups_.rend(); ++it)
    if (Widget* w = (*it)->hitTest(p)) return w;
  if (!modals_.empty()) return modals_.back()->hitTest(p);
  Widget* w = root_->hitTest(p);
  return w == root_.get() ? nullptr : w;  // the bare root is not a target
}

bool UiRoot::wantsKeyboard() const {
  if (!modals_.empty() || !popups_.empty()) return true;
  return focus_ != nullptr;
}

bool UiRoot::wantsPointer(Vec2 p) const { return !modals_.empty() || hitTestLayers(p) != nullptr; }

void UiRoot::setFocus(Widget* w) {
  if (w == focus_) return;
  Widget* old = focus_;
  focus_ = w;
  if (old) old->onFocusChanged(false);
  if (w) w->onFocusChanged(true);
}

void UiRoot::collectFocusable(Widget& w, std::vector<Widget*>& out) const {
  if (!w.visible()) return;
  if (w.focusable() && w.interactive()) out.push_back(&w);
  for (const auto& c : w.children()) collectFocusable(*c, out);
}

void UiRoot::focusNext(bool backwards) {
  std::vector<Widget*> list;
  collectFocusable(popups_.empty() ? activeLayer() : *popups_.back(), list);
  if (list.empty()) return;
  const auto it = std::find(list.begin(), list.end(), focus_);
  size_t i = 0;
  if (it == list.end()) i = backwards ? list.size() - 1 : 0;
  else {
    const size_t cur = size_t(it - list.begin());
    i = backwards ? (cur + list.size() - 1) % list.size() : (cur + 1) % list.size();
  }
  setFocus(list[i]);
}

bool UiRoot::focusDirection(float dx, float dy) {
  std::vector<Widget*> list;
  collectFocusable(popups_.empty() ? activeLayer() : *popups_.back(), list);
  if (!focus_) {
    if (list.empty()) return false;
    setFocus(list.front());
    return true;
  }
  const Rect& f = focus_->rect();
  const Vec2 c{f.x + f.w * 0.5f, f.y + f.h * 0.5f};
  Widget* best = nullptr;
  float bestScore = std::numeric_limits<float>::max();
  for (Widget* w : list) {
    if (w == focus_) continue;
    const Rect& r = w->rect();
    const float vx = r.x + r.w * 0.5f - c.x, vy = r.y + r.h * 0.5f - c.y;
    const float along = vx * dx + vy * dy;
    if (along <= 0.5f) continue;  // not in that direction
    const float across = std::abs(vx * dy - vy * dx);
    const float score = along + 2.f * across;  // prefer widgets straight ahead
    if (score < bestScore) {
      bestScore = score;
      best = w;
    }
  }
  if (best) setFocus(best);
  return best != nullptr;
}

Widget& UiRoot::openPopup(std::unique_ptr<Widget> popup) {
  popup->attach(this);
  popups_.push_back(std::move(popup));
  layout(viewport_);
  return *popups_.back();
}

void UiRoot::closePopup(const Widget& popup) {
  const auto it = std::find_if(popups_.begin(), popups_.end(), [&](const auto& p) { return p.get() == &popup; });
  if (it == popups_.end()) return;
  // Close it and everything opened above it (submenus).
  for (auto j = it; j != popups_.end(); ++j) {
    forget(j->get());
    closing_.push_back(std::move(*j));
  }
  popups_.erase(it, popups_.end());
}

bool UiRoot::isPopupOpen(const Widget* popup) const {
  return popup && std::any_of(popups_.begin(), popups_.end(), [&](const auto& p) { return p.get() == popup; });
}

void UiRoot::closeAllPopups() {
  if (!popups_.empty()) closePopup(*popups_.front());
}

Widget& UiRoot::pushModal(std::unique_ptr<Widget> modal) {
  closeAllPopups();
  modal->attach(this);
  modals_.push_back(std::move(modal));
  setFocus(nullptr);
  layout(viewport_);
  focusNext(false);
  return *modals_.back();
}

void UiRoot::closeModal(const Widget& modal) {
  const auto it = std::find_if(modals_.begin(), modals_.end(), [&](const auto& m) { return m.get() == &modal; });
  if (it == modals_.end()) return;
  closeAllPopups();
  for (auto j = it; j != modals_.end(); ++j) {
    forget(j->get());
    closing_.push_back(std::move(*j));
  }
  modals_.erase(it, modals_.end());
}

void UiRoot::deliverKey(const UiEvent& e) {
  for (Widget* w = focus_; w; w = w->parent())
    if (w->interactive() && w->onKey(e)) return;
  // Unhandled: navigation and layer keys.
  if (e.key == Key::Tab) {
    focusNext(e.shift());
  } else if (e.key == Key::Escape) {
    if (!popups_.empty()) {
      closePopup(*popups_.back());
    } else {
      Widget* m = topModal();
      const bool modalHandled = m && m->onKey(e);  // e.g. a dialog closes itself
      if (!modalHandled) setFocus(nullptr);
    }
  } else if (e.key == Key::Up || e.key == Key::Down || e.key == Key::Left || e.key == Key::Right) {
    const float dx = e.key == Key::Left ? -1.f : e.key == Key::Right ? 1.f : 0.f;
    const float dy = e.key == Key::Up ? -1.f : e.key == Key::Down ? 1.f : 0.f;
    focusDirection(dx, dy);
  }
}

void UiRoot::destroyLater(std::unique_ptr<Widget> w) {
  if (!w) return;
  forget(w.get());
  closing_.push_back(std::move(w));
}

bool UiRoot::handleEvent(const UiEvent& e) {
  bool consumed = false;
  mods_ = e.mods;
  switch (e.type) {
    case UiEvent::Type::PointerMove: {
      pointer_ = e.pos;
      Widget* over = hitTestLayers(e.pos);
      if (over != hover_) {
        hover_ = over;
        hoverTime_ = 0;
      }
      if (capture_) capture_->onPointerMove(e);
      else if (hover_) hover_->onPointerMove(e);
      consumed = capture_ != nullptr || hover_ != nullptr || !modals_.empty();
      break;
    }
    case UiEvent::Type::PointerDown: {
      pointer_ = e.pos;
      hoverTime_ = 0;
      Widget* target = hitTestLayers(e.pos);
      // A click outside every popup closes them (and is not passed on).
      if (!popups_.empty() && !std::any_of(popups_.begin(), popups_.end(), [&](const auto& p) { return isInside(target, p.get()); })) {
        closeAllPopups();
        consumed = true;
        if (!target) break;
      }
      setFocus(nullptr);
      for (Widget* w = target; w; w = w->parent())
        if (w->focusable() && w->interactive()) {
          setFocus(w);
          break;
        }
      for (Widget* w = target; w; w = w->parent())
        if (w->interactive() && w->onPointerDown(e)) {
          capture_ = w;
          break;
        }
      consumed = consumed || target != nullptr || !modals_.empty();
      break;
    }
    case UiEvent::Type::PointerUp: {
      pointer_ = e.pos;
      if (Widget* c = capture_) {
        capture_ = nullptr;
        c->onPointerUp(e);
        consumed = true;
      } else {
        consumed = hitTestLayers(e.pos) != nullptr || !modals_.empty();
      }
      break;
    }
    case UiEvent::Type::Wheel: {
      for (Widget* w = hitTestLayers(e.pos); w; w = w->parent())
        if (w->interactive() && w->onWheel(e)) {
          consumed = true;
          break;
        }
      consumed = consumed || hitTestLayers(e.pos) != nullptr || !modals_.empty();
      break;
    }
    case UiEvent::Type::KeyDown:
      if (!wantsKeyboard()) break;
      deliverKey(e);
      consumed = true;
      break;
    case UiEvent::Type::Text:
      if (focus_ && focus_->interactive()) consumed = focus_->onText(e.codepoint);
      consumed = consumed || !modals_.empty();
      break;
  }
  if (!closing_.empty()) {  // no widget code is running any more: release closed layers
    closing_.clear();
    layout(viewport_);
  }
  return consumed;
}

}  // namespace litholattice::ui
