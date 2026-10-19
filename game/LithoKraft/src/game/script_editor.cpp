#include "lithokraft/game/script_editor.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include "litholattice/core/log.h"
#include "litholattice/ui/containers.h"
#include "litholattice/ui/dialog.h"
#include "litholattice/ui/lists.h"
#include "litholattice/ui/menus.h"
#include "litholattice/ui/text_edit.h"

namespace lithokraft {
namespace {

/// Line of the first "file (row,col): message" error for `file`, 0-based; -1 if none.
int firstErrorLine(const std::vector<std::string>& errors, const std::string& file) {
  for (const std::string& e : errors) {
    if (e.rfind(file + " (", 0) != 0) continue;
    const size_t open = file.size() + 2;
    return std::max(0, std::atoi(e.c_str() + open) - 1);
  }
  return -1;
}

}  // namespace

ScriptEditor::ScriptEditor(ui::UiRoot& ui, ScriptEngine& scripts, std::filesystem::path dir, std::function<void()> afterReload)
    : ui_(ui), scripts_(scripts), dir_(std::move(dir)), afterReload_(std::move(afterReload)) {}

void ScriptEditor::toggle() {
  if (isOpen()) close();
  else open();
}

void ScriptEditor::refreshFiles() {
  namespace fs = std::filesystem;
  files_.clear();
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir_, ec))
    if (e.is_regular_file() && e.path().extension() == ".as") files_.push_back(e.path().filename().string());
  std::sort(files_.begin(), files_.end());
  if (list_) {
    list_->setItems(files_);
    const auto it = std::find(files_.begin(), files_.end(), file_);
    list_->setSelected(it == files_.end() ? -1 : int(it - files_.begin()));
  }
}

void ScriptEditor::open() {
  if (isOpen()) return;
  ui::Dialog& dlg = ui::Dialog::show(ui_, "Script editor (debug) - " + dir_.generic_string());
  dlg.setId("script-editor");
  // Most of the screen, wherever it is.
  dlg.layout() = ui::Layout::fill({40, 30, 40, 30});
  dialog_ = &dlg;
  dlg.setCloseGuard([this] {
    if (!modified()) return true;
    confirmDiscard([this] { close(true); });
    return false;
  });

  auto& bar = dlg.body().add<ui::MenuBar>();
  ui::MenuItem save;
  save.label = "Save + reload";
  save.shortcut = "Ctrl+S";
  save.action = [this] { this->save(); };
  ui::MenuItem reload;
  reload.label = "Reload from disk";
  reload.action = [this] { confirmDiscard([this] { openFile(file_); }); };
  ui::MenuItem closeItem;
  closeItem.label = "Close";
  closeItem.shortcut = "Esc";
  closeItem.action = [this] { close(); };
  bar.addMenu("File", {save, reload, ui::MenuItem::separatorItem(), closeItem});

  auto& split = dlg.body().add<ui::Splitter>(ui::Orientation::Horizontal, 0.2f);
  split.layout().flex = 1;
  auto list = std::make_unique<ui::ListView>();
  list_ = list.get();
  split.setPane(0, std::move(list));
  auto editor = std::make_unique<ui::TextEditor>();
  editor_ = editor.get();
  split.setPane(1, std::move(editor));
  statusLabel_ = &dlg.body().add<ui::Label>();

  list_->on("select", [this](ui::Widget&) {
    const std::string* name = list_->selectedItem();
    if (!name || *name == file_) return;
    const std::string next = *name;
    confirmDiscard([this, next] { openFile(next); });
    refreshFiles();  // keeps the current file selected until the switch happens
  });
  editor_->on("change", [this](ui::Widget&) { updateStatus(); });
  editor_->on("save", [this](ui::Widget&) { this->save(); });
  dlg.on("close", [this](ui::Widget&) {
    dialog_ = nullptr;
    list_ = nullptr;
    editor_ = nullptr;
    statusLabel_ = nullptr;
  });

  refreshFiles();
  if (file_.empty() && !files_.empty()) file_ = files_.front();
  if (!file_.empty()) openFile(file_);
  else setStatus("no .as files in " + dir_.generic_string());
  editor_->focus();
}

void ScriptEditor::close(bool force) {
  if (!dialog_) return;
  if (!force && modified()) {
    confirmDiscard([this] { close(true); });
    return;
  }
  dialog_->close();  // "close" handler clears the pointers
}

bool ScriptEditor::modified() const { return editor_ && editor_->buffer().revision() != savedRevision_; }

bool ScriptEditor::openFile(const std::string& name) {
  if (std::find(files_.begin(), files_.end(), name) == files_.end()) return false;  // listed files only
  std::ifstream in(dir_ / name, std::ios::binary);
  if (!in) {
    setStatus("cannot read " + name);
    return false;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  file_ = name;
  if (editor_) {
    editor_->setText(ss.str());
    savedRevision_ = editor_->buffer().revision();
  }
  refreshFiles();
  setStatus("opened " + name);
  return true;
}

bool ScriptEditor::save() {
  if (!editor_ || file_.empty()) return false;
  {
    std::ofstream out(dir_ / file_, std::ios::binary | std::ios::trunc);
    const std::string text = editor_->text();
    out.write(text.data(), std::streamsize(text.size()));
    if (!out) {
      setStatus("write failed: " + file_);
      return false;
    }
  }
  savedRevision_ = editor_->buffer().revision();
  logInfo("script editor: saved %s, hot reloading scripts", file_.c_str());
  if (scripts_.reload()) {
    if (afterReload_) afterReload_();
    setStatus("saved " + file_ + " - scripts reloaded OK");
    return true;
  }
  const auto& errors = scripts_.buildErrors();
  setStatus("saved " + file_ + " - compile failed (previous scripts still active): " +
            (errors.empty() ? std::string("see log") : errors.front()));
  if (const int line = firstErrorLine(errors, file_); line >= 0) {
    editor_->buffer().setCaret({line, 0});
    editor_->scrollToLine(line);
  }
  return false;
}

void ScriptEditor::confirmDiscard(std::function<void()> action) {
  if (!modified()) {
    action();
    return;
  }
  ui::Dialog& ask = ui::Dialog::show(ui_, "Unsaved changes");
  ask.body().add<ui::Label>(file_ + " has unsaved changes. Discard them?");
  ask.addButton("Discard", std::move(action));
  ask.addButton("Cancel");
}

void ScriptEditor::updateStatus() {
  if (!editor_) return;
  const ui::TextPos c = editor_->buffer().caret();
  setStatus(file_ + (modified() ? " *" : "") + "  line " + std::to_string(c.line + 1) + ", col " + std::to_string(c.col + 1) +
            "  (Ctrl+S: save + hot reload, Esc: close)");
}

void ScriptEditor::setStatus(std::string s) {
  status_ = std::move(s);
  if (statusLabel_) statusLabel_->setText(status_);
}

}  // namespace lithokraft
