// Menus: Menu (a popup list of MenuItems with shortcuts, checks, separators and submenus),
// MenuBar (top-level titles that open menus) and ContextMenu (a menu at the pointer).
// Items run a C++ action and / or a script handler; the menu closes afterwards.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "litholattice/ui/widget.h"

namespace litholattice::ui {

class UiRoot;

struct MenuItem {
  std::string label, shortcut;  ///< shortcut is display text only ("Ctrl+S")
  std::function<void()> action;
  std::string script;           ///< optional script handler, called with the item label
  std::vector<MenuItem> submenu;
  bool enabled = true, separator = false;
  int checked = -1;             ///< -1 = not checkable, 0 / 1 = check mark

  static MenuItem separatorItem() { MenuItem m; m.separator = true; return m; }
};

/// A popup menu. Open it with Menu::open (or MenuBar / ContextMenu do it).
class Menu : public Widget {
 public:
  explicit Menu(std::vector<MenuItem> items);
  /// Opens `items` as a popup with its top-left at `pos`; returns the menu.
  static Menu& open(UiRoot& ui, std::vector<MenuItem> items, Vec2 pos);
  const std::vector<MenuItem>& items() const { return items_; }
  int highlighted() const { return highlight_; }
  /// Runs item `index` (action, script, then closes the menu chain). False if disabled.
  bool activate(int index);
  Vec2 measure(const Theme& theme) const override;
  bool onPointerDown(const UiEvent& e) override;
  void onPointerUp(const UiEvent& e) override;
  void onPointerMove(const UiEvent& e) override;
  bool onKey(const UiEvent& e) override;
  /// Called when an item ran (the bar closes its open menu).
  std::function<void()> onDone;
  /// Left / Right on a top-level menu (a menu bar switches to the neighbour; dir = -1 / +1).
  std::function<void(int dir)> onNavigate;

 protected:
  void paint(Painter& p) const override;

 private:
  float rowHeight() const;
  Rect rowRect(int i) const;
  int rowAt(Vec2 p) const;
  void openSubmenu(int index);
  void step(int dir);
  std::vector<MenuItem> items_;
  int highlight_ = -1;
  Menu* child_ = nullptr;   ///< open submenu
  Menu* parentMenu_ = nullptr;
};

/// Horizontal bar of menu titles. Click (or Enter / Down) opens a menu below its title; while a
/// menu is open, hovering another title switches to it; Left / Right switch too.
class MenuBar : public Widget {
 public:
  MenuBar();
  void addMenu(std::string title, std::vector<MenuItem> items);
  int count() const { return int(menus_.size()); }
  /// Index of the open menu, -1 if none.
  int openIndex() const;
  void openMenu(int index);
  void closeMenu();
  Vec2 measure(const Theme& theme) const override;
  bool onPointerDown(const UiEvent& e) override;
  void onPointerMove(const UiEvent& e) override;
  bool onKey(const UiEvent& e) override;

 protected:
  void paint(Painter& p) const override;

 private:
  Rect titleRect(int i) const;
  int titleAt(Vec2 p) const;
  std::vector<std::pair<std::string, std::vector<MenuItem>>> menus_;
  int open_ = 0;
  Menu* openMenu_ = nullptr;
};

/// Opens a context menu at the pointer (call from a "rightclick" handler).
struct ContextMenu {
  static Menu& open(UiRoot& ui, Vec2 pointer, std::vector<MenuItem> items) { return Menu::open(ui, std::move(items), pointer); }
};

}  // namespace litholattice::ui
