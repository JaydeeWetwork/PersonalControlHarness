// Item views: ListView, TreeView and Dropdown. Events: "select" when the selection changes,
// "activate" on Enter or a click on the selected row; Dropdown "change".
#pragma once
#include <string>
#include <vector>
#include "litholattice/ui/widget.h"

namespace litholattice::ui {

/// Rows of text with one selected row; scrolls with the wheel and follows the keyboard.
class ListView : public Widget {
 public:
  ListView();
  void setItems(std::vector<std::string> items);
  const std::vector<std::string>& items() const { return items_; }
  int selected() const { return selected_; }
  /// -1 clears; emits "select" when it differs and `notify`.
  void setSelected(int index, bool notify = false);
  const std::string* selectedItem() const;
  float rowHeight() const;
  Vec2 measure(const Theme& theme) const override;
  bool onPointerDown(const UiEvent& e) override;
  bool onWheel(const UiEvent& e) override;
  bool onKey(const UiEvent& e) override;

 protected:
  void paint(Painter& p) const override;

 private:
  int rowAt(Vec2 p) const;
  void scrollTo(int row);
  std::vector<std::string> items_;
  int selected_ = -1;
  float scroll_ = 0;
};

struct TreeNode {
  std::string text;
  std::vector<TreeNode> children;
  bool expanded = false;
  int data = 0;  ///< caller payload
};

/// Expandable hierarchy. Right / Left expand / collapse (or move to the parent); the selection
/// is a path of child indices.
class TreeView : public Widget {
 public:
  TreeView();
  std::vector<TreeNode>& roots() { return roots_; }
  void setRoots(std::vector<TreeNode> roots);
  /// Selected node (nullptr if none).
  const TreeNode* selected() const;
  const std::vector<int>& selectedPath() const { return selectedPath_; }
  void selectPath(std::vector<int> path, bool notify = false);
  /// Visible rows in order (for tests / scripts).
  int visibleRowCount() const { return int(rows().size()); }
  Vec2 measure(const Theme& theme) const override;
  bool onPointerDown(const UiEvent& e) override;
  bool onWheel(const UiEvent& e) override;
  bool onKey(const UiEvent& e) override;

 protected:
  void paint(Painter& p) const override;

 private:
  struct Row {
    std::vector<int> path;
    const TreeNode* node;
    int depth;
  };
  std::vector<Row> rows() const;
  TreeNode* nodeAt(const std::vector<int>& path);
  int selectedRow(const std::vector<Row>& rows) const;
  float rowHeight() const;
  std::vector<TreeNode> roots_;
  std::vector<int> selectedPath_;
  float scroll_ = 0;
};

/// A button showing the current item; opens a popup list to choose another.
class Dropdown : public Widget {
 public:
  explicit Dropdown(std::vector<std::string> items = {}, int selected = 0);
  void setItems(std::vector<std::string> items);
  const std::vector<std::string>& items() const { return items_; }
  int selected() const { return selected_; }
  void setSelected(int index, bool notify = false);
  bool isOpen() const { return popup_ != nullptr; }
  void open();
  Vec2 measure(const Theme& theme) const override;
  bool onPointerDown(const UiEvent& e) override;
  bool onKey(const UiEvent& e) override;

 protected:
  void paint(Painter& p) const override;

 private:
  std::vector<std::string> items_;
  int selected_ = 0;
  Widget* popup_ = nullptr;
};

}  // namespace litholattice::ui
