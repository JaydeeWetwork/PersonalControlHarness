#include "litholattice/ui/text_buffer.h"
#include <algorithm>
#include <cctype>

namespace litholattice::ui {
namespace {

bool isWord(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

}  // namespace

TextBuffer::TextBuffer(bool multiLine) : multiLine_(multiLine) {}

void TextBuffer::setText(std::string_view text) {
  lines_.assign(1, std::string());
  for (const char c : text) {
    if (c == '\r') continue;
    if (c == '\n' && multiLine_) lines_.emplace_back();
    else if (c != '\n') lines_.back() += c;
  }
  caret_ = anchor_ = {};
  preferredCol_ = -1;
  undo_.clear();
  redo_.clear();
  lastEdit_ = EditKind::Other;
  changed();
}

std::string TextBuffer::text() const {
  std::string out;
  for (size_t i = 0; i < lines_.size(); ++i) {
    if (i) out += '\n';
    out += lines_[i];
  }
  return out;
}

TextPos TextBuffer::clamp(TextPos p) const {
  p.line = std::clamp(p.line, 0, lineCount() - 1);
  p.col = std::clamp(p.col, 0, int(lines_[size_t(p.line)].size()));
  return p;
}

size_t TextBuffer::offsetOf(TextPos p) const {
  p = clamp(p);
  size_t off = 0;
  for (int i = 0; i < p.line; ++i) off += lines_[size_t(i)].size() + 1;
  return off + size_t(p.col);
}

TextPos TextBuffer::posAt(size_t offset) const {
  for (int i = 0; i < lineCount(); ++i) {
    const size_t len = lines_[size_t(i)].size();
    if (offset <= len) return {i, int(offset)};
    offset -= len + 1;
  }
  return endPos();
}

std::pair<TextPos, TextPos> TextBuffer::selection() const {
  return caret_ < anchor_ ? std::pair{caret_, anchor_} : std::pair{anchor_, caret_};
}

std::string TextBuffer::selectedText() const {
  const auto [a, b] = selection();
  const std::string all = text();
  return all.substr(offsetOf(a), offsetOf(b) - offsetOf(a));
}

void TextBuffer::setCaret(TextPos p, bool extend) {
  caret_ = clamp(p);
  if (!extend) anchor_ = caret_;
  preferredCol_ = -1;
  lastEdit_ = EditKind::Other;
}

void TextBuffer::select(TextPos from, TextPos to) {
  anchor_ = clamp(from);
  caret_ = clamp(to);
  preferredCol_ = -1;
}

void TextBuffer::selectAll() { select({0, 0}, endPos()); }

void TextBuffer::record(EditKind kind) {
  // Consecutive typing (or deleting) is one undo step.
  if (kind == EditKind::Other || kind != lastEdit_) {
    undo_.push_back({lines_, caret_, anchor_});
    if (undo_.size() > kMaxHistory) undo_.erase(undo_.begin());
  }
  redo_.clear();
  lastEdit_ = kind;
}

void TextBuffer::changed() { ++revision_; }

void TextBuffer::replaceRange(TextPos from, TextPos to, std::string_view s) {
  const std::string head = lines_[size_t(from.line)].substr(0, size_t(from.col));
  const std::string tail = lines_[size_t(to.line)].substr(size_t(to.col));
  lines_.erase(lines_.begin() + from.line + 1, lines_.begin() + to.line + 1);
  std::vector<std::string> pieces{std::string()};
  for (const char c : s) {
    if (c == '\r') continue;
    if (c == '\n' && multiLine_) pieces.emplace_back();
    else if (c != '\n') pieces.back() += c;
  }
  const int lastCol = int(pieces.back().size()) + (pieces.size() == 1 ? from.col : 0);
  pieces.front() = head + pieces.front();
  pieces.back() += tail;
  lines_[size_t(from.line)] = pieces.front();
  lines_.insert(lines_.begin() + from.line + 1, pieces.begin() + 1, pieces.end());
  caret_ = anchor_ = {from.line + int(pieces.size()) - 1, lastCol};
  preferredCol_ = -1;
  changed();
}

bool TextBuffer::eraseSelection() {
  if (!hasSelection()) return false;
  record(EditKind::Other);
  const auto [a, b] = selection();
  replaceRange(a, b, {});
  return true;
}

void TextBuffer::insert(std::string_view s) {
  const bool typing = s.size() == 1 && !hasSelection() && s[0] != '\n';
  record(typing ? EditKind::Typing : EditKind::Other);
  const auto [a, b] = selection();
  replaceRange(a, b, s);
  if (typing) lastEdit_ = EditKind::Typing;
}

void TextBuffer::newline() {
  if (!multiLine_) return;
  // Keep the current line's indentation.
  const std::string& cur = lines_[size_t(caret_.line)];
  size_t indent = 0;
  while (indent < cur.size() && indent < size_t(caret_.col) && cur[indent] == ' ') ++indent;
  insert("\n" + std::string(indent, ' '));
}

void TextBuffer::backspace() {
  if (hasSelection()) {
    eraseSelection();
    return;
  }
  if (caret_ == TextPos{}) return;
  record(EditKind::Deleting);
  const TextPos from = caret_.col > 0 ? TextPos{caret_.line, caret_.col - 1}
                                      : TextPos{caret_.line - 1, int(lines_[size_t(caret_.line - 1)].size())};
  replaceRange(from, caret_, {});
  lastEdit_ = EditKind::Deleting;
}

void TextBuffer::deleteForward() {
  if (hasSelection()) {
    eraseSelection();
    return;
  }
  if (caret_ == endPos()) return;
  record(EditKind::Deleting);
  const int len = int(lines_[size_t(caret_.line)].size());
  const TextPos to = caret_.col < len ? TextPos{caret_.line, caret_.col + 1} : TextPos{caret_.line + 1, 0};
  replaceRange(caret_, to, {});
  lastEdit_ = EditKind::Deleting;
}

void TextBuffer::moveLeft(bool extend, bool word) {
  if (hasSelection() && !extend) {
    setCaret(selection().first);
    return;
  }
  TextPos p = caret_;
  if (p.col == 0) {
    if (p.line > 0) p = {p.line - 1, int(lines_[size_t(p.line - 1)].size())};
  } else if (word) {
    const std::string& l = lines_[size_t(p.line)];
    while (p.col > 0 && !isWord(l[size_t(p.col - 1)])) --p.col;
    while (p.col > 0 && isWord(l[size_t(p.col - 1)])) --p.col;
  } else {
    --p.col;
  }
  setCaret(p, extend);
}

void TextBuffer::moveRight(bool extend, bool word) {
  if (hasSelection() && !extend) {
    setCaret(selection().second);
    return;
  }
  TextPos p = caret_;
  const std::string& l = lines_[size_t(p.line)];
  if (p.col >= int(l.size())) {
    if (p.line + 1 < lineCount()) p = {p.line + 1, 0};
  } else if (word) {
    while (p.col < int(l.size()) && !isWord(l[size_t(p.col)])) ++p.col;
    while (p.col < int(l.size()) && isWord(l[size_t(p.col)])) ++p.col;
  } else {
    ++p.col;
  }
  setCaret(p, extend);
}

void TextBuffer::moveUp(bool extend, int lines) {
  const int col = preferredCol_ >= 0 ? preferredCol_ : caret_.col;
  if (caret_.line == 0) setCaret({0, 0}, extend);
  else setCaret({std::max(0, caret_.line - lines), col}, extend);
  preferredCol_ = col;
}

void TextBuffer::moveDown(bool extend, int lines) {
  const int col = preferredCol_ >= 0 ? preferredCol_ : caret_.col;
  if (caret_.line == lineCount() - 1) setCaret(endPos(), extend);
  else setCaret({std::min(lineCount() - 1, caret_.line + lines), col}, extend);
  preferredCol_ = col;
}

void TextBuffer::moveHome(bool extend) {
  const std::string& l = lines_[size_t(caret_.line)];
  int first = 0;
  while (first < int(l.size()) && l[size_t(first)] == ' ') ++first;
  setCaret({caret_.line, caret_.col == first ? 0 : first}, extend);
}

void TextBuffer::moveEnd(bool extend) { setCaret({caret_.line, int(lines_[size_t(caret_.line)].size())}, extend); }

bool TextBuffer::undo() {
  if (undo_.empty()) return false;
  redo_.push_back({lines_, caret_, anchor_});
  Snapshot s = std::move(undo_.back());
  undo_.pop_back();
  lines_ = std::move(s.lines);
  caret_ = s.caret;
  anchor_ = s.anchor;
  lastEdit_ = EditKind::Other;
  changed();
  return true;
}

bool TextBuffer::redo() {
  if (redo_.empty()) return false;
  undo_.push_back({lines_, caret_, anchor_});
  Snapshot s = std::move(redo_.back());
  redo_.pop_back();
  lines_ = std::move(s.lines);
  caret_ = s.caret;
  anchor_ = s.anchor;
  lastEdit_ = EditKind::Other;
  changed();
  return true;
}

}  // namespace litholattice::ui
