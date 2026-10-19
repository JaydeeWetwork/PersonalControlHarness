#include "litholattice/ui/text_edit.h"
#include <algorithm>
#include <cmath>
#include "litholattice/ui/ui_root.h"

namespace litholattice::ui {

// ---- TextEditBase

TextEditBase::TextEditBase(std::string styleClass, bool multiLine) : Widget(std::move(styleClass)), buffer_(multiLine) {
  setFocusable(true);
}

void TextEditBase::setText(std::string_view t) {
  buffer_.setText(t);
  caretMoved();
}

Vec2 TextEditBase::glyph() const { return theme().glyphSize(style().textScale); }

TextPos TextEditBase::positionAt(Vec2 p) const {
  const Vec2 o = textOrigin(), g = glyph();
  const int line = std::clamp(int(std::floor((p.y - o.y) / g.y)), 0, buffer_.lineCount() - 1);
  const int col = int(std::lround((p.x - o.x) / g.x));
  return buffer_.clamp({line, std::max(0, col)});
}

Rect TextEditBase::caretRect() const {
  const Vec2 o = textOrigin(), g = glyph();
  const TextPos c = buffer_.caret();
  return {o.x + float(c.col) * g.x, o.y + float(c.line) * g.y, 1.f, g.y};
}

void TextEditBase::edited() {
  caretMoved();
  emit("change");
}

bool TextEditBase::onPointerDown(const UiEvent& e) {
  if (e.button != PointerButton::Left) return false;
  buffer_.setCaret(positionAt(e.pos), e.shift());
  caretMoved();
  return true;
}

void TextEditBase::onPointerMove(const UiEvent& e) {
  if (!pressed()) return;
  buffer_.setCaret(positionAt(e.pos), true);
  caretMoved();
}

bool TextEditBase::onKey(const UiEvent& e) {
  const bool sel = e.shift(), word = e.ctrl();
  if (e.ctrl()) {
    IClipboard* clip = ui() ? &ui()->clipboard() : nullptr;
    switch (e.key) {
      case Key::A: buffer_.selectAll(); caretMoved(); return true;
      case Key::C:
        if (clip && buffer_.hasSelection()) clip->setText(buffer_.selectedText());
        return true;
      case Key::X:
        if (clip && buffer_.hasSelection() && !readOnly_) {
          clip->setText(buffer_.selectedText());
          buffer_.eraseSelection();
          edited();
        }
        return true;
      case Key::V:
        if (clip && !readOnly_) {
          buffer_.insert(clip->text());
          edited();
        }
        return true;
      case Key::Z:
        if (!readOnly_ && buffer_.undo()) edited();
        return true;
      case Key::Y:
        if (!readOnly_ && buffer_.redo()) edited();
        return true;
      default: break;
    }
  }
  switch (e.key) {
    case Key::Left: buffer_.moveLeft(sel, word); break;
    case Key::Right: buffer_.moveRight(sel, word); break;
    case Key::Home:
      if (e.ctrl()) buffer_.moveDocStart(sel);
      else buffer_.moveHome(sel);
      break;
    case Key::End:
      if (e.ctrl()) buffer_.moveDocEnd(sel);
      else buffer_.moveEnd(sel);
      break;
    case Key::Backspace:
      if (readOnly_) return true;
      buffer_.backspace();
      edited();
      return true;
    case Key::Delete:
      if (readOnly_) return true;
      buffer_.deleteForward();
      edited();
      return true;
    default: return onSpecialKey(e);
  }
  caretMoved();
  return true;
}

bool TextEditBase::onText(char32_t c) {
  if (readOnly_) return true;
  if (c < 32 || c > 126) return c >= 32;  // the font is ASCII: swallow other printable input
  buffer_.insert(std::string(1, char(c)));
  edited();
  return true;
}

void TextEditBase::paintText(Painter& p, const Rect& clip, int firstLine, int lastLine) const {
  const Style& s = style();
  const Vec2 o = textOrigin(), g = glyph();
  Painter::ClipScope scope(p, clip);
  const auto [a, b] = buffer_.selection();
  for (int i = std::max(0, firstLine); i <= std::min(lastLine, buffer_.lineCount() - 1); ++i) {
    const float y = o.y + float(i) * g.y;
    if (buffer_.hasSelection() && i >= a.line && i <= b.line) {
      const int c0 = i == a.line ? a.col : 0;
      const int c1 = i == b.line ? b.col : int(buffer_.line(i).size()) + 1;  // +1: show the line break
      p.fill({o.x + float(c0) * g.x, y, float(c1 - c0) * g.x, g.y}, s.accent.withAlpha(0.35f));
    }
    p.text({o.x, y}, buffer_.line(i), s.text, s.textScale);
  }
  const bool blinkOn = !ui() || std::fmod(ui()->time(), 1.f) < 0.6f;
  if (focused() && blinkOn) p.fill(caretRect(), s.text);
}

// ---- TextBox

TextBox::TextBox(std::string text) : TextEditBase("textbox", false) { setText(text); }

Vec2 TextBox::measure(const Theme& theme) const {
  const Style& s = theme.style(styleClass());
  const Vec2 g = theme.glyphSize(s.textScale);
  return {20 * g.x + s.padding.horizontal(), g.y + s.padding.vertical()};
}

Vec2 TextBox::textOrigin() const {
  const Rect c = contentRect();
  return {c.x - scrollX_, c.y + (c.h - glyph().y) * 0.5f};
}

void TextBox::caretMoved() {
  const Rect c = contentRect();
  if (c.w <= 0) return;  // not laid out yet
  const float x = float(buffer().caret().col) * glyph().x;
  if (x - scrollX_ > c.w - glyph().x) scrollX_ = x - c.w + glyph().x;
  if (x < scrollX_) scrollX_ = x;
  scrollX_ = std::max(0.f, scrollX_);
}

bool TextBox::onSpecialKey(const UiEvent& e) {
  if (e.key != Key::Enter) return false;
  emit("submit");
  return true;
}

void TextBox::paint(Painter& p) const {
  const Style& s = style();
  p.fill(rect(), s.background);
  p.frame(rect(), focused() ? s.accent : s.border, s.borderWidth);
  if (buffer().text().empty() && !focused()) {
    p.textIn(contentRect(), placeholder_, s.textDisabled, false, s.textScale);
    return;
  }
  paintText(p, contentRect(), 0, 0);
}

// ---- TextEditor

TextEditor::TextEditor() : TextEditBase("editor", true) {}

Vec2 TextEditor::measure(const Theme& theme) const {
  const Vec2 g = theme.glyphSize(theme.style(styleClass()).textScale);
  return {60 * g.x, 16 * g.y};
}

float TextEditor::gutterWidth() const {
  if (!lineNumbers_) return 0;
  const int digits = int(std::to_string(buffer().lineCount()).size());
  return float(std::max(3, digits) + 1) * glyph().x;
}

Rect TextEditor::textArea() const {
  const Rect c = contentRect();
  const float gw = gutterWidth();
  return {c.x + gw + 4, c.y, std::max(0.f, c.w - gw - 4), c.h};
}

int TextEditor::visibleLines() const { return std::max(1, int(textArea().h / glyph().y)); }

Vec2 TextEditor::textOrigin() const {
  const Rect a = textArea();
  return {a.x - scrollX_, a.y - scrollY_};
}

void TextEditor::scrollToLine(int line) {
  if (textArea().h <= 0) return;  // not laid out yet
  const float g = glyph().y, top = float(line) * g;
  if (top < scrollY_) scrollY_ = top;
  if (top + g > scrollY_ + textArea().h) scrollY_ = top + g - textArea().h;
  const float maxY = std::max(0.f, float(buffer().lineCount()) * g - textArea().h);
  scrollY_ = std::clamp(scrollY_, 0.f, maxY);
}

void TextEditor::caretMoved() {
  if (textArea().w <= 0) return;  // not laid out yet
  scrollToLine(buffer().caret().line);
  const float x = float(buffer().caret().col) * glyph().x, w = textArea().w;
  if (x - scrollX_ > w - glyph().x) scrollX_ = x - w + glyph().x;
  if (x < scrollX_) scrollX_ = x;
  scrollX_ = std::max(0.f, scrollX_);
}

bool TextEditor::onWheel(const UiEvent& e) {
  const float g = glyph().y;
  const float maxY = std::max(0.f, float(buffer().lineCount()) * g - textArea().h);
  scrollY_ = std::clamp(scrollY_ - e.wheel * 3 * g, 0.f, maxY);
  return true;
}

bool TextEditor::onSpecialKey(const UiEvent& e) {
  const bool sel = e.shift();
  switch (e.key) {
    case Key::Up: buffer().moveUp(sel); break;
    case Key::Down: buffer().moveDown(sel); break;
    case Key::PageUp: buffer().moveUp(sel, visibleLines()); break;
    case Key::PageDown: buffer().moveDown(sel, visibleLines()); break;
    case Key::Enter:
      if (readOnly()) return true;
      buffer().newline();
      edited();
      return true;
    case Key::Tab:
      if (readOnly() || e.shift()) return false;  // Shift+Tab still navigates out
      buffer().insert(std::string(size_t(4 - buffer().caret().col % 4), ' '));
      edited();
      return true;
    case Key::S:
      if (!e.ctrl()) return false;
      emit("save");
      return true;
    default: return false;
  }
  caretMoved();
  return true;
}

void TextEditor::paint(Painter& p) const {
  const Style& s = style();
  p.fill(rect(), s.background);
  p.frame(rect(), focused() ? s.accent : s.border, s.borderWidth);
  const Rect c = contentRect();
  const int first = firstVisibleLine(), last = first + visibleLines();
  if (lineNumbers_) {
    const float gw = gutterWidth();
    p.fill({c.x, c.y, gw, c.h}, s.hover.withAlpha(0.5f));
    Painter::ClipScope scope(p, {c.x, c.y, gw, c.h});
    const Vec2 g = glyph();
    for (int i = first; i <= std::min(last, buffer().lineCount() - 1); ++i) {
      const std::string n = std::to_string(i + 1);
      const bool current = i == buffer().caret().line;
      p.text({c.x + gw - float(n.size() + 1) * g.x, c.y - scrollY_ + float(i) * g.y}, n, current ? s.text : s.textDisabled,
             s.textScale);
    }
  }
  paintText(p, textArea(), first, last);
}

}  // namespace litholattice::ui
