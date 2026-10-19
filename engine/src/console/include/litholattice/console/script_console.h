// AngelScript exposure of lithoscript (exposure #3).
//
// Scripts get three functions, all allowlist-gated on kPermConsole — a script
// whose mod was not granted the "console" permission cannot bind them at all
// (ScriptEngine::bindGlobalFunction refuses), so there is no unpermitted path:
//
//   void   consoleCmd(const string &in line)            -- execute a console line
//   string consoleCvar(const string &in name)           -- read a cvar ("" when missing)
//   void   consoleSetCvar(const string &in name, const string &in value)
//
// `registerConsoleScriptBindings()` adds those declarations to the
// ScriptBindingAllowlist (idempotent; call once at startup). Afterwards the
// game (or a mod host) calls bindConsoleToScripts() per ScriptEngine — the
// per-mod permission check happens inside bindGlobalFunction. Keep the
// returned ConsoleScriptContext alive as long as the ScriptEngine.
#pragma once

#include <memory>
#include <string>

#include "litholattice/console/concmd.h"
#include "litholattice/script/script_engine.h"

namespace litholattice::console {

/// Opaque per-binding context (registry + mod attribution for log lines).
struct ConsoleScriptContext;

/// Adds the console decls to the script binding allowlist. Idempotent.
void registerConsoleScriptBindings();

/// Binds consoleCmd / consoleCvar / consoleSetCvar into `se` against
/// `registry`. Returns the context the bindings close over (null on
/// failure); the caller keeps it alive for the ScriptEngine's lifetime.
/// False/null when the allowlist or the mod's permissions refuse.
std::shared_ptr<ConsoleScriptContext> bindConsoleToScripts(ScriptEngine& se,
                                                           ConsoleRegistry& registry,
                                                           const std::string& modId);

}  // namespace litholattice::console
