#include "litholattice/script/script_engine.h"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>
#include <algorithm>
#include "scriptstdstring/scriptstdstring.h"
#include "litholattice/core/log.h"
#include "script_memory_internal.h"

namespace litholattice {
namespace {

struct Budget {
  std::chrono::steady_clock::time_point deadline;
  unsigned lines = 0;
};
// Called by AngelScript for every executed statement: read the clock only every 256 lines.
void lineCallback(asIScriptContext* ctx, Budget* b) {
  if ((++b->lines & 255u) == 0 && std::chrono::steady_clock::now() > b->deadline) ctx->Abort();  // runaway script
}

// Header prepended to every AngelScript allocation: records the size and
// the owning mod's quota account for accounting. 32 bytes, so the payload
// stays 16-aligned (malloc returns 16-aligned storage on this platform).
//
// Review F-M2: the header holds a shared_ptr<QuotaAccount> — exactly like
// the operator-new hook's QuotaHeader — NEVER a raw ScriptEngine*. The old
// raw owner pointer was dereferenced by freeShim unconditionally, so an AS
// allocation retained past ScriptEngine destruction (a ref-counted script
// object/string held by a binding) freed into dead engine memory.
// Review F-M1: this account is also the SINGLE quota counter (see
// script_engine.h): the shims charge/credit it directly, and no second
// per-engine counter exists for the operator-new path to overwrite.
struct AllocHeader {
  size_t n;
  std::shared_ptr<detail::QuotaAccount> account;  // null outside a mod's hook execution
  size_t reserved = 0;  // pads the header to 32 bytes (payload alignment)
};
static_assert(sizeof(AllocHeader) % 16 == 0, "AllocHeader must keep the payload 16-aligned");

}  // namespace

void* ScriptEngine::allocShim(size_t n) {
  void* raw = std::malloc(sizeof(AllocHeader) + n);
  if (!raw) return nullptr;
  auto* h = static_cast<AllocHeader*>(raw);
  h->n = n;
  // The header sits in raw malloc'd memory: the shared_ptr must be
  // placement-new constructed (assigning to unconstructed storage is UB),
  // mirroring the operator-new hook's QuotaHeader.
  ScriptEngine* owner = tlsScriptAllocOwner;
  new (&h->account) std::shared_ptr<detail::QuotaAccount>(owner ? owner->quotaAccount() : nullptr);
  h->reserved = 0;
  if (h->account) {
    // Charge the ONE counter (F-M1). `owner` is dereferenced only here, at
    // allocation time, while the engine is provably alive (it is executing
    // this mod's hook on this thread) — never at free time (F-M2).
    h->account->bytes += static_cast<int64_t>(n);
    if (h->account->bytes > static_cast<int64_t>(owner->memoryQuotaBytes_) &&
        owner->quotaState_ != QuotaState::kSuspended) {
      // The message construction allocates through the operator-new hook:
      // charge those bytes, but do not re-enforce (single suspend, one log).
      QuotaHookGuard guard;
      owner->suspend("memory quota exceeded: " + std::to_string(h->account->bytes) +
                     " bytes > " + std::to_string(owner->memoryQuotaBytes_) + " byte quota");
    }
  }
  return static_cast<char*>(raw) + sizeof(AllocHeader);
}

void ScriptEngine::freeShim(void* p) {
  if (!p) return;
  auto* h = reinterpret_cast<AllocHeader*>(static_cast<char*>(p) - sizeof(AllocHeader));
  const size_t n = h->n;
  // Credit via the shared account, NOT the engine (F-M2): the engine may be
  // destroyed already; the shared_ptr keeps the account alive and this is
  // pure arithmetic. The placement-new'd shared_ptr is destroyed explicitly
  // (raw memory gets no automatic destructor call).
  std::shared_ptr<detail::QuotaAccount> account = std::move(h->account);
  h->account.~shared_ptr<detail::QuotaAccount>();
  if (account) {
    account->bytes -= static_cast<int64_t>(n);
    if (account->bytes < 0) account->bytes = 0;
  }
  std::free(h);
}

// Global operator new/delete hook support (script_memory_hook.cpp). Charge and
// credit are pure integer arithmetic: they never allocate, so they are safe
// to call from inside the replaceable global operators.
void ScriptEngine::quotaCharge(size_t bytes) noexcept {
  // The single counter (F-M1): charge the shared account; there is no
  // per-engine mirror left for this path to overwrite.
  quotaAccount_->bytes += static_cast<int64_t>(bytes);
}

void ScriptEngine::quotaCredit(size_t bytes) noexcept {
  quotaAccount_->bytes -= static_cast<int64_t>(bytes);
  if (quotaAccount_->bytes < 0) quotaAccount_->bytes = 0;
}

// May log (which allocates): the hook calls this only with its reentrancy
// guard held, so nested allocations charge but do not re-enforce.
void ScriptEngine::quotaEnforce() {
  if (quotaAccount_->bytes > static_cast<int64_t>(memoryQuotaBytes_) &&
      quotaState_ != QuotaState::kSuspended) {
    suspend("memory quota exceeded: " + std::to_string(quotaAccount_->bytes) +
             " bytes > " + std::to_string(memoryQuotaBytes_) + " byte quota");
  }
}

void ScriptEngine::installMemoryShims() {
  static std::once_flag once;
  std::call_once(once, [] {
    // Installed before the first engine exists (init() runs first), so every
    // AngelScript allocation in the process goes through the accounting shims.
    asSetGlobalMemoryFunctions(&ScriptEngine::allocShim, &ScriptEngine::freeShim);
  });
}

void ScriptEngine::messageCallback(const asSMessageInfo* m, void* selfPtr) {
  auto* self = static_cast<ScriptEngine*>(selfPtr);
  const char* kind = m->type == asMSGTYPE_ERROR ? "error" : (m->type == asMSGTYPE_WARNING ? "warning" : "info");
  if (m->type == asMSGTYPE_ERROR) {
    logError("[script] %s (%d,%d) %s: %s", m->section, m->row, m->col, kind, m->message);
    self->buildErrors_.push_back(std::string(m->section) + " (" + std::to_string(m->row) + "," + std::to_string(m->col) +
                                 "): " + m->message);
  } else {
    logInfo("[script] %s (%d,%d) %s: %s", m->section, m->row, m->col, kind, m->message);
  }
}

void ScriptEngine::scriptLog(const std::string& msg, ScriptEngine* self) {
  logInfo("[script] %s", msg.c_str());
  ++self->logCount_;
  self->lastLog_ = msg;
}

bool ScriptEngine::init() {
  installMemoryShims();
  ScriptBindingAllowlist::registerBuiltins();
  engine_ = asCreateScriptEngine();
  if (!engine_) { logError("AngelScript: engine creation failed"); return false; }
  engine_->SetMessageCallback(asFUNCTION(messageCallback), this, asCALL_CDECL);
  RegisterStdString(engine_);
  // The core binding goes through the same gated path as everything else.
  int r = bindGlobalFunction("void log(const string &in)", asFUNCTION(scriptLog), asCALL_CDECL_OBJLAST, this);
  if (r < 0) { logError("AngelScript: register log failed (%d)", r); return false; }
  logInfo("AngelScript %s ready (sandbox: allowlisted bindings only)", version());
  return true;
}

int ScriptEngine::bindGlobalFunction(const char* decl, asSFuncPtr funcPointer, asDWORD callConv, void* obj) {
  if (!engine_) return asERROR;
  if (!ScriptBindingAllowlist::isAllowlisted(decl)) {
    logError("[script] binding refused: '%s' is not on the script binding allowlist", decl);
    return asINVALID_DECLARATION;
  }
  // Records feed setModPermissions()/syncBindings(): a refused decl is
  // recorded so a later grant can bind it; a failed engine registration
  // leaves any existing record untouched (a duplicate bind must not
  // corrupt the live triple).
  BindingRecord* rec = nullptr;
  for (auto& b : bindings_)
    if (b.decl == decl) { rec = &b; break; }
  const uint32_t need = ScriptBindingAllowlist::requiredPermissions(decl);
  if ((modPermissions_ & need) != need) {
    if (!rec) bindings_.push_back(BindingRecord{decl, funcPointer, callConv, obj, false});
    logError("[script] binding refused: '%s' requires permission bits 0x%x (mod holds 0x%x)", decl, need,
             modPermissions_);
    return asINVALID_DECLARATION;
  }
  if (rec && rec->bound) {
    // Already registered on this engine (possibly eagerly by syncBindings()
    // after a permission grant). Re-asserting the identical triple is
    // idempotent success; a conflicting triple is refused, mirroring the
    // asALREADY_REGISTERED the engine would return. (Compares only the
    // meaningful asSFuncPtr bytes: flag + ptr.dummy, skipping struct padding.)
    const bool sameTriple =
        rec->callConv == callConv && rec->obj == obj && rec->func.flag == funcPointer.flag &&
        std::memcmp(rec->func.ptr.dummy, funcPointer.ptr.dummy, sizeof(rec->func.ptr.dummy)) == 0;
    if (sameTriple) return asSUCCESS;
    logError("[script] binding '%s' already registered with a different function", decl);
    return asALREADY_REGISTERED;
  }
  const int r = engine_->RegisterGlobalFunction(decl, funcPointer, callConv, obj);
  if (r >= 0) {
    if (!rec) {
      bindings_.push_back(BindingRecord{decl, funcPointer, callConv, obj, true});
    } else {
      rec->func = funcPointer;
      rec->callConv = callConv;
      rec->obj = obj;
      rec->bound = true;
    }
  } else {
    logError("[script] binding registration failed: '%s' (%d)", decl, r);
  }
  return r;
}

void ScriptEngine::beginTick() {
  cpuUsedUs_ = 0;
  // A clean tick feeds the suspension-policy decay clock: no overrun last tick
  // and the mod was running normally (not throttled, not suspended). Evaluated
  // before the throttle countdown below can flip kThrottled -> kNormal.
  const bool prevTickClean = quotaState_ == QuotaState::kNormal;
  if (quotaState_ == QuotaState::kThrottled && throttleTicksRemaining_ > 0) {
    if (--throttleTicksRemaining_ == 0) {
      quotaState_ = QuotaState::kNormal;
      logInfo("[script] mod throttle lifted (CPU back inside %u us/tick budget)", modBudgetUs_);
    }
  }
  // Suspension policy: backoff-cooldown countdown with auto-recovery onto
  // probation, probation countdown, and clean-tick decay of the record.
  if (suspensionPolicy_.onTick(prevTickClean) == SuspensionPolicy::TickAction::kAutoRecover) {
    quotaState_ = QuotaState::kNormal;
    throttleTicksRemaining_ = 0;
    throttleCount_ = 0;
    cpuUsedUs_ = 0;
    suspendReason_.clear();
  }
}

void ScriptEngine::accountCpu(uint64_t elapsedUs, const char* what) {
  cpuUsedUs_ += elapsedUs;
  if (quotaState_ == QuotaState::kSuspended) return;
  // Probation halves the effective budget (see SuspensionPolicy).
  const uint32_t effectiveBudget = suspensionPolicy_.effectiveCpuBudgetUs(modBudgetUs_);
  if (cpuUsedUs_ <= effectiveBudget) { throttleCount_ = 0; return; }  // clean tick: streak broken
  if (suspensionPolicy_.probationActive()) {
    // Zero tolerance on probation: a single overrun suspends immediately,
    // bypassing the throttle ladder.
    suspend("CPU quota overrun during probation (" + std::to_string(cpuUsedUs_) + " us > " +
            std::to_string(effectiveBudget) + " us probation budget)");
    return;
  }
  ++throttleCount_;
  if (throttleCount_ >= maxThrottles_) {
    suspend("repeated CPU quota overruns (" + std::to_string(throttleCount_) + "): " +
            std::to_string(cpuUsedUs_) + " us > " + std::to_string(modBudgetUs_) + " us/tick budget");
    return;
  }
  quotaState_ = QuotaState::kThrottled;
  throttleTicksRemaining_ = throttleTicks_;
  logError("[script] mod throttled after '%s': CPU quota exceeded (%llu us > %u us/tick budget, overrun #%u); "
           "hooks skipped for %u ticks",
           what, static_cast<unsigned long long>(cpuUsedUs_), modBudgetUs_, throttleCount_, throttleTicks_);
}

void ScriptEngine::suspend(const std::string& reason) {
  if (quotaState_ == QuotaState::kSuspended) return;  // single suspend per incident
  quotaState_ = QuotaState::kSuspended;
  suspendReason_ = reason;
  logError("[script] mod suspended: %s", reason.c_str());
  suspensionPolicy_.onSuspended(reason);  // backoff cooldown or escalation + modsys: log lines
  // A memory-quota trip fires inside the running script (allocShim /
  // quotaEnforce): abort the in-flight context NOW instead of letting it
  // run to the hook's wall-clock budget. CPU-path suspends happen after
  // Execute() returned, so executingCtx_ is already null there. Abort() is
  // safe from inside Execute (the line callback does the same); the quota
  // hook's reentrancy guard is held by our caller, so any nested allocation
  // it performs charges but never re-enforces.
  if (executingCtx_) executingCtx_->Abort();
}

void ScriptEngine::resume() {
  quotaState_ = QuotaState::kNormal;
  throttleTicksRemaining_ = 0;
  throttleCount_ = 0;
  cpuUsedUs_ = 0;
  suspendReason_.clear();
  if (suspensionPolicy_.reset())
    logInfo("modsys: mod '%s' manually re-enabled; suspension record cleared",
            suspensionPolicy_.displayId());
  logInfo("[script] mod quotas reset (admin resume)");
}

void ScriptEngine::setModPermissions(uint32_t perms) {
  if (perms == modPermissions_) return;
  modPermissions_ = perms;
  syncBindings();  // revocation takes effect now, not at the next bind
}

void ScriptEngine::syncBindings() {
  if (!engine_) return;
  bool revoked = false;
  for (auto& b : bindings_) {
    const uint32_t need = ScriptBindingAllowlist::requiredPermissions(b.decl.c_str());
    const bool granted = (modPermissions_ & need) == need;
    if (granted && !b.bound) {
      // Widened mask: a previously refused binding can register now.
      if (engine_->RegisterGlobalFunction(b.decl.c_str(), b.func, b.callConv, b.obj) >= 0) {
        b.bound = true;
        logInfo("[script] binding '%s' registered after permission grant", b.decl.c_str());
      } else {
        logError("[script] binding '%s' re-registration failed after grant", b.decl.c_str());
      }
    } else if (!granted && b.bound) {
      b.bound = false;
      revoked = true;
      logError("[script] permission revoked for '%s': unbinding", b.decl.c_str());
    }
  }
  if (revoked) rebuildSandbox();  // the only true unregister AngelScript allows
}

void ScriptEngine::unbindAll() {
  bindings_.clear();
  rebuildSandbox();
  logInfo("[script] all bindings dropped; reload required to bring scripts back");
}

void ScriptEngine::rebuildSandbox() {
  // AngelScript has no unregister for global functions: a revoked binding's
  // engine registration would otherwise stay reachable (a reload would
  // re-expose it without going through the gate). The only sound unbind is
  // a full engine rebuild: records persist, granted bindings re-register,
  // compiled scripts are dropped (the game reload()s). Must not run while
  // a script is executing (setModPermissions documents the same contract).
  // Fix 2026-10-11 (regression): the contract was documented 4x, enforced 0x.
  // A mid-execution call would ShutDownAndRelease() under a running context.
  // Fail-closed: refuse loudly rather than corrupt the engine.
  assert(executingCtx_ == nullptr);
  if (executingCtx_ != nullptr) {
    logError("[script] rebuildSandbox refused: a script is executing (fail-closed)");
    return;
  }
  for (auto& b : bindings_) b.bound = false;
  engine_->ShutDownAndRelease();
  engine_ = nullptr;
  module_ = nullptr;
  if (!init()) {
    logError("[script] sandbox rebuild failed: engine re-creation failed");
    return;
  }
  // init() re-bound log through the normal gate (its record is bound again);
  // re-register every other granted record.
  for (auto& b : bindings_) {
    if (b.bound) continue;
    const uint32_t need = ScriptBindingAllowlist::requiredPermissions(b.decl.c_str());
    if ((modPermissions_ & need) != need) continue;
    if (engine_->RegisterGlobalFunction(b.decl.c_str(), b.func, b.callConv, b.obj) >= 0) {
      b.bound = true;
    } else {
      logError("[script] binding '%s' re-registration failed after rebuild", b.decl.c_str());
    }
  }
  logError("[script] sandbox rebuilt after permission revocation; reload required");
}

void ScriptEngine::shutdown() {
  bindings_.clear();
  if (engine_) { engine_->ShutDownAndRelease(); engine_ = nullptr; module_ = nullptr; }
}

bool ScriptEngine::buildModule(const std::vector<std::pair<std::string, std::string>>& sources) {
  buildErrors_.clear();
  asIScriptModule* staging = engine_->GetModule("game.staging", asGM_ALWAYS_CREATE);
  for (const auto& [name, code] : sources)
    if (staging->AddScriptSection(name.c_str(), code.c_str(), code.size()) < 0) {
      engine_->DiscardModule("game.staging");
      return false;
    }
  if (staging->Build() < 0) {
    logError("AngelScript: build failed%s", module_ ? " (previous scripts stay active)" : "");
    engine_->DiscardModule("game.staging");
    return false;
  }
  if (module_) engine_->DiscardModule("game");
  staging->SetName("game");
  module_ = staging;
  return true;
}

bool ScriptEngine::loadDirectory(const std::string& dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!engine_ || !fs::is_directory(dir, ec)) { logError("scripts folder not found: %s", dir.c_str()); return false; }
  dir_ = dir;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_regular_file() && e.path().extension() == ".as") files.push_back(e.path());
  std::sort(files.begin(), files.end());
  std::vector<std::pair<std::string, std::string>> sources;
  for (const auto& f : files) {
    std::ifstream in(f, std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    sources.emplace_back(f.filename().string(), ss.str());
  }
  logInfo("scripts: %zu file(s) from %s", files.size(), dir.c_str());
  return buildModule(sources);
}

bool ScriptEngine::reload() { return !dir_.empty() && loadDirectory(dir_); }

bool ScriptEngine::loadSource(const std::string& sectionName, const std::string& code) {
  if (!engine_) return false;
  return buildModule({{sectionName, code}});
}

bool ScriptEngine::callHook(const char* decl) {
  if (quotaState_ == QuotaState::kSuspended) return false;  // suspended: never executes
  if (quotaState_ == QuotaState::kThrottled) return true;   // throttled: skip this tick, not an error
  if (!module_) return true;
  asIScriptFunction* fn = module_->GetFunctionByDecl(decl);
  if (!fn) return true;  // hook not defined: fine
  return run(fn, decl, nullptr, budgets_.hookMs);
}

bool ScriptEngine::callWithString(const std::string& decl, const std::string& arg) {
  if (quotaState_ == QuotaState::kSuspended) return false;
  if (quotaState_ == QuotaState::kThrottled) return true;
  if (!module_) return false;
  asIScriptFunction* fn = module_->GetFunctionByDecl(decl.c_str());
  if (!fn || fn->GetParamCount() != 1) {
    logError("[script] handler not found: %s", decl.c_str());
    return false;
  }
  std::string copy = arg;  // SetArgObject takes a mutable pointer; `const string &in` is copied anyway
  return run(fn, decl.c_str(), &copy, budgets_.handlerMs);
}

bool ScriptEngine::run(asIScriptFunction* fn, const char* what, std::string* arg, int budgetMs) {
  // RequestContext reuses pooled contexts (no allocation per call after the first).
  asIScriptContext* ctx = engine_->RequestContext();
  if (!ctx) { logError("[script] %s: no script context", what); return false; }
  Budget budget{std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs)};
  bool ok = ctx->SetLineCallback(asFUNCTION(lineCallback), &budget, asCALL_CDECL) >= 0;
  int r = ok ? ctx->Prepare(fn) : asERROR;
  if (r < 0) { logError("[script] %s: prepare failed (%d)", what, r); ok = false; }
  if (ok && arg && (r = ctx->SetArgObject(0, arg)) < 0) { logError("[script] %s: bad argument (%d)", what, r); ok = false; }
  if (ok) {
    const auto t0 = std::chrono::steady_clock::now();
    bool cppThrew = false;  // a C++ exception (not an AS exception) escaped Execute()
    {
      ModAttribution attr(this);  // script allocations during Execute count toward this mod
      asIScriptContext* const prevCtx = executingCtx_;
      executingCtx_ = ctx;
      // Exception guard (review F-L10): Execute() is a C++ call — a throwing
      // bound callback propagates straight through it. Unguarded, the throw
      // skipped the executingCtx_ restore below and escaped run() into
      // callers that assume the pairing held. The context is now restored
      // on EVERY path and the throw is reported as a failed call.
      try {
        r = ctx->Execute();
      } catch (const std::exception& e) {
        logError("[script] %s: C++ exception escaped a bound callback: %s", what, e.what());
        cppThrew = true;
        r = asEXECUTION_EXCEPTION;
      } catch (...) {
        logError("[script] %s: unknown C++ exception escaped a bound callback", what);
        cppThrew = true;
        r = asEXECUTION_EXCEPTION;
      }
      executingCtx_ = prevCtx;
    }
    const uint64_t elapsedUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    accountCpu(elapsedUs, what);
    ok = r == asEXECUTION_FINISHED;
    if (r == asEXECUTION_EXCEPTION && !cppThrew)
      logError("[script] exception in %s: %s", what, ctx->GetExceptionString());
    else if (r == asEXECUTION_EXCEPTION)
      ok = false;  // C++ throw: already logged above; GetExceptionString() has no AS exception to read
    else if (r == asEXECUTION_ABORTED) {
      // A quota trip aborts the context mid-execution (see suspend()): say
      // so, instead of blaming the wall-clock budget.
      if (quotaState_ == QuotaState::kSuspended)
        logError("[script] %s aborted (%s)", what, suspendReason_.c_str());
      else
        logError("[script] %s aborted (time budget %d ms)", what, budgetMs);
    }
  }
  ctx->ClearLineCallback();
  engine_->ReturnContext(ctx);
  return ok;
}

}  // namespace litholattice
