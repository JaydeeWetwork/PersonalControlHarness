// Dialog (shown modally with UiRoot::pushModal): title bar (drag to move), a body for content
// and an optional button row. Escape or close() emits "close" and removes it. Tooltip: the
// floating hint UiRoot draws for a hovered widget's tooltip(); usable standalone too.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include "litholattice/ui/containers.h"
#include "litholattice/ui/controls.h"

namespace litholattice::ui {

class UiRoot;

class Dialog : public Widget {
 public:
  explicit Dialog(std::string title, Vec2 size = {});
  /// Shows a new dialog centred on screen as a modal; returns it.
  static Dialog& show(UiRoot& ui, std::string title, Vec2 size = {});
  const std::string& title() const { return title_; }
  /// Where content goes (a vertical stack).
  StackPanel& body() { return *body_; }
  /// Adds a button to the bottom row; `closes` closes the dialog after `onClick`.
  Button& addButton(std::string text, std::function<void()> onClick = {}, bool closes = true);
  /// Emits "close" and removes the dialog from its UiRoot.
  void close();
  /// Consulted when the user closes the dialog (Escape); return false to keep it open (e.g. to
  /// ask about unsaved changes first). close() itself always closes.
  void setCloseGuard(std::function<bool()> guard) { closeGuard_ = std::move(guard); }
  Vec2 measure(const Theme& theme) const override;
  bool onPointerDown(const UiEvent& e) override;
  void onPointerMove(const UiEvent& e) override;
  bool onKey(const UiEvent& e) override;

 protected:
  void arrange(const Theme& theme) override;
  void paint(Painter& p) const override;

 private:
  float titleHeight() const;
  std::string title_;
  StackPanel* body_ = nullptr;
  StackPanel* buttons_ = nullptr;
  Vec2 grab_{-1, -1};
  std::function<bool()> closeGuard_;
};

/// A boxed one-line hint.
class Tooltip : public Widget {
 public:
  explicit Tooltip(std::string text = {}) : Widget("tooltip"), text_(std::move(text)) {}
  void setText(std::string t) { text_ = std::move(t); }
  const std::string& text() const { return text_; }
  Vec2 measure(const Theme& theme) const override;
  Widget* hitTest(Vec2) override { return nullptr; }

 protected:
  void paint(Painter& p) const override;

 private:
  std::string text_;
};

}  // namespace litholattice::ui
