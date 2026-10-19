// Sandboxed AngelScript host. Scripts get ONLY registered bindings (string type,
// log(string), and whatever the game binds through the allowlist — no file,
// network, or system access). Runaway scripts are aborted after a time budget.
//
// One ScriptEngine = one mod's sandbox. It carries that mod's permission
// bitmask (ScriptBindingAllowlist + LithoLatticeModPermission) and its quotas:
// a CPU budget per tick (overrun -> throttled for a few ticks -> suspended,
// with a logged reason, per mod_safety.h) and a memory quota (overrun ->
// suspended with a logged reason). Suspensions feed the automatic
// suspension-recovery policy (SuspensionPolicy: backoff cooldown, probation,
// escalation to manual re-enable); every transition is logged through the
// engine-log tap ("modsys:" lines) for the debug console.
// Quota defaults are powers of two.
#pragma once
#include <memory>
#include <cstdint>
#include <string>
#include <vector>
#include <angelscript.h>
#include "litholattice/script/script_binding_allowlist.h"
#include "litholattice/script/suspension_policy.h"

namespace litholattice {
namespace detail {
/// Quota accounting control block (defined here so both the engine and the
/// memory hook see it). Allocation headers hold a shared_ptr to this, never a
/// raw ScriptEngine*, so post-destruction credits can't UAF the engine.
struct QuotaAccount {
  int64_t bytes = 0;
};
}

class ScriptEngine {
 public:
  ScriptEngine() = default;
  ~ScriptEngine() { shutdown(); }
  ScriptEngine(const ScriptEngine&) = delete;
  ScriptEngine& operator=(const ScriptEngine&) = delete;

  bool init();      // engine + message callback + core API (string, log)
  /// The mod's granted permission bitmask (LithoLatticeModPermission bits, from
  /// the mod's mod.json). The game sets this before binding the mod's API;
  /// bindGlobalFunction() refuses bindings the mask does not grant. Changing
  /// the mask re-syncs the bindings (see syncBindings): narrowing the mask
  /// unbinds now-revoked functions. Call only when no script is executing.
  void setModPermissions(uint32_t perms);
  uint32_t modPermissions() const { return modPermissions_; }
  /// Re-checks every recorded binding against the current mask. Bindings
  /// whose required bits are no longer granted are unbound; bindings refused
  /// earlier that a widened mask now grants are registered. AngelScript has
  /// no unregister for global functions, so revocation rebuilds the whole
  /// script sandbox (see rebuildSandbox): the revoked registration is truly
  /// gone, and the next load()/reload() compiles against the (now stricter)
  /// gate. Call only when no script is executing.
  void syncBindings();
  /// Drops every binding record and rebuilds the sandbox (tearing down a
  /// mod's API surface). Re-bind + reload() to bring scripts back. Call only
  /// when no script is executing.
  void unbindAll();

  /// Gated binding registration: the ONLY way to add a script API. Refuses
  /// (returns < 0, logs a reason) when `decl` is not on the
  /// ScriptBindingAllowlist, or when the mod's permission bitmask does not
  /// grant the entry's required bits. The raw asIScriptEngine* is deliberately
  /// NOT exposed: every binding goes through the allowlist, no bypass.
  /// Returns the AngelScript result (>= 0 on success).
  int bindGlobalFunction(const char* decl, asSFuncPtr funcPointer, asDWORD callConv,
                         void* obj = nullptr);

  /// Per-mod per-tick CPU budget in microseconds (default 1024 = 1ms).
  /// Powers of two. See beginTick()/callHook() for the enforcement policy.
  void setModBudgetUs(uint32_t us) { modBudgetUs_ = us; }
  uint32_t modBudgetUs() const { return modBudgetUs_; }
  /// Per-mod script memory quota in bytes (default 1 MiB). Powers of two.
  /// AngelScript heap allocations made while this mod's hooks run (script
  /// class instances, engine internals) are attributed to it via the memory
  /// shims; exceeding the quota suspends the mod with a logged reason.
  /// The global operator new/delete hook (script_memory_hook.cpp) extends the
  /// same attribution to standard-container memory: std::string heap buffers
  /// and other container allocations made while this mod's hooks run count
  /// against the quota too, closing the old string-buffer blind spot.
  /// Honest limits: SSO strings never heap-allocate (nothing to count);
  /// allocations on threads with no attributed mod are unattributed by
  /// design; third-party libraries with custom allocators (mmap, pools)
  /// bypass the hook; the first Execute in the process leaves a small
  /// one-time AngelScript lazy allocation attributed to the running mod
  /// (freed at engine shutdown, so it never grows). Each allocation is
  /// attributed at exactly one point (the AS shims use malloc/free directly
  /// and never pass through the operator-new hook), so nothing is
  /// double-counted.
  void setModMemoryQuotaBytes(uint64_t bytes) { memoryQuotaBytes_ = bytes; }
  uint64_t modMemoryQuotaBytes() const { return memoryQuotaBytes_; }
  /// Throttle window: ticks to skip the mod's hooks after a CPU overrun
  /// (default 8). Powers of two. 0 is clamped to 1 (a zero window would leave
  /// a throttled mod stuck until an admin resume()).
  void setThrottleTicks(uint32_t ticks) { throttleTicks_ = ticks ? ticks : 1; }
  uint32_t throttleTicks() const { return throttleTicks_; }
  /// Consecutive overrunning ticks before suspension (default 4). Powers of two.
  /// A tick inside the budget breaks the streak.
  void setMaxThrottles(uint32_t n) { maxThrottles_ = n; }
  uint32_t maxThrottles() const { return maxThrottles_; }

  /// Starts a new tick: resets per-tick CPU accounting, counts down an active
  /// throttle window (the mod resumes when it reaches zero, unless it was
  /// suspended), and advances the suspension policy (cooldown countdown with
  /// auto-recovery on probation, probation countdown, clean-tick decay).
  /// The game/sim calls this once per tick before hooks.
  void beginTick();
  bool throttled() const { return quotaState_ == QuotaState::kThrottled; }
  bool suspended() const { return quotaState_ == QuotaState::kSuspended; }
  /// Why the mod was suspended (empty unless suspended).
  const std::string& suspendReason() const { return suspendReason_; }
  /// Admin re-enable: clears throttle/suspend state, quota counters, AND the
  /// suspension-policy record (consecutive count, cooldown, probation).
  void resume();
  /// Identifies the mod in suspension-policy log lines ("modsys: ..."). The
  /// ModLoader sets this when it creates the engine; empty renders as
  /// "<unknown>".
  void setModId(const std::string& id) { modId_ = id; suspensionPolicy_.setModId(id); }
  const std::string& modId() const { return modId_; }
  /// The automatic suspension-recovery policy (backoff cooldown, probation,
  /// escalation to manual re-enable). Driven by beginTick()/suspend()/
  /// resume(); exposed for tests and admin introspection.
  const SuspensionPolicy& suspensionPolicy() const { return suspensionPolicy_; }
  /// Script heap bytes currently attributed to this mod.
  /// Derived ONLY from the single shared quota account (review F-M1):
  /// there is no second counter for this value to disagree with.
  int64_t modMemoryBytes() const { return quotaAccount_->bytes; }
  /// CPU microseconds this mod's hooks have consumed in the current tick.
  uint64_t tickCpuUs() const { return cpuUsedUs_; }

  void shutdown();

  // Compile every *.as file in dir into one module. False on missing dir/compile error; on
  // failure the previously loaded scripts stay active (so a hot reload never leaves none).
  bool loadDirectory(const std::string& dir);
  bool loadSource(const std::string& sectionName, const std::string& code);  // tests/tools
  /// Recompiles the last loadDirectory() folder (hot reload); same failure rule.
  bool reload();
  const std::string& directory() const { return dir_; }
  /// Wall-clock budgets per call (a script past its budget is aborted and logged). Hooks such
  /// as onLoad() run once and get more time; UI handlers run on the frame and get less.
  struct Budgets {
    int hookMs = 500;
    int handlerMs = 100;
  };
  void setBudgets(Budgets b) { budgets_ = b; }
  const Budgets& budgets() const { return budgets_; }
  // Call a hook like "void onLoad()" if the scripts define it. False only on a runtime error;
  // a throttled (skipped) or suspended mod returns without executing (false when suspended).
  bool callHook(const char* decl);
  /// Calls `decl` (one `const string &in` parameter, e.g. a UI handler) with `arg`. False if
  /// the function is missing, has another signature, or fails (or the mod is suspended).
  bool callWithString(const std::string& decl, const std::string& arg);
  /// Compiler errors of the last build ("file (row,col): message"), empty after a clean build.
  const std::vector<std::string>& buildErrors() const { return buildErrors_; }

  int logCount() const { return logCount_; }
  const std::string& lastLog() const { return lastLog_; }
  static const char* version() { return ANGELSCRIPT_VERSION_STRING; }

  /// Global operator-new hook support (script_memory_hook.cpp). The hook
  /// calls these to attribute standard-container / string heap buffers to
  /// this mod while its hooks execute. quotaCharge/quotaCredit perform no
  /// allocation and never throw, so they are safe to call from inside the
  /// replaceable global operators; quotaEnforce() may log (which allocates)
  /// and must only be called with the hook's reentrancy guard held.
  void quotaCharge(size_t bytes) noexcept;
  void quotaCredit(size_t bytes) noexcept;
  void quotaEnforce();
  /// Shared quota account (see script_memory_hook.cpp): allocation headers hold
  /// a shared_ptr to this, so post-destruction credits never touch freed memory.
  std::shared_ptr<detail::QuotaAccount> quotaAccount() const { return quotaAccount_; }

  // Quota defaults (powers of two, per mod_safety.h).
  static constexpr uint32_t kDefaultModBudgetUs = 1024;             // 2^10 us = ~1 ms/tick
  static constexpr uint64_t kDefaultModMemoryQuotaBytes = 1 << 20;  // 2^20 = 1 MiB
  static constexpr uint32_t kDefaultThrottleTicks = 8;              // skip 8 ticks after overrun
  static constexpr uint32_t kDefaultMaxThrottles = 4;               // then suspend

 private:
  enum class QuotaState { kNormal, kThrottled, kSuspended };
  static void scriptLog(const std::string& msg, ScriptEngine* self);
  static void messageCallback(const asSMessageInfo* m, void* self);
  /// Builds `sources` (name, code) into a staging module; swaps it in only on success.
  bool buildModule(const std::vector<std::pair<std::string, std::string>>& sources);
  /// Rebuilds the whole AngelScript engine: the only true unregister the API
  /// allows (revocation / unbindAll path). Binding records persist and
  /// granted bindings re-register; compiled scripts are dropped (the game
  /// reload()s). Must not run while a script is executing.
  void rebuildSandbox();
  bool run(asIScriptFunction* fn, const char* what, std::string* arg, int budgetMs);
  /// Applies the throttle/suspend policy after a hook consumed `elapsedUs`.
  void accountCpu(uint64_t elapsedUs, const char* what);
  void suspend(const std::string& reason);
  /// One recorded bindGlobalFunction attempt (granted or refused): the
  /// re-sync surface for setModPermissions. AngelScript cannot unregister a
  /// global function, so `bound` tracks the record-level truth and
  /// revocation additionally discards the module (see syncBindings).
  struct BindingRecord {
    std::string decl;
    asSFuncPtr func;
    asDWORD callConv;
    void* obj = nullptr;
    bool bound = false;  ///< currently registered on the engine
  };
  std::vector<BindingRecord> bindings_;

  // Per-mod accounting shims for AngelScript's global memory functions.
  // Installed once per process (main thread only, per mod_safety.h); allocations
  // made while a mod's hooks run are attributed to that mod via
  // tlsScriptAllocOwner (script_memory_internal.h; shared with the global
  // operator new/delete hook in script_memory_hook.cpp).
  static void* allocShim(size_t n);
  static void freeShim(void* p);
  static void installMemoryShims();

  asIScriptEngine* engine_ = nullptr;
  uint32_t modPermissions_ = 0;  ///< granted LithoLatticeModPermission bits
  uint32_t modBudgetUs_ = kDefaultModBudgetUs;
  uint64_t memoryQuotaBytes_ = kDefaultModMemoryQuotaBytes;
  uint32_t throttleTicks_ = kDefaultThrottleTicks;
  uint32_t maxThrottles_ = kDefaultMaxThrottles;
  QuotaState quotaState_ = QuotaState::kNormal;
  uint32_t throttleTicksRemaining_ = 0;
  uint32_t throttleCount_ = 0;  ///< consecutive CPU overruns
  uint64_t cpuUsedUs_ = 0;      ///< this tick's hook CPU time
  std::string suspendReason_;
  std::string modId_;                ///< owning mod's id for log lines (empty until set)
  SuspensionPolicy suspensionPolicy_;  ///< auto-recovery/backoff/probation/escalation
  // The ONE memory-quota counter (review F-M1): quotaAccount_->bytes. Both
  // accounting paths — the AngelScript shims and the global operator-new
  // hook — charge/credit THIS account and nothing else; the old separate
  // memoryBytes_ field was overwritten by the operator-new path, silently
  // erasing the AS-heap total from the quota check (quota evasion).
  std::shared_ptr<detail::QuotaAccount> quotaAccount_ = std::make_shared<detail::QuotaAccount>();
  /// The context currently inside Execute() on this thread (null outside
  /// run()). A memory-quota trip fires inside the running script, so
  /// suspend() aborts this context instead of letting the script run to the
  /// hook's wall-clock budget. Scripts run on one thread (mod_safety.h), so
  /// no locking: set/cleared only around Execute().
  asIScriptContext* executingCtx_ = nullptr;
  asIScriptModule* module_ = nullptr;
  int logCount_ = 0;
  std::string lastLog_, dir_;
  std::vector<std::string> buildErrors_;
  Budgets budgets_;
};

}  // namespace litholattice
