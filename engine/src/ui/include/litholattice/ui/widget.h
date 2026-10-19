// Widget: the node of the retained UI tree. Owns its children, has a style class (theme), a
// layout description (anchors in its parent, or stack / grid slot parameters), and virtual
// hooks for measuring, arranging, painting and input. Containers override arrange().
#pragma once
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "litholattice/ui/events.h"
#include "litholattice/ui/painter.h"

namespace litholattice::ui {

class UiRoot;

/// How a widget is placed in its parent.
///  - Anchors (default parent): per axis, anchorMin == anchorMax places a box of `size` (or the
///    preferred size) at that fraction of the parent, shifted by `offset` and `pivot`; otherwise
///    the widget stretches between the anchors, inset by `insets`.
///  - Stacks / grids use `size`, `flex` and `margin` and ignore the anchors.
struct Layout {
  Vec2 anchorMin{0, 0}, anchorMax{1, 1};
  Edges insets;         ///< stretched axes: inset from the anchored edges
  Vec2 offset;          ///< fixed axes: offset from the anchor point
  Vec2 pivot{0, 0};     ///< fixed axes: which point of the box sits on the anchor (0..1)
  Vec2 size;            ///< fixed size; 0 = preferred (measure)
  Vec2 minSize;
  Edges margin;
  float flex = 0;       ///< stacks: share of the leftover space along the stack axis

  /// Fills the parent (optionally inset).
  static Layout fill(Edges insets = {}) { Layout l; l.insets = insets; return l; }
  /// A box of `size` with its `pivot` at `anchor` of the parent, plus `offset`.
  static Layout at(Vec2 anchor, Vec2 pivot, Vec2 offset = {}, Vec2 size = {}) {
    Layout l;
    l.anchorMin = l.anchorMax = anchor;
    l.pivot = pivot;
    l.offset = offset;
    l.size = size;
    return l;
  }
  static Layout sized(Vec2 size, float flex = 0) { Layout l = at({0, 0}, {0, 0}, {}, size); l.flex = flex; return l; }
};

/// Where a widget with layout `l` and preferred size `size` goes inside `parent`.
Rect anchoredRect(const Layout& l, const Rect& parent, Vec2 size);

class Widget {
 public:
  using Handler = std::function<void(Widget&)>;

  explicit Widget(std::string styleClass = "none");
  virtual ~Widget();
  Widget(const Widget&) = delete;
  Widget& operator=(const Widget&) = delete;

  // ---- tree
  /// Constructs a child of type T in place; returns it.
  template <typename T, typename... Args>
  T& add(Args&&... args) {
    auto w = std::make_unique<T>(std::forward<Args>(args)...);
    T& ref = *w;
    addChild(std::move(w));
    return ref;
  }
  Widget& addChild(std::unique_ptr<Widget> child);
  /// Detaches `child` and returns ownership (nullptr if not a child).
  std::unique_ptr<Widget> removeChild(const Widget& child);
  void clearChildren();
  Widget* parent() const { return parent_; }
  std::span<const std::unique_ptr<Widget>> children() const { return children_; }
  /// Depth-first search of this subtree by id.
  Widget* find(std::string_view id);
  template <typename T>
  T* findAs(std::string_view id) { return dynamic_cast<T*>(find(id)); }
  UiRoot* ui() const { return ui_; }

  // ---- properties
  const std::string& id() const { return id_; }
  Widget& setId(std::string id) { id_ = std::move(id); return *this; }
  const std::string& styleClass() const { return styleClass_; }
  Widget& setStyleClass(std::string c) { styleClass_ = std::move(c); return *this; }
  bool visible() const { return visible_; }
  Widget& setVisible(bool v);
  bool enabled() const { return enabled_; }
  Widget& setEnabled(bool e) { enabled_ = e; return *this; }
  /// Visible and enabled up to the root.
  bool interactive() const;
  bool focusable() const { return focusable_; }
  const std::string& tooltip() const { return tooltip_; }
  Widget& setTooltip(std::string t) { tooltip_ = std::move(t); return *this; }
  Layout& layout() { return layout_; }
  const Layout& layout() const { return layout_; }
  Widget& setLayout(const Layout& l) { layout_ = l; return *this; }
  /// Rectangle assigned by the last layout pass (screen pixels).
  const Rect& rect() const { return rect_; }
  const Style& style() const;
  /// The attached UiRoot's theme (the built-in theme while detached).
  const Theme& theme() const;
  /// rect() minus the style padding.
  Rect contentRect() const { return rect_.inset(style().padding); }

  // ---- state shortcuts (owned by UiRoot)
  bool hovered() const;
  bool pressed() const;   ///< has pointer capture
  bool focused() const;
  void focus();

  // ---- events
  /// Adds a C++ handler for `event` ("click", "change", "submit", "select", ...).
  Widget& on(std::string event, Handler h);
  /// Names a script function (e.g. "void onSave(const string &in)") to call on `event`; it
  /// receives this widget's id. Runs through UiRoot's IScriptHost.
  Widget& onScript(std::string event, std::string functionDecl);
  /// Runs the handlers of `event` (C++ first, then the script handler).
  void emit(std::string_view event);
  bool hasHandler(std::string_view event) const;

  // ---- layout (called by UiRoot / parents)
  /// Preferred outer size (content + padding). Default: the anchored children's extent or 0.
  virtual Vec2 measure(const Theme& theme) const;
  /// The preferred size a widget gets from its parent, honouring layout().size and minSize.
  static Vec2 childSize(const Widget& child, const Theme& theme);
  /// Places this widget at `r` and arranges its children.
  void place(const Rect& r, const Theme& theme);

  // ---- painting
  void paintTree(Painter& p) const;

  // ---- input (return true when handled; unhandled keys bubble to the parent)
  virtual bool onPointerDown(const UiEvent&) { return false; }
  virtual void onPointerUp(const UiEvent&) {}
  /// Pointer motion while hovered or captured.
  virtual void onPointerMove(const UiEvent&) {}
  virtual bool onWheel(const UiEvent&) { return false; }
  virtual bool onKey(const UiEvent&) { return false; }
  virtual bool onText(char32_t) { return false; }
  virtual void onFocusChanged(bool /*focused*/) {}
  /// Deepest interactive widget under `p` in this subtree (children last-painted first).
  virtual Widget* hitTest(Vec2 p);

 protected:
  /// Positions children inside contentRect(); default: anchors.
  virtual void arrange(const Theme& theme);
  /// Draws this widget (not its children). Default: background + border of the style.
  virtual void paint(Painter& p) const;
  /// Draws children; containers that scroll or clip override this.
  virtual void paintChildren(Painter& p) const;
  void setFocusable(bool f) { focusable_ = f; }
  /// Re-arranges this subtree in its current rectangle (after scrolling, splitter drags, ...).
  void relayout() { place(rect_, theme()); }
  /// Background for the current state: pressed > hovered > normal (disabled overrides).
  Color stateBackground() const;

 private:
  friend class UiRoot;
  void attach(UiRoot* ui);

  std::string id_, styleClass_, tooltip_;
  bool visible_ = true, enabled_ = true, focusable_ = false;
  Layout layout_;
  Rect rect_;
  Widget* parent_ = nullptr;
  UiRoot* ui_ = nullptr;
  std::vector<std::unique_ptr<Widget>> children_;
  std::vector<std::pair<std::string, Handler>> handlers_;
  std::vector<std::pair<std::string, std::string>> scriptHandlers_;
};

}  // namespace litholattice::ui
