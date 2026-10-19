// AngelScript console bindings: permission-gated through the allowlist.
#include "litholattice/console/script_console.h"

#include <angelscript.h>

#include "litholattice/core/log.h"
#include "litholattice/mods/mod_interface.h"  // kPermConsole

namespace litholattice::console {

struct ConsoleScriptContext {
  ConsoleRegistry* registry = nullptr;
  std::string modId;
  /// The owning mod's granted permission mask, captured at bind time. Every
  /// binding installs it for the call duration (mirroring the MCP
  /// IConsoleExec adapter contract): mod-issued lines run with the mod's
  /// granted permissions, never ambient authority. A restricted mask revokes
  /// cheat authority for cheat-gated commands/cvars even when the registry's
  /// cheatsAllowed() is true (fail-closed: it can only narrow, never widen).
  uint32_t permissionMask = 0;
};

namespace {

// Installs the owning mod's mask for the duration of one binding call and
// restores the ambient mask afterwards (re-entrant: bindings may nest).
class ScopedCallerPermissions {
 public:
  ScopedCallerPermissions(ConsoleRegistry* reg, uint32_t mask)
      : reg_(reg), saved_(reg ? reg->callerPermissions() : kCallerFullAuthority) {
    if (reg_) reg_->setCallerPermissions(mask);
  }
  ~ScopedCallerPermissions() {
    if (reg_) reg_->setCallerPermissions(saved_);
  }
  ScopedCallerPermissions(const ScopedCallerPermissions&) = delete;
  ScopedCallerPermissions& operator=(const ScopedCallerPermissions&) = delete;

 private:
  ConsoleRegistry* reg_;
  uint32_t saved_;
};

void scriptConsoleCmd(const std::string& line, ConsoleScriptContext* ctx) {
  if (ctx && ctx->registry) {
    ScopedCallerPermissions scope(ctx->registry, ctx->permissionMask);
    ctx->registry->executeLine(line);
  }
}

std::string scriptConsoleCvar(const std::string& name, ConsoleScriptContext* ctx) {
  if (ctx && ctx->registry) {
    ScopedCallerPermissions scope(ctx->registry, ctx->permissionMask);
    if (const CVar* cv = ctx->registry->findCVar(name)) return cv->value;
    logWarn("console: script mod '%s' read unknown cvar '%s'", ctx->modId.c_str(), name.c_str());
  }
  return {};
}

void scriptConsoleSetCvar(const std::string& name, const std::string& value, ConsoleScriptContext* ctx) {
  if (!ctx || !ctx->registry) return;
  ScopedCallerPermissions scope(ctx->registry, ctx->permissionMask);
  std::string err;
  if (!ctx->registry->setCVar(name, value, &err))
    logWarn("console: script mod '%s': %s", ctx->modId.c_str(), err.c_str());
}

struct BindingDecl {
  const char* decl;
  asSFuncPtr ptr;
  const char* description;
};

const BindingDecl kConsoleBindings[] = {
    {"void consoleCmd(const string &in)", asFUNCTION(scriptConsoleCmd), "execute a console line"},
    {"string consoleCvar(const string &in)", asFUNCTION(scriptConsoleCvar), "read a cvar value"},
    {"void consoleSetCvar(const string &in, const string &in)", asFUNCTION(scriptConsoleSetCvar),
     "write a cvar value"},
};

}  // namespace

void registerConsoleScriptBindings() {
  ScriptBindingAllowlist::registerBuiltins();
  for (const BindingDecl& b : kConsoleBindings)
    ScriptBindingAllowlist::registerBinding({b.decl, uint32_t(kPermConsole), b.description});
}

std::shared_ptr<ConsoleScriptContext> bindConsoleToScripts(ScriptEngine& se, ConsoleRegistry& registry,
                                                           const std::string& modId) {
  auto ctx = std::make_shared<ConsoleScriptContext>();
  ctx->registry = &registry;
  ctx->modId = modId;
  ctx->permissionMask = se.modPermissions();  // the mod's own authority, not ambient
  for (const BindingDecl& b : kConsoleBindings) {
    if (se.bindGlobalFunction(b.decl, b.ptr, asCALL_CDECL_OBJLAST, ctx.get()) < 0) {
      logWarn("console: refusing '%s' for mod '%s' (allowlist or missing kPermConsole)", b.decl,
              modId.c_str());
      return nullptr;
    }
  }
  return ctx;
}

}  // namespace litholattice::console
