// TextBuffer: the editing model behind TextBox and TextEditor, independent of drawing. Lines of
// ASCII / UTF-8 bytes, a caret and a selection anchor, the text position API (line / column <->
// offset), editing operations and undo / redo. Columns are byte indices (the UI font is ASCII).
#pragma once
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace litholattice::ui {

struct TextPos {
  int line = 0, col = 0;
  friend auto operator<=>(const TextPos&, const TextPos&) = default;
};

class TextBuffer {
 public:
  explicit TextBuffer(bool multiLine = true);

  // ---- content
  void setText(std::string_view text);  ///< resets caret, selection and history
  std::string text() const;
  int lineCount() const { return int(lines_.size()); }
  const std::string& line(int i) const { return lines_[size_t(i)]; }
  bool multiLine() const { return multiLine_; }
  /// Bumped by every edit (lets views and owners notice changes cheaply).
  uint64_t revision() const { return revision_; }

  // ---- positions
  TextPos clamp(TextPos p) const;
  /// Byte offset of `p` in text() ('\n' counts as one).
  size_t offsetOf(TextPos p) const;
  TextPos posAt(size_t offset) const;
  TextPos endPos() const { return {lineCount() - 1, int(lines_.back().size())}; }

  // ---- caret and selection
  TextPos caret() const { return caret_; }
  TextPos anchor() const { return anchor_; }
  bool hasSelection() const { return caret_ != anchor_; }
  /// Ordered (begin, end).
  std::pair<TextPos, TextPos> selection() const;
  std::string selectedText() const;
  /// Moves the caret; keeps the anchor when `extend` (shift-selection).
  void setCaret(TextPos p, bool extend = false);
  void select(TextPos from, TextPos to);
  void selectAll();

  // ---- editing (replace the selection; recorded for undo)
  void insert(std::string_view s);
  void backspace();
  void deleteForward();
  void newline();
  /// Removes the selection; returns false if there was none.
  bool eraseSelection();

  // ---- caret movement
  void moveLeft(bool extend, bool word = false);
  void moveRight(bool extend, bool word = false);
  void moveUp(bool extend, int lines = 1);
  void moveDown(bool extend, int lines = 1);
  void moveHome(bool extend);  ///< first non-blank, then column 0
  void moveEnd(bool extend);
  void moveDocStart(bool extend) { setCaret({0, 0}, extend); }
  void moveDocEnd(bool extend) { setCaret(endPos(), extend); }

  // ---- history
  bool undo();
  bool redo();
  bool canUndo() const { return !undo_.empty(); }

 private:
  struct Snapshot {
    std::vector<std::string> lines;
    TextPos caret, anchor;
  };
  enum class EditKind { Other, Typing, Deleting };
  void record(EditKind kind);
  void replaceRange(TextPos from, TextPos to, std::string_view s);
  void changed();

  std::vector<std::string> lines_{std::string()};
  TextPos caret_, anchor_;
  int preferredCol_ = -1;  ///< column kept across vertical moves
  bool multiLine_;
  uint64_t revision_ = 0;
  std::vector<Snapshot> undo_, redo_;
  EditKind lastEdit_ = EditKind::Other;
  static constexpr size_t kMaxHistory = 256;
};

}  // namespace litholattice::ui
