#include "litholattice/script/script_binding_allowlist.h"
#include <algorithm>
#include <cctype>
#include "litholattice/core/log.h"
#include "litholattice/mods/mod_interface.h"  // LithoLatticeModPermission bits

namespace litholattice {
namespace {

// The built-in allowlist table. Reviewed against mod_safety.h: deterministic
// engine APIs only — no raw file, network, process, system, wall-clock, or
// unseeded-RNG bindings may ever be added here.
const ScriptBindingEntry kBuiltinBindings[] = {
    // Core (always allowed).
    {"void log(const string &in)", 0, "logging; every script gets this"},
    {"uint engineVersion()", 0, "version queries: safe introspection"},
    {"uint sdkVersion()", 0, "version queries: safe introspection"},
    {"uint protocolVersion()", 0, "version queries: safe introspection"},
    // World / mod API (standard LithoLatticeModApi surface for scripts).
    {"uint64 worldTick()", kPermWorldRead, "deterministic tick counter (no wall-clock)"},
    {"uint16 getBlock(int, int, int)", kPermWorldRead, "world read"},
    {"int setBlock(int, int, int, uint16)", kPermWorldWrite, "world write (engine rate-limits)"},
    {"uint16 registerBlock(const string &in, const string &in)", kPermBlocks, "register blocks/items"},
    {"void registerUiPanel(const string &in, const string &in)", kPermUI, "UI panels"},
    {"void registerInputAction(const string &in, const string &in)", kPermInput, "input actions"},
    {"int loadAsset(const string &in, const string &in)", kPermAssets, "assets via the sandboxed asset pipeline"},
    {"string currentDimension()", kPermWorldRead, "which dimension the player is in"},
    // Standard game UI API (kPermUI).
    {"string ui_text(const string &in)", kPermUI, "read widget text by id"},
    {"void ui_setText(const string &in, const string &in)", kPermUI, "write widget text by id"},
    {"bool ui_checked(const string &in)", kPermUI, "read checkbox state by id"},
    {"void ui_setChecked(const string &in, bool)", kPermUI, "write checkbox state by id"},
    {"float ui_value(const string &in)", kPermUI, "read number field by id"},
    {"void ui_setValue(const string &in, float)", kPermUI, "write number field by id"},
    {"void ui_setVisible(const string &in, bool)", kPermUI, "widget visibility"},
    {"void ui_setEnabled(const string &in, bool)", kPermUI, "widget enabled state"},
    {"bool ui_show(const string &in)", kPermUI, "build a widget tree from JSON"},
    {"void ui_close(const string &in)", kPermUI, "close a widget by id"},
};

// Defense-in-depth heuristic for runtime registrations: reject declarations
// that smell like raw file / network / process / system / clock access.
// The real control is review of every entry; this only catches accidents.
const char* kForbiddenTokens[] = {
    "file", "socket", "network", "http", "tcp", "udp", "system", "exec",
    "popen", "spawn", "fork", "process", "shell", "os_", "env", "clock",
    "random", "dll", "library",
};

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool containsForbiddenToken(const std::string& decl) {
  const std::string d = lower(decl);
  for (const char* t : kForbiddenTokens)
    if (d.find(t) != std::string::npos) return true;
  return false;
}

std::vector<ScriptBindingEntry>& table() {
  static std::vector<ScriptBindingEntry> t;
  return t;
}
bool& builtinsDone() {
  static bool done = false;
  return done;
}

}  // namespace

void ScriptBindingAllowlist::registerBuiltins() {
  if (builtinsDone()) return;
  builtinsDone() = true;
  for (const ScriptBindingEntry& e : kBuiltinBindings) table().push_back(e);
}

bool ScriptBindingAllowlist::registerBinding(const ScriptBindingEntry& entry) {
  if (isAllowlisted(entry.decl)) {
    logError("[allowlist] refused duplicate binding: %s", entry.decl);
    return false;
  }
  if (containsForbiddenToken(entry.decl)) {
    logError("[allowlist] refused binding '%s': forbidden token (no raw fs/network/process/system)",
             entry.decl);
    return false;
  }
  table().push_back(entry);
  return true;
}

bool ScriptBindingAllowlist::isAllowlisted(const std::string& decl) {
  for (const ScriptBindingEntry& e : table())
    if (decl == e.decl) return true;
  return false;
}

uint32_t ScriptBindingAllowlist::requiredPermissions(const std::string& decl) {
  for (const ScriptBindingEntry& e : table())
    if (decl == e.decl) return e.requiredPermissions;
  return 0;
}

const std::vector<ScriptBindingEntry>& ScriptBindingAllowlist::entries() { return table(); }

}  // namespace litholattice
