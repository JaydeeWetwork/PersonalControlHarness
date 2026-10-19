// Debug in-game script editor (F10): a modal dialog with a menu bar, the scripts/*.as files on
// the left and a TextEditor on the right. Save (Ctrl+S) writes the file and hot-reloads all
// scripts through the ScriptEngine (on a compile error the previous scripts stay active and the
// caret jumps to the first error). Only files listed from the scripts folder can be opened or
// written: the editor never takes a path from the user.
#pragma once
#include "lithokraft/game/engine.h"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "litholattice/script/script_engine.h"
#include "litholattice/ui/ui_root.h"

namespace litholattice::ui {
class Dialog;
class Label;
class ListView;
class TextEditor;
}  // namespace litholattice::ui

namespace lithokraft {

class ScriptEditor {
 public:
  /// `afterReload` runs after a successful hot reload (e.g. re-run the onLoad hook).
  ScriptEditor(ui::UiRoot& ui, ScriptEngine& scripts, std::filesystem::path dir, std::function<void()> afterReload = {});
  void toggle();
  void open();
  /// Closes (asks first when there are unsaved changes, unless `force`).
  void close(bool force = false);
  bool isOpen() const { return dialog_ != nullptr; }

  // For tests / tools.
  bool openFile(const std::string& name);
  bool save();
  bool modified() const;
  const std::string& currentFile() const { return file_; }
  const std::string& status() const { return status_; }
  ui::TextEditor* editor() const { return editor_; }

 private:
  void refreshFiles();
  /// Runs `action` now, or after the user confirms discarding unsaved changes.
  void confirmDiscard(std::function<void()> action);
  void updateStatus();
  void setStatus(std::string s);

  ui::UiRoot& ui_;
  ScriptEngine& scripts_;
  std::filesystem::path dir_;
  std::function<void()> afterReload_;
  std::vector<std::string> files_;
  std::string file_, status_;
  uint64_t savedRevision_ = 0;
  ui::Dialog* dialog_ = nullptr;
  ui::ListView* list_ = nullptr;
  ui::TextEditor* editor_ = nullptr;
  ui::Label* statusLabel_ = nullptr;
};

}  // namespace lithokraft
