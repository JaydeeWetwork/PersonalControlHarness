// UiRoot: owns the widget tree plus the popup and modal layers, and routes input: hover, pointer
// capture, focus, keyboard navigation (Tab / Shift+Tab, arrows between focusable widgets),
// popups that close on an outside click, modal dialogs that block everything below, tooltips.
#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "litholattice/ui/widget.h"

namespace litholattice::ui {

class Tooltip;

/// Calls script handlers for widget events (implemented by the game on top of AngelScript).
class IScriptHost {
 public:
  virtual ~IScriptHost() = default;
  /// Calls `functionDecl` with the widget id; false if missing or it failed.
  virtual bool callHandler(std::string_view functionDecl, std::string_view widgetId) = 0;
};

class UiRoot {
 public:
  explicit UiRoot(Theme theme = Theme::defaults());
  ~UiRoot();
  UiRoot(const UiRoot&) = delete;
  UiRoot& operator=(const UiRoot&) = delete;

  /// The main layer: add screens / HUD panels here.
  Widget& root() { return *root_; }
  const Theme& theme() const { return theme_; }
  void setTheme(Theme t) { theme_ = std::move(t); }
  void setScriptHost(IScriptHost* host) { scripts_ = host; }
  IScriptHost* scriptHost() const { return scripts_; }
  void setClipboard(IClipboard* c) { clipboard_ = c; }
  IClipboard& clipboard() { return clipboard_ ? *clipboard_ : fallbackClipboard_; }

  // ---- frame
  void layout(Vec2 viewport);
  void paint(IUiRenderer& renderer) const;
  /// Advances time (tooltip delay, caret blink).
  void update(float dt);
  float time() const { return time_; }
  /// Routes one event; true if the UI consumed it (the game should then ignore it).
  bool handleEvent(const UiEvent& e);
  /// True while a modal is open, a popup is open, or a text field has focus: the game should
  /// route all keyboard input to the UI.
  bool wantsKeyboard() const;
  /// True if `p` is over any visible widget (pointer input belongs to the UI there).
  bool wantsPointer(Vec2 p) const;

  /// Destroys `w` once no widget code is running (use when a handler removes its own widget).
  void destroyLater(std::unique_ptr<Widget> w);
  /// Modifier keys of the event being handled (e.g. shift-click in a "click" handler).
  uint8_t modifiers() const { return mods_; }

  // ---- focus / state
  Widget* focus() const { return focus_; }
  void setFocus(Widget* w);
  Widget* hover() const { return hover_; }
  Widget* capture() const { return capture_; }
  /// Moves focus to the next (or previous) focusable widget in the active layer.
  void focusNext(bool backwards);
  /// Moves focus to the nearest focusable widget in a direction (dx, dy in -1..1).
  bool focusDirection(float dx, float dy);

  // ---- layers
  /// Opens a popup (menu, dropdown list) positioned by its own layout; closed by an outside
  /// click, Escape or closePopup(). Stacks: a submenu opens above its menu.
  Widget& openPopup(std::unique_ptr<Widget> popup);
  void closePopup(const Widget& popup);
  void closeAllPopups();
  bool popupOpen() const { return !popups_.empty(); }
  /// True while `popup` is open (popups are closed by the root, so owners check this).
  bool isPopupOpen(const Widget* popup) const;
  /// Shows a modal (dialog): input below it is blocked; Escape is offered to it first.
  Widget& pushModal(std::unique_ptr<Widget> modal);
  /// Closes `modal` (and any modal above it).
  void closeModal(const Widget& modal);
  bool modalOpen() const { return !modals_.empty(); }
  Widget* topModal() const { return modals_.empty() ? nullptr : modals_.back().get(); }

 private:
  friend class Widget;
  void forget(const Widget* w);   // a widget is being destroyed
  /// The layer receiving input: top popup, else top modal, else the root.
  Widget& activeLayer() const;
  Widget* hitTestLayers(Vec2 p) const;
  void collectFocusable(Widget& w, std::vector<Widget*>& out) const;
  void deliverKey(const UiEvent& e);

  Theme theme_;
  std::unique_ptr<Widget> root_;
  std::vector<std::unique_ptr<Widget>> modals_, popups_;
  std::vector<std::unique_ptr<Widget>> closing_;  // destroyed at the end of handleEvent
  Widget *focus_ = nullptr, *hover_ = nullptr, *capture_ = nullptr;
  IScriptHost* scripts_ = nullptr;
  IClipboard* clipboard_ = nullptr;
  MemoryClipboard fallbackClipboard_;
  std::unique_ptr<Tooltip> tooltip_;  // drawn for the hovered widget's tooltip()
  Vec2 viewport_, pointer_;
  float time_ = 0, hoverTime_ = 0;
  uint8_t mods_ = kModNone;
};

}  // namespace litholattice::ui
