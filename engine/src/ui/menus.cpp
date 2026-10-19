#include "litholattice/ui/menus.h"
#include <algorithm>
#include <cmath>
#include "litholattice/ui/lists.h"
#include "litholattice/ui/ui_root.h"

namespace litholattice::ui {

// ---- Menu

Menu::Menu(std::vector<MenuItem> items) : Widget("menu"), items_(std::move(items)) { setFocusable(true); }

Menu& Menu::open(UiRoot& ui, std::vector<MenuItem> items, Vec2 pos) {
  auto menu = std::make_unique<Menu>(std::move(items));
  menu->setLayout(Layout::at({0, 0}, {0, 0}, pos));
  Menu& m = static_cast<Menu&>(ui.openPopup(std::move(menu)));
  ui.setFocus(&m);
  return m;
}

float Menu::rowHeight() const { return theme().glyphSize(style().textScale).y + 6; }

Vec2 Menu::measure(const Theme& theme) const {
  const Style& s = theme.style(styleClass());
  const Vec2 g = theme.glyphSize(s.textScale);
  size_t label = 4, shortcut = 0;
  float h = 0;
  for (const auto& i : items_) {
    label = std::max(label, i.label.size());
    shortcut = std::max(shortcut, i.shortcut.size());
    h += i.separator ? 6 : g.y + 6;
  }
  // check column + label + gap + shortcut / submenu arrow
  return {float(2 + label + 3 + std::max<size_t>(shortcut, 1)) * g.x + s.padding.horizontal(), h + s.padding.vertical()};
}

Rect Menu::rowRect(int i) const {
  const Rect c = contentRect();
  float y = c.y;
  for (int k = 0; k < i; ++k) y += items_[size_t(k)].separator ? 6 : rowHeight();
  return {c.x, y, c.w, items_[size_t(i)].separator ? 6 : rowHeight()};
}

int Menu::rowAt(Vec2 p) const {
  for (int i = 0; i < int(items_.size()); ++i)
    if (rowRect(i).contains(p)) return items_[size_t(i)].separator ? -1 : i;
  return -1;
}

void Menu::openSubmenu(int index) {
  if (child_ && ui() && ui()->isPopupOpen(child_)) ui()->closePopup(*child_);
  child_ = nullptr;
  if (index < 0 || items_[size_t(index)].submenu.empty() || !ui()) return;
  const Rect r = rowRect(index);
  Menu& sub = open(*ui(), items_[size_t(index)].submenu, {rect().right() - 2, r.y - style().padding.top});
  sub.parentMenu_ = this;
  child_ = &sub;
  ui()->setFocus(this);  // keep keyboard here until the user enters the submenu
}

bool Menu::activate(int index) {
  if (index < 0 || index >= int(items_.size())) return false;
  const MenuItem item = items_[size_t(index)];  // copy: closing may release this menu
  if (item.separator || !item.enabled) return false;
  if (!item.submenu.empty()) {
    openSubmenu(index);
    if (child_) {
      child_->highlight_ = 0;
      ui()->setFocus(child_);
    }
    return true;
  }
  Menu* top = this;
  while (top->parentMenu_) top = top->parentMenu_;
  const std::function<void()> done = top->onDone;
  UiRoot* root = ui();
  if (root) root->closePopup(*top);
  if (item.action) item.action();
  if (!item.script.empty() && root && root->scriptHost()) root->scriptHost()->callHandler(item.script, item.label);
  if (done) done();
  return true;
}

bool Menu::onPointerDown(const UiEvent& e) { return e.button == PointerButton::Left || e.button == PointerButton::Right; }

void Menu::onPointerUp(const UiEvent& e) {
  const int i = rowAt(e.pos);
  if (i >= 0) activate(i);
}

void Menu::onPointerMove(const UiEvent& e) {
  const int i = rowAt(e.pos);
  if (i == highlight_ || i < 0) return;
  highlight_ = i;
  openSubmenu(items_[size_t(i)].submenu.empty() ? -1 : i);
}

void Menu::step(int dir) {
  const int n = int(items_.size());
  for (int k = 1; k <= n; ++k) {
    const int i = ((highlight_ < 0 ? (dir > 0 ? -1 : 0) : highlight_) + dir * k + n * 2) % n;
    if (!items_[size_t(i)].separator) {
      highlight_ = i;
      return;
    }
  }
}

bool Menu::onKey(const UiEvent& e) {
  switch (e.key) {
    case Key::Up: step(-1); return true;
    case Key::Down: step(1); return true;
    case Key::Enter:
    case Key::Space: activate(highlight_); return true;
    case Key::Right:
      if (highlight_ >= 0 && !items_[size_t(highlight_)].submenu.empty()) {
        activate(highlight_);
        return true;
      }
      if (onNavigate) {
        onNavigate(1);
        return true;
      }
      return false;
    case Key::Left:
    case Key::Escape:
      if (parentMenu_ && ui()) {
        Menu* parent = parentMenu_;
        ui()->closePopup(*this);
        parent->child_ = nullptr;
        ui()->setFocus(parent);
        return true;
      }
      if (e.key == Key::Left && onNavigate) {
        onNavigate(-1);
        return true;
      }
      if (e.key == Key::Escape && ui()) {
        ui()->closePopup(*this);
        return true;
      }
      return false;
    default: return false;
  }
}

void Menu::paint(Painter& p) const {
  const Style& s = style();
  p.fill(rect(), s.background);
  p.frame(rect(), s.border, s.borderWidth);
  const Vec2 g = p.theme().glyphSize(s.textScale);
  for (int i = 0; i < int(items_.size()); ++i) {
    const MenuItem& it = items_[size_t(i)];
    const Rect r = rowRect(i);
    if (it.separator) {
      p.fill({r.x + 4, r.y + 2, r.w - 8, 1}, s.border.withAlpha(1.f));
      continue;
    }
    if (i == highlight_ && it.enabled) p.fill(r, s.accent.withAlpha(0.5f));
    const Color col = it.enabled ? s.text : s.textDisabled;
    if (it.checked == 1) p.textIn({r.x, r.y, 2 * g.x, r.h}, "*", s.accent, true, s.textScale);
    p.textIn({r.x + 2 * g.x, r.y, r.w, r.h}, it.label, col, false, s.textScale);
    const std::string right = it.submenu.empty() ? it.shortcut : ">";
    p.textIn({r.right() - float(right.size() + 1) * g.x, r.y, r.w, r.h}, right, s.textDisabled, false, s.textScale);
  }
}

// ---- MenuBar

MenuBar::MenuBar() : Widget("menu") { setFocusable(true); }

int MenuBar::openIndex() const { return ui() && ui()->isPopupOpen(openMenu_) ? open_ : -1; }

void MenuBar::addMenu(std::string title, std::vector<MenuItem> items) { menus_.emplace_back(std::move(title), std::move(items)); }

Vec2 MenuBar::measure(const Theme& theme) const {
  return {0, theme.glyphSize(theme.style(styleClass()).textScale).y + 8};
}

Rect MenuBar::titleRect(int i) const {
  const Theme& t = theme();
  float x = rect().x + 4;
  for (int k = 0; k < i; ++k) x += t.textWidth(menus_[size_t(k)].first, style().textScale) + 16;
  return {x, rect().y, t.textWidth(menus_[size_t(i)].first, style().textScale) + 16, rect().h};
}

int MenuBar::titleAt(Vec2 p) const {
  for (int i = 0; i < count(); ++i)
    if (titleRect(i).contains(p)) return i;
  return -1;
}

void MenuBar::openMenu(int index) {
  closeMenu();
  if (index < 0 || index >= count() || !ui()) return;
  open_ = index;
  const Rect t = titleRect(index);
  openMenu_ = &Menu::open(*ui(), menus_[size_t(index)].second, {t.x, t.bottom()});
  openMenu_->onDone = [this] { openMenu_ = nullptr; };
  openMenu_->onNavigate = [this](int dir) { openMenu((open_ + dir + count()) % count()); };
}

void MenuBar::closeMenu() {
  if (openMenu_ && ui() && ui()->isPopupOpen(openMenu_)) ui()->closePopup(*openMenu_);
  openMenu_ = nullptr;
}

bool MenuBar::onPointerDown(const UiEvent& e) {
  const int i = titleAt(e.pos);
  if (i < 0) return false;
  if (openIndex() == i) closeMenu();
  else openMenu(i);
  return true;
}

void MenuBar::onPointerMove(const UiEvent& e) {
  const int i = titleAt(e.pos);
  if (i >= 0 && openIndex() >= 0 && i != openIndex()) openMenu(i);
}

bool MenuBar::onKey(const UiEvent& e) {
  if (count() == 0) return false;
  switch (e.key) {
    case Key::Left: open_ = (open_ + count() - 1) % count(); return true;
    case Key::Right: open_ = (open_ + 1) % count(); return true;
    case Key::Enter:
    case Key::Down:
    case Key::Space: openMenu(open_); return true;
    default: return false;
  }
}

void MenuBar::paint(Painter& p) const {
  const Style& s = style();
  p.fill(rect(), s.background);
  for (int i = 0; i < count(); ++i) {
    const Rect t = titleRect(i);
    if (i == openIndex()) p.fill(t, s.accent.withAlpha(0.5f));
    else if (focused() && openIndex() < 0 && i == open_) p.fill(t, s.hover);
    p.textIn(t, menus_[size_t(i)].first, s.text, true, s.textScale);
  }
}

// ---- Dropdown (a button + a Menu of its items)

Dropdown::Dropdown(std::vector<std::string> items, int selected) : Widget("button"), items_(std::move(items)) {
  setFocusable(true);
  setSelected(selected);
}

void Dropdown::setItems(std::vector<std::string> items) {
  items_ = std::move(items);
  selected_ = std::clamp(selected_, 0, std::max(0, int(items_.size()) - 1));
}

void Dropdown::setSelected(int index, bool notify) {
  index = std::clamp(index, 0, std::max(0, int(items_.size()) - 1));
  if (index == selected_) return;
  selected_ = index;
  if (notify) emit("change");
}

void Dropdown::open() {
  if (!ui() || items_.empty()) return;
  std::vector<MenuItem> list;
  for (int i = 0; i < int(items_.size()); ++i) {
    MenuItem m;
    m.label = items_[size_t(i)];
    m.checked = i == selected_ ? 1 : 0;
    m.action = [this, i] { setSelected(i, true); };
    list.push_back(std::move(m));
  }
  Menu& m = Menu::open(*ui(), std::move(list), {rect().x, rect().bottom()});
  m.layout().minSize = {rect().w, 0};
  popup_ = &m;
}

Vec2 Dropdown::measure(const Theme& theme) const {
  const Style& s = theme.style(styleClass());
  size_t widest = 4;
  for (const auto& i : items_) widest = std::max(widest, i.size());
  const Vec2 g = theme.glyphSize(s.textScale);
  return {float(widest + 3) * g.x + s.padding.horizontal(), g.y + s.padding.vertical()};
}

bool Dropdown::onPointerDown(const UiEvent& e) {
  if (e.button != PointerButton::Left) return false;
  open();
  return false;  // no capture: the popup takes over
}

bool Dropdown::onKey(const UiEvent& e) {
  switch (e.key) {
    case Key::Enter:
    case Key::Space: open(); return true;
    case Key::Up: setSelected(selected_ - 1, true); return true;
    case Key::Down: setSelected(selected_ + 1, true); return true;
    default: return false;
  }
}

void Dropdown::paint(Painter& p) const {
  const Style& s = style();
  const bool open = ui() && ui()->isPopupOpen(popup_);
  p.fill(rect(), open ? s.pressed : stateBackground());
  p.frame(rect(), s.border, s.borderWidth);
  const Rect c = contentRect();
  if (!items_.empty()) p.textIn(c, items_[size_t(selected_)], s.text, false, s.textScale);
  p.textIn({c.right() - p.theme().glyphSize(s.textScale).x, c.y, c.w, c.h}, "v", s.textDisabled, false, s.textScale);
}

}  // namespace litholattice::ui
