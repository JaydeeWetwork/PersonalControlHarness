// Text input widgets over TextBuffer: TextBox (one line) and TextEditor (multi-line, line
// numbers, scrolling, Tab inserts spaces, Ctrl+S emits "save"). Shared keys: arrows (+Shift
// selects, +Ctrl by word), Home / End, Backspace / Delete, Ctrl+A / C / X / V / Z / Y; mouse
// click places the caret, drag selects. Events: "change" after edits, "submit" (TextBox Enter).
#pragma once
#include <string>
#include "litholattice/ui/text_buffer.h"
#include "litholattice/ui/widget.h"

namespace litholattice::ui {

/// Behaviour shared by TextBox and TextEditor; not used directly.
class TextEditBase : public Widget {
 public:
  TextBuffer& buffer() { return buffer_; }
  const TextBuffer& buffer() const { return buffer_; }
  std::string text() const { return buffer_.text(); }
  void setText(std::string_view t);
  bool readOnly() const { return readOnly_; }
  void setReadOnly(bool r) { readOnly_ = r; }

  /// Text position under a screen point (clamped into the text).
  TextPos positionAt(Vec2 p) const;
  /// Screen rectangle of the caret (1 px wide).
  Rect caretRect() const;

  bool onPointerDown(const UiEvent& e) override;
  void onPointerMove(const UiEvent& e) override;
  bool onKey(const UiEvent& e) override;
  bool onText(char32_t c) override;

 protected:
  TextEditBase(std::string styleClass, bool multiLine);
  /// Top-left of the text (line 0, column 0) on screen, after scrolling.
  virtual Vec2 textOrigin() const = 0;
  /// Called after the caret moved or the text changed (scroll the caret into view).
  virtual void caretMoved() = 0;
  /// Keys only one variant handles (Enter, Tab, PageUp / PageDown, Ctrl+S).
  virtual bool onSpecialKey(const UiEvent& e) = 0;
  Vec2 glyph() const;
  void paintText(Painter& p, const Rect& clip, int firstLine, int lastLine) const;
  void edited();

 private:
  TextBuffer buffer_;
  bool readOnly_ = false;
};

class TextBox final : public TextEditBase {
 public:
  explicit TextBox(std::string text = {});
  TextBox& setPlaceholder(std::string p) { placeholder_ = std::move(p); return *this; }
  Vec2 measure(const Theme& theme) const override;

 protected:
  Vec2 textOrigin() const override;
  void caretMoved() override;
  bool onSpecialKey(const UiEvent& e) override;
  void paint(Painter& p) const override;

 private:
  std::string placeholder_;
  float scrollX_ = 0;
};

class TextEditor final : public TextEditBase {
 public:
  TextEditor();
  TextEditor& setLineNumbers(bool on) { lineNumbers_ = on; return *this; }
  /// Lines that fit in the view.
  int visibleLines() const;
  int firstVisibleLine() const { return int(scrollY_ / glyph().y); }
  /// Scrolls so `line` is visible.
  void scrollToLine(int line);
  Vec2 measure(const Theme& theme) const override;
  bool onWheel(const UiEvent& e) override;

 protected:
  Vec2 textOrigin() const override;
  void caretMoved() override;
  bool onSpecialKey(const UiEvent& e) override;
  void paint(Painter& p) const override;

 private:
  float gutterWidth() const;
  Rect textArea() const;
  bool lineNumbers_ = true;
  float scrollX_ = 0, scrollY_ = 0;
};

}  // namespace litholattice::ui
