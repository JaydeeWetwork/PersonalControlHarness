// AngelScript bindings for the UI: widget events call script functions (ui::IScriptHost), and
// scripts get a small, sandboxed API over widgets by id:
//   string ui_text(const string &in id)            void ui_setText(const string &in id, const string &in text)
//   bool ui_checked(const string &in id)           void ui_setChecked(const string &in id, bool on)
//   float ui_value(const string &in id)            void ui_setValue(const string &in id, float v)
//   void ui_setVisible(const string &in id, bool)  void ui_setEnabled(const string &in id, bool)
//   bool ui_show(const string &in layoutJson)      void ui_close(const string &in id)
// ui_show builds a widget tree from JSON (see docs/UI.md) and adds it to the screen; its "on"
// handlers name script functions of the form `void f(const string &in widgetId)`.
#pragma once
#include "lithokraft/game/engine.h"
#include <string_view>
#include "litholattice/script/script_engine.h"
#include "litholattice/ui/ui_root.h"

namespace lithokraft {

class UiScriptBindings final : public ui::IScriptHost {
 public:
  /// `scripts` and `ui` must outlive this object. Call registerApi() after scripts.init() and
  /// before loading script files. The caller must first set the mod's permissions on the
  /// ScriptEngine (the ui_* bindings require kPermUI); anything unlisted is refused.
  UiScriptBindings(ScriptEngine& scripts, ui::UiRoot& ui) : scripts_(scripts), ui_(ui) {}
  bool registerApi();
  bool callHandler(std::string_view functionDecl, std::string_view widgetId) override;
  ui::UiRoot& ui() { return ui_; }

 private:
  ScriptEngine& scripts_;
  ui::UiRoot& ui_;
};

}  // namespace lithokraft
