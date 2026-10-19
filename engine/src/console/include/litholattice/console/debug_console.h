// DebugConsole: the Half-Life-style dropdown console for lithoscript. Tilde
// drops it from the top of the screen; it renders through the ui framework (a StackPanel holding
// a read-only TextEditor log view over a TextBox command line — composition,
// no new widgets).
//
// INPUT CONFINEMENT (hard rule): the tilde key never toggles the console open
// while UiRoot::wantsKeyboard() (a text field has focus, a modal/popup is
// open) — typing "~" in a field keeps working. While the console is open its
// TextBox holds focus, so wantsKeyboard() is true and keystrokes typed here
// never reach mods (ModInputRouter::shouldDeliverToMod), exactly like any
// other text field. Tilde while open closes it (HL1 behavior).
//
// GAME WIRING (one call site, see Game::routeUiInput):
//   for (const ui::UiEvent& e : uiEvents) {
//     if (debugConsole.handleTildeKey(ui.root(), e)) continue;   // tilde toggle
//     if (debugConsole.isOpen() && debugConsole.handleKeyPreFilter(e)) continue;
//     ... ui_.root().handleEvent(e) ...
//   }
// plus debugConsole.update() once per frame.
#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "litholattice/console/completer.h"
#include "litholattice/console/concmd.h"
#include "litholattice/ui/lists.h"
#include "litholattice/ui/text_edit.h"
#include "litholattice/ui/ui_root.h"

namespace litholattice::console {

class DebugConsole {
 public:
  explicit DebugConsole(ConsoleRegistry& registry);
  ~DebugConsole() = default;
  DebugConsole(const DebugConsole&) = delete;
  DebugConsole& operator=(const DebugConsole&) = delete;

  /// Builds the panel (hidden) and adds it on top of ui.root(). Call once,
  /// after the UiRoot exists. Available in ALL builds, not just debug.
  void attach(ui::UiRoot& ui);

  bool isOpen() const { return open_; }
  void open();
  void close();
  void toggle();

  /// Tilde handling for the game loop. Consumes UiEvent::Text('~') and
  /// UiEvent::Text('`') (the grave-accent key with/without shift): opens the
  /// console when closed and closes it when open. Never opens while
  /// ui.wantsKeyboard() — that check runs before the key-repeat debounce, so
  /// typing "~" in a field always keeps working. Debounced: a held tilde key
  /// (text repeat) toggles at most once per 200 ms. Returns true when the
  /// event was consumed.
  bool handleTildeKey(ui::UiRoot& ui, const ui::UiEvent& e);

  /// Key pre-filter while open; call BEFORE ui.handleEvent. Tab performs
  /// HL1-style completion of the input line; typing filters the VS Code-style
  /// autocomplete dropdown (a ListView above the input). While the dropdown
  /// is visible Up/Down move its selection, Enter accepts it, Escape
  /// dismisses it (the console stays open); otherwise Up/Down walk command
  /// history, PageUp/PageDown scroll the log view, Escape closes.
  /// Returns true when the event was consumed.
  bool handleKeyPreFilter(const ui::UiEvent& e);

  /// Pulls new engine-log tap records into the log view (call once per frame).
  void update();

  // ---- test seams ----
  size_t historySize() const { return history_.size(); }
  std::string inputText() const;
  size_t logLineCount() const;
  /// Feeds a line as if submitted from the input box (history + dispatch).
  void submitForTest(const std::string& line);
  /// Autocomplete dropdown state (headless-verifiable).
  bool completionVisible() const { return completionView_ && completionView_->visible(); }
  size_t completionRowCount() const { return completionRows_.size(); }
  int completionSelected() const { return completionView_ ? completionView_->selected() : -1; }
  /// Sets the input text as if typed (fires the completion refresh).
  void setInputTextForTest(const std::string& text);

 private:
  void onSubmit();
  void appendLogLine(const std::string& line);
  void trimLogView();

  // ---- autocomplete (shares the registry-agnostic Completer unit) ----
  void refreshCompletion();
  void showCompletion();
  void hideCompletion();
  bool dropdownVisible() const { return completionVisible(); }
  void tabComplete();
  void acceptCompletion();
  void moveCompletionSelection(int delta);
  static std::string formatCompletionRow(const CompletionCandidate& c, size_t nameWidth);

  ConsoleRegistry& registry_;
  RegistryCompletionSource completionSource_;  ///< injected candidate source (the Fragment seam)
  Completer completer_;
  ui::UiRoot* ui_ = nullptr;
  ui::Widget* panel_ = nullptr;
  ui::TextEditor* logView_ = nullptr;
  ui::ListView* completionView_ = nullptr;
  ui::TextBox* input_ = nullptr;
  std::vector<CompletionCandidate> completionRows_;

  bool open_ = false;
  std::vector<std::string> history_;
  int historyPos_ = -1;  ///< -1 = not browsing (at the fresh line)
  std::string pendingInput_;  ///< input stashed while browsing history
  uint64_t tapSeqSeen_ = 0;
  std::chrono::steady_clock::time_point lastToggle_{};

  static constexpr size_t kMaxHistory = 64;
  static constexpr size_t kMaxViewLines = 512;  ///< matches the log tap capacity
  static constexpr float kHeightFraction = 0.42f;
  static constexpr size_t kMaxCompletionRows = 24;    ///< dropdown lists at most this many rows
  static constexpr size_t kMaxCompletionVisible = 8;  ///< rows shown before the list scrolls
};

}  // namespace litholattice::console
