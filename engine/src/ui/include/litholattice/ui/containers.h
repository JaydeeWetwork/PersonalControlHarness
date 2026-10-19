// Layout containers and scrolling: StackPanel (vertical / horizontal), GridPanel, ScrollBar,
// ScrollView, Splitter, TabView.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "litholattice/ui/widget.h"

namespace litholattice::ui {

enum class Orientation { Vertical, Horizontal };

/// Children one after another with `spacing`; along the axis each child gets its size (or
/// layout().size) plus a share of the leftover space by layout().flex; across the axis it
/// stretches (or keeps its size when `stretch` is false). Child margins are honoured.
class StackPanel : public Widget {
 public:
  explicit StackPanel(Orientation o = Orientation::Vertical, float spacing = 4, std::string styleClass = "stack");
  Orientation orientation() const { return orientation_; }
  StackPanel& setSpacing(float s) { spacing_ = s; return *this; }
  StackPanel& setStretch(bool s) { stretch_ = s; return *this; }
  Vec2 measure(const Theme& theme) const override;

 protected:
  void arrange(const Theme& theme) override;

 private:
  Orientation orientation_;
  float spacing_;
  bool stretch_ = true;
};

/// Fixed-size cells in `columns` columns (row-major), `spacing` between cells.
class GridPanel : public Widget {
 public:
  GridPanel(int columns, Vec2 cell, float spacing = 4, std::string styleClass = "stack");
  int columns() const { return columns_; }
  Vec2 cell() const { return cell_; }
  GridPanel& setColumns(int c) { columns_ = std::max(1, c); return *this; }
  Vec2 measure(const Theme& theme) const override;

 protected:
  void arrange(const Theme& theme) override;

 private:
  int columns_;
  Vec2 cell_;
  float spacing_;
};

/// Vertical or horizontal scroll bar over [0, contentSize - viewSize]; "change" on user input.
class ScrollBar : public Widget {
 public:
  explicit ScrollBar(Orientation o = Orientation::Vertical);
  void setRange(float contentSize, float viewSize);
  float offset() const { return offset_; }
  float maxOffset() const { return std::max(0.f, content_ - view_); }
  void setOffset(float o, bool notify = false);
  bool needed() const { return content_ > view_ + 0.5f; }
  Vec2 measure(const Theme&) const override { return {kThickness, kThickness}; }
  bool onPointerDown(const UiEvent& e) override;
  void onPointerMove(const UiEvent& e) override;
  bool onWheel(const UiEvent& e) override;

  static constexpr float kThickness = 10;

 protected:
  void paint(Painter& p) const override;

 private:
  Rect thumb() const;
  float along(Vec2 p) const { return orientation_ == Orientation::Vertical ? p.y : p.x; }
  Orientation orientation_;
  float content_ = 0, view_ = 0, offset_ = 0, grab_ = -1;
};

/// Scrolls one content widget vertically (wheel, scroll bar, ensureVisible()). The content
/// keeps the view's width and its own preferred height.
class ScrollView : public Widget {
 public:
  ScrollView();
  /// Replaces the content; returns it.
  Widget& setContent(std::unique_ptr<Widget> content);
  template <typename T, typename... Args>
  T& emplaceContent(Args&&... args) {
    return static_cast<T&>(setContent(std::make_unique<T>(std::forward<Args>(args)...)));
  }
  Widget* content() const { return content_; }
  float scroll() const { return bar_->offset(); }
  void setScroll(float y) { bar_->setOffset(y); }
  /// Scrolls the minimum needed so `r` (screen coordinates at the current scroll) is visible.
  void ensureVisible(const Rect& r);
  Vec2 measure(const Theme& theme) const override;
  bool onWheel(const UiEvent& e) override;
  Widget* hitTest(Vec2 p) override;

 protected:
  void arrange(const Theme& theme) override;
  void paintChildren(Painter& p) const override;

 private:
  Rect viewRect() const;
  Widget* content_ = nullptr;
  ScrollBar* bar_ = nullptr;
};

/// Two panes with a draggable divider; ratio() is the first pane's share.
class Splitter : public Widget {
 public:
  explicit Splitter(Orientation o = Orientation::Horizontal, float ratio = 0.3f);
  /// Sets pane 0 or 1 (replacing any previous one); returns it.
  Widget& setPane(int index, std::unique_ptr<Widget> pane);
  float ratio() const { return ratio_; }
  void setRatio(float r);
  bool onPointerDown(const UiEvent& e) override;
  void onPointerMove(const UiEvent& e) override;
  Widget* hitTest(Vec2 p) override;

  static constexpr float kHandle = 6;

 protected:
  void arrange(const Theme& theme) override;
  void paint(Painter& p) const override;

 private:
  Rect handleRect() const;
  Orientation orientation_;
  float ratio_;
  Widget* panes_[2] = {nullptr, nullptr};
};

/// Pages with a tab header; one page visible. "change" when the user switches tabs.
class TabView : public Widget {
 public:
  TabView();
  /// Adds a page; returns it.
  Widget& addTab(std::string title, std::unique_ptr<Widget> page);
  int current() const { return current_; }
  void setCurrent(int index, bool notify = false);
  int count() const { return int(titles_.size()); }
  Vec2 measure(const Theme& theme) const override;
  bool onPointerDown(const UiEvent& e) override;
  bool onKey(const UiEvent& e) override;

 protected:
  void arrange(const Theme& theme) override;
  void paint(Painter& p) const override;

 private:
  float headerHeight() const;
  Rect tabRect(int i) const;
  std::vector<std::string> titles_;
  std::vector<Widget*> pages_;
  int current_ = -1;
};

}  // namespace litholattice::ui
