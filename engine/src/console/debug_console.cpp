// DebugConsole implementation.
#include "litholattice/console/debug_console.h"

#include "litholattice/core/log.h"
#include "litholattice/ui/containers.h"
#include "litholattice/ui/text_edit.h"

#include <algorithm>
#include <cstring>

namespace litholattice::console {
namespace {

const char* levelTag(LogLevel l) {
  switch (l) {
    case LogLevel::Info: return "";
    case LogLevel::Warn: return "WARN ";
    case LogLevel::Error: return "ERROR ";
  }
  return "";
}

std::string truncate(std::string s, size_t maxLen) {
  if (s.size() > maxLen) s = s.substr(0, maxLen - 3) + "...";
  return s;
}

}  // namespace

DebugConsole::DebugConsole(ConsoleRegistry& registry)
    : registry_(registry), completionSource_(registry), completer_(completionSource_) {
  lastToggle_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
}

void DebugConsole::attach(ui::UiRoot& ui) {
  ui_ = &ui;
  auto panel = std::make_unique<ui::StackPanel>(ui::Orientation::Vertical, 2, "console");
  // Top-anchored dropdown: full width, top 42% of the viewport.
  ui::Layout layout;
  layout.anchorMin = {0, 0};
  layout.anchorMax = {1, kHeightFraction};
  layout.insets = {4, 4, 4, 0};  // left, top, right, bottom
  panel->setLayout(layout);
  auto& stack = *panel;

  auto& logView = stack.add<ui::TextEditor>();
  logView.setReadOnly(true);
  logView.setLineNumbers(false);
  ui::Layout logLayout = ui::Layout::sized({0, 0}, 1);  // flex: takes the leftover height
  logView.setLayout(logLayout);
  logView_ = &logView;

  // VS Code-style autocomplete dropdown: lives ABOVE the input, hidden until
  // there is something to show. A plain ui::ListView (composition, no new
  // widgets); rows are formatted "kind  name  detail" strings.
  auto& completion = stack.add<ui::ListView>();
  completion.setLayout(ui::Layout::sized({0, 0}));
  completion.setVisible(false);
  completion.on("activate", [this](ui::Widget&) { acceptCompletion(); });
  completionView_ = &completion;

  auto& input = stack.add<ui::TextBox>();
  input.setPlaceholder("type a lithoscript command (? for help)");
  input.setLayout(ui::Layout::sized({0, 28}));
  input.on("submit", [this](ui::Widget&) { onSubmit(); });
  input.on("change", [this](ui::Widget&) { refreshCompletion(); });
  input_ = &input;

  panel->setVisible(false);
  panel_ = &ui.root().addChild(std::move(panel));

  // Console output goes to the log view (and stays out of the engine log:
  // echoing it there would recurse through the tap).
  registry_.setOutput([this](const std::string& line) { appendLogLine(line); });
  // The engine's own log lines stream into the view via update().
  registry_.registerBuiltins(
#ifdef NDEBUG
      false
#else
      true
#endif
  );
  registry_.registerCommand("toggleconsole", "toggleconsole: opens/closes the debug console",
                            [this](ConsoleRegistry&, const ConsoleArgs&) { toggle(); });
  registry_.registerCommand("clear", "clear: clears the console log view",
                            [this](ConsoleRegistry&, const ConsoleArgs&) {
                              if (logView_) logView_->buffer().setText("");
                            });
}

void DebugConsole::open() {
  if (open_ || !ui_) return;
  open_ = true;
  panel_->setVisible(true);
  ui_->setFocus(input_);
  historyPos_ = -1;
  pendingInput_.clear();
}

void DebugConsole::close() {
  if (!open_ || !ui_) return;
  open_ = false;
  hideCompletion();
  panel_->setVisible(false);
  if (ui_->focus() == input_) ui_->setFocus(nullptr);
}

void DebugConsole::toggle() {
  if (open_) close();
  else open();
}

bool DebugConsole::handleTildeKey(ui::UiRoot& ui, const ui::UiEvent& e) {
  if (e.type != ui::UiEvent::Type::Text) return false;
  if (e.codepoint != U'~' && e.codepoint != U'`') return false;
  const auto now = std::chrono::steady_clock::now();
  const bool debounced = now - lastToggle_ < std::chrono::milliseconds(200);
  if (open_) {
    if (debounced) return true;  // key repeat: swallow
    lastToggle_ = now;
    close();
    return true;
  }
  // Closed: never steal the tilde from a focused text field / modal / popup —
  // this check comes BEFORE the debounce so typing "~" always keeps working.
  if (ui.wantsKeyboard()) return false;
  if (debounced) return true;  // key repeat right after a toggle: swallow
  lastToggle_ = now;
  open();
  return true;
}

bool DebugConsole::handleKeyPreFilter(const ui::UiEvent& e) {
  if (!open_ || e.type != ui::UiEvent::Type::KeyDown) return false;
  if (e.mods & (ui::kModCtrl | ui::kModAlt)) return false;
  // Tab: HL1-style completion (consumed before TextBox can insert spaces).
  if (e.key == ui::Key::Tab) {
    tabComplete();
    return true;
  }
  // While the autocomplete dropdown is visible it owns these keys.
  if (dropdownVisible()) {
    switch (e.key) {
      case ui::Key::Escape:
        hideCompletion();  // dismiss the dropdown; the console stays open
        return true;
      case ui::Key::Enter:
        acceptCompletion();
        return true;
      case ui::Key::Up:
        moveCompletionSelection(-1);
        return true;
      case ui::Key::Down:
        moveCompletionSelection(1);
        return true;
      default:
        break;
    }
  }
  switch (e.key) {
    case ui::Key::Escape:
      close();
      return true;
    case ui::Key::Up:
    case ui::Key::Down: {
      if (history_.empty()) return true;
      if (historyPos_ == -1) {
        pendingInput_ = input_->text();
        historyPos_ = int(history_.size()) - 1;
      } else if (e.key == ui::Key::Up) {
        historyPos_ = std::max(0, historyPos_ - 1);
      } else {
        ++historyPos_;
        if (historyPos_ >= int(history_.size())) {
          historyPos_ = -1;  // back past the newest: restore the pending line
          input_->setText(pendingInput_);
          return true;
        }
      }
      input_->setText(history_[size_t(historyPos_)]);
      return true;
    }
    case ui::Key::PageUp:
      if (logView_) logView_->scrollToLine(std::max(0, logView_->firstVisibleLine() - logView_->visibleLines()));
      return true;
    case ui::Key::PageDown:
      if (logView_) logView_->scrollToLine(logView_->firstVisibleLine() + logView_->visibleLines());
      return true;
    default:
      return false;
  }
}

void DebugConsole::update() {
  if (!ui_) return;
  const std::vector<LogRecord> records = engineLogTap().snapshot();
  bool added = false;
  for (const LogRecord& rec : records) {
    if (rec.seq <= tapSeqSeen_) continue;
    tapSeqSeen_ = rec.seq;
    appendLogLine("[" + rec.timestamp + "] " + levelTag(rec.level) + rec.text);
    added = true;
  }
  if (added) trimLogView();
}

void DebugConsole::onSubmit() {
  const std::string line = input_->text();
  hideCompletion();
  input_->setText("");
  historyPos_ = -1;
  pendingInput_.clear();
  if (line.empty()) return;
  if (history_.empty() || history_.back() != line) {
    history_.push_back(line);
    if (history_.size() > kMaxHistory) history_.erase(history_.begin());
  }
  appendLogLine("] " + line);  // Quake-style echo of the submitted line
  registry_.executeLine(line);
  if (logView_) logView_->scrollToLine(logView_->buffer().lineCount());
  trimLogView();
}

void DebugConsole::appendLogLine(const std::string& line) {
  if (!logView_) return;
  ui::TextBuffer& buf = logView_->buffer();
  const bool atEnd = buf.caret() == buf.endPos();
  buf.moveDocEnd(false);
  if (buf.lineCount() > 1 || !buf.line(0).empty()) buf.insert("\n");
  buf.insert(line);
  if (atEnd) {
    logView_->scrollToLine(buf.lineCount());
  }
}

void DebugConsole::trimLogView() {
  if (!logView_) return;
  ui::TextBuffer& buf = logView_->buffer();
  const int over = buf.lineCount() - int(kMaxViewLines);
  if (over > 0) {
    buf.select({0, 0}, {over, 0});
    buf.eraseSelection();
  }
}

std::string DebugConsole::inputText() const { return input_ ? input_->text() : std::string(); }

void DebugConsole::setInputTextForTest(const std::string& text) {
  if (input_) {
    input_->setText(text);
    refreshCompletion();
  }
}

// ---- autocomplete ----

void DebugConsole::refreshCompletion() {
  if (!open_ || !input_ || !completionView_) {
    hideCompletion();
    return;
  }
  completionRows_ = completer_.filter(input_->text());
  if (completionRows_.size() > kMaxCompletionRows) completionRows_.resize(kMaxCompletionRows);
  if (completionRows_.empty()) {
    hideCompletion();
    return;
  }
  showCompletion();
}

std::string DebugConsole::formatCompletionRow(const CompletionCandidate& c, size_t nameWidth) {
  const char* tag = completionKindTag(c.kind);
  std::string row = tag + std::string(6 - std::strlen(tag), ' ') + c.name +
                    std::string(nameWidth + 2 - c.name.size(), ' ');
  if (c.kind == CompletionKind::Cvar && !c.detail.empty()) {
    row += "= " + c.detail;
    if (!c.description.empty()) row += "  " + truncate(c.description, 40);
  } else if (!c.description.empty()) {
    row += truncate(c.description, 48);
  }
  return row;
}

void DebugConsole::showCompletion() {
  size_t nameWidth = 0;
  for (const auto& c : completionRows_) nameWidth = std::max(nameWidth, c.name.size());
  std::vector<std::string> items;
  items.reserve(completionRows_.size());
  for (const auto& c : completionRows_) items.push_back(formatCompletionRow(c, nameWidth));
  completionView_->setItems(std::move(items));
  completionView_->setSelected(0);
  const float h = float(std::min(completionRows_.size(), kMaxCompletionVisible)) *
                      completionView_->rowHeight() +
                  8.0f;
  completionView_->setLayout(ui::Layout::sized({0, h}));
  completionView_->setVisible(true);
}

void DebugConsole::hideCompletion() {
  completionRows_.clear();
  if (completionView_) {
    completionView_->setVisible(false);
    completionView_->setSelected(-1);
  }
}

void DebugConsole::tabComplete() {
  if (!input_) return;
  const CompletionResult r = completer_.complete(input_->text());
  if (r.matches.empty()) return;  // no matches: no-op
  std::string line = r.completedLine;
  if (r.matches.size() == 1) line += " ";  // unique match: done, add the trailing space
  input_->setText(line);
  if (r.matches.size() > 1) {
    // HL1 lists the matches in the console.
    std::string names;
    for (const auto& m : r.matches) {
      if (!names.empty()) names += ", ";
      names += m.name;
    }
    appendLogLine(names);
    trimLogView();
  }
  refreshCompletion();
}

void DebugConsole::acceptCompletion() {
  if (!dropdownVisible() || completionRows_.empty() || !input_) return;
  int sel = completionView_->selected();
  if (sel < 0 || size_t(sel) >= completionRows_.size()) sel = 0;
  const CompletionCandidate& c = completionRows_[size_t(sel)];
  const std::string line = input_->text();
  const CompletionSplit s = Completer::splitCompletionLine(line);
  std::string next;
  if (!s.head.empty() && s.hasSpace && Completer::isContextCommand(s.head)) {
    next = std::string(s.head) + " " + c.name;  // completing the `exec`/`map` argument
  } else if (!s.head.empty()) {
    const size_t headPos = size_t(s.head.data() - line.data());
    next = std::string(line.substr(0, headPos)) + c.name +
           std::string(line.substr(headPos + s.head.size()));
  } else {
    next = c.name;
  }
  input_->setText(next + " ");
  hideCompletion();
}

void DebugConsole::moveCompletionSelection(int delta) {
  if (!dropdownVisible() || completionRows_.empty()) return;
  int sel = completionView_->selected();
  if (sel < 0) sel = 0;
  sel = std::clamp(sel + delta, 0, int(completionRows_.size()) - 1);
  completionView_->setSelected(sel);
}

size_t DebugConsole::logLineCount() const { return logView_ ? size_t(logView_->buffer().lineCount()) : 0; }

void DebugConsole::submitForTest(const std::string& line) {
  if (input_) input_->setText(line);
  onSubmit();
}

}  // namespace litholattice::console
