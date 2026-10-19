#include "litholattice/ui/lists.h"
#include <algorithm>
#include <cmath>
#include "litholattice/ui/ui_root.h"

namespace litholattice::ui {
namespace {

/// Keeps `row` inside a view of `viewH` pixels scrolled by `scroll`.
float scrollToRow(float scroll, int row, float rowH, float viewH) {
  if (viewH <= 0) return scroll;  // not laid out yet
  const float top = float(row) * rowH;
  if (top < scroll) return top;
  if (top + rowH > scroll + viewH) return top + rowH - viewH;
  return scroll;
}

/// Node at a path of child indices (nullptr if the path is invalid); works for const and not.
template <typename Nodes>
auto* findNode(Nodes& roots, const std::vector<int>& path) {
  Nodes* level = &roots;
  decltype(&roots.front()) node = nullptr;
  for (const int i : path) {
    if (i < 0 || i >= int(level->size())) return decltype(node){nullptr};
    node = &(*level)[size_t(i)];
    level = &node->children;
  }
  return node;
}

float clampScroll(float scroll, int rows, float rowH, float viewH) {
  return std::clamp(scroll, 0.f, std::max(0.f, float(rows) * rowH - viewH));
}

}  // namespace

// ---- ListView

ListView::ListView() : Widget("list") { setFocusable(true); }

void ListView::setItems(std::vector<std::string> items) {
  items_ = std::move(items);
  if (selected_ >= int(items_.size())) selected_ = int(items_.size()) - 1;
  scroll_ = clampScroll(scroll_, int(items_.size()), rowHeight(), contentRect().h);
}

float ListView::rowHeight() const { return theme().glyphSize(style().textScale).y + 4; }

void ListView::setSelected(int index, bool notify) {
  index = std::clamp(index, -1, int(items_.size()) - 1);
  if (index == selected_) return;
  selected_ = index;
  if (index >= 0) scrollTo(index);
  if (notify) emit("select");
}

const std::string* ListView::selectedItem() const { return selected_ >= 0 ? &items_[size_t(selected_)] : nullptr; }

void ListView::scrollTo(int row) {
  scroll_ = clampScroll(scrollToRow(scroll_, row, rowHeight(), contentRect().h), int(items_.size()), rowHeight(), contentRect().h);
}

Vec2 ListView::measure(const Theme& theme) const {
  const Style& s = theme.style(styleClass());
  size_t widest = 8;
  for (const auto& i : items_) widest = std::max(widest, i.size());
  const float rh = theme.glyphSize(s.textScale).y + 4;
  return {float(widest) * theme.glyphSize(s.textScale).x + s.padding.horizontal(),
          rh * float(std::clamp(int(items_.size()), 3, 10)) + s.padding.vertical()};
}

int ListView::rowAt(Vec2 p) const {
  const Rect c = contentRect();
  if (!c.contains(p)) return -1;
  const int row = int(std::floor((p.y - c.y + scroll_) / rowHeight()));
  return row < int(items_.size()) ? row : -1;
}

bool ListView::onPointerDown(const UiEvent& e) {
  if (e.button != PointerButton::Left) return false;
  const int row = rowAt(e.pos);
  if (row < 0) return true;
  if (row == selected_) emit("activate");
  else setSelected(row, true);
  return true;
}

bool ListView::onWheel(const UiEvent& e) {
  scroll_ = clampScroll(scroll_ - e.wheel * 3 * rowHeight(), int(items_.size()), rowHeight(), contentRect().h);
  return true;
}

bool ListView::onKey(const UiEvent& e) {
  const int page = std::max(1, int(contentRect().h / rowHeight()));
  const int last = int(items_.size()) - 1;
  switch (e.key) {
    case Key::Up: setSelected(std::max(0, selected_ - 1), true); return true;
    case Key::Down: setSelected(std::min(last, selected_ + 1), true); return true;
    case Key::PageUp: setSelected(std::max(0, selected_ - page), true); return true;
    case Key::PageDown: setSelected(std::min(last, selected_ + page), true); return true;
    case Key::Home: setSelected(0, true); return true;
    case Key::End: setSelected(last, true); return true;
    case Key::Enter:
      if (selected_ >= 0) emit("activate");
      return true;
    default: return false;
  }
}

void ListView::paint(Painter& p) const {
  Widget::paint(p);
  const Style& s = style();
  const Rect c = contentRect();
  const float rh = rowHeight();
  Painter::ClipScope clip(p, c);
  const int first = int(scroll_ / rh), last = std::min(int(items_.size()) - 1, first + int(c.h / rh) + 1);
  for (int i = first; i <= last; ++i) {
    const Rect row{c.x, c.y + float(i) * rh - scroll_, c.w, rh};
    if (i == selected_) p.fill(row, focused() ? s.accent.withAlpha(0.55f) : s.pressed);
    p.textIn(row.inset({4, 0, 0, 0}), items_[size_t(i)], s.text, false, s.textScale);
  }
}

// ---- TreeView

TreeView::TreeView() : Widget("list") { setFocusable(true); }

void TreeView::setRoots(std::vector<TreeNode> roots) {
  roots_ = std::move(roots);
  selectedPath_.clear();
}

float TreeView::rowHeight() const { return theme().glyphSize(style().textScale).y + 4; }

std::vector<TreeView::Row> TreeView::rows() const {
  std::vector<Row> out;
  std::vector<int> path;
  const auto walk = [&](const auto& self, const std::vector<TreeNode>& nodes, int depth) -> void {
    for (size_t i = 0; i < nodes.size(); ++i) {
      path.push_back(int(i));
      out.push_back({path, &nodes[i], depth});
      if (nodes[i].expanded) self(self, nodes[i].children, depth + 1);
      path.pop_back();
    }
  };
  walk(walk, roots_, 0);
  return out;
}

TreeNode* TreeView::nodeAt(const std::vector<int>& path) { return findNode(roots_, path); }

const TreeNode* TreeView::selected() const { return findNode(roots_, selectedPath_); }

int TreeView::selectedRow(const std::vector<Row>& rs) const {
  for (size_t i = 0; i < rs.size(); ++i)
    if (rs[i].path == selectedPath_) return int(i);
  return -1;
}

void TreeView::selectPath(std::vector<int> path, bool notify) {
  if (path == selectedPath_ || (!path.empty() && !nodeAt(path))) return;
  selectedPath_ = std::move(path);
  const auto rs = rows();
  const int row = selectedRow(rs);
  if (row >= 0) scroll_ = clampScroll(scrollToRow(scroll_, row, rowHeight(), contentRect().h), int(rs.size()), rowHeight(), contentRect().h);
  if (notify) emit("select");
}

Vec2 TreeView::measure(const Theme& theme) const {
  const Style& s = theme.style(styleClass());
  return {200 + s.padding.horizontal(), (theme.glyphSize(s.textScale).y + 4) * 10 + s.padding.vertical()};
}

bool TreeView::onPointerDown(const UiEvent& e) {
  if (e.button != PointerButton::Left) return false;
  const Rect c = contentRect();
  const auto rs = rows();
  const int row = int(std::floor((e.pos.y - c.y + scroll_) / rowHeight()));
  if (row < 0 || row >= int(rs.size())) return true;
  const Row& r = rs[size_t(row)];
  const float g = theme().glyphSize(style().textScale).x;
  const float arrowX = c.x + float(r.depth) * 2 * g;
  if (e.pos.x < arrowX + 2 * g && !r.node->children.empty()) {
    TreeNode* n = nodeAt(r.path);
    n->expanded = !n->expanded;
    return true;
  }
  if (r.path == selectedPath_) emit("activate");
  else selectPath(r.path, true);
  return true;
}

bool TreeView::onWheel(const UiEvent& e) {
  scroll_ = clampScroll(scroll_ - e.wheel * 3 * rowHeight(), visibleRowCount(), rowHeight(), contentRect().h);
  return true;
}

bool TreeView::onKey(const UiEvent& e) {
  const auto rs = rows();
  if (rs.empty()) return false;
  const int row = selectedRow(rs);
  TreeNode* n = nodeAt(selectedPath_);
  switch (e.key) {
    case Key::Up: selectPath(rs[size_t(std::max(0, row - 1))].path, true); return true;
    case Key::Down: selectPath(rs[size_t(std::min(int(rs.size()) - 1, row + 1))].path, true); return true;
    case Key::Right:
      if (n && !n->children.empty()) {
        if (!n->expanded) n->expanded = true;
        else selectPath(rs[size_t(row + 1)].path, true);
      }
      return true;
    case Key::Left:
      if (n && n->expanded) n->expanded = false;
      else if (selectedPath_.size() > 1) selectPath({selectedPath_.begin(), selectedPath_.end() - 1}, true);
      return true;
    case Key::Enter:
      if (n) emit("activate");
      return true;
    default: return false;
  }
}

void TreeView::paint(Painter& p) const {
  Widget::paint(p);
  const Style& s = style();
  const Rect c = contentRect();
  const float rh = rowHeight(), g = p.theme().glyphSize(s.textScale).x;
  Painter::ClipScope clip(p, c);
  const auto rs = rows();
  for (size_t i = 0; i < rs.size(); ++i) {
    const Rect row{c.x, c.y + float(i) * rh - scroll_, c.w, rh};
    if (row.bottom() < c.y || row.y > c.bottom()) continue;
    const Row& r = rs[i];
    if (r.path == selectedPath_) p.fill(row, focused() ? s.accent.withAlpha(0.55f) : s.pressed);
    const float x = c.x + float(r.depth) * 2 * g;
    if (!r.node->children.empty()) p.textIn({x, row.y, 2 * g, rh}, r.node->expanded ? "-" : "+", s.accent, true, s.textScale);
    p.textIn({x + 2 * g, row.y, c.w, rh}, r.node->text, s.text, false, s.textScale);
  }
}

}  // namespace litholattice::ui
