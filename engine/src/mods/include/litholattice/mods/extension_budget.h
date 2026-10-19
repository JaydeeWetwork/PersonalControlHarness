// Hierarchical per-tick budgets for extensions (worker 3).
//
// Survey result (2026-10-11): the sibling hardening agent landed its per-mod
// quota system mid-task inside ScriptEngine (script/script_engine.h): per-mod
// CPU 1024µs + script-heap memory 1MiB quotas, throttle-8-then-suspend — but
// it is script-hook-specific (AngelScript allocations via memory shims), not
// a general native-mod framework. Extensions are native IMod children, so
// this file stays a standalone ExtensionBudget tracker with a concrete
// consumption seam for the hardening agent:
//   (a) kDefaultModTickBudgetUs == ScriptEngine::kDefaultModBudgetUs (both
//       1024µs = 2^10): the top-level numbers already agree;
//   (b) drive ExtensionBudget::recordUsage()/noteMemoryBytes() from the
//       agent's own measurement points and the throttle/suspend state machine
//       below applies unchanged — or promote this class to the shared
//       implementation behind ScriptEngine's QuotaState.
//
// Policy (JD's call, 2026-10-11): budgets are parent-declared and
// engine-enforced. An extension's CPU/memory quota is carved out of ITS
// parent's quota in proportion to its declared budgetShare, so the whole
// subtree under one top-level mod can never exceed the top-level budget.
//
// Share-sum rule: siblings' budgetShares should sum to <= 100; the parent
// keeps the remainder (100 - sum) for itself. When a parent declares shares
// summing over 100 the loader does NOT refuse the tree (a config error, not
// a security violation): clampShares() scales every share down
// proportionally so they sum to <= 100 and logs the effective values
// (warn-and-clamp). The mod.json parser already clamps any single share to
// 100; the C facet path clamps via ExtensionLoader (see extension_loader.cpp).
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "litholattice/mods/extensible_interface.h"

namespace litholattice {

/// Per-tick CPU budget for a top-level mod, in microseconds. 2^10 = 1024:
/// the same 1024µs number AngelScript gets (mod_safety.h rule 5). Every
/// extension's quota is carved out of its parent's quota per budgetShare, so
/// a whole extension tree can never exceed this number per tick.
inline constexpr uint64_t kDefaultModTickBudgetUs = 1024;

/// Default per-mod memory ceiling, in bytes (2^26 = 64 MiB). This is the
/// NATIVE-extension analogue of ScriptEngine::kDefaultModMemoryQuotaBytes
/// (1 MiB, which covers only the AngelScript heap via allocation shims —
/// far too small for native code). v1 is accounting-only (the extension
/// reports via noteMemoryBytes); the hardening agent wires real measurement
/// in later — the state machine below already treats a memory overage like a
/// CPU overrun.
inline constexpr uint64_t kDefaultModMemoryBudgetBytes = 64ull * 1024 * 1024;

/// Overrun policy: kBudgetThrottleStreak consecutive over-quota ticks ->
/// throttled for kBudgetThrottleCooldownTicks ticks; after the cooldown, a
/// short probation streak (2) of overruns -> suspended (until reload).
inline constexpr uint32_t kBudgetThrottleStreak = 4;
inline constexpr uint32_t kBudgetProbationStreak = 2;
inline constexpr uint32_t kBudgetThrottleCooldownTicks = 8;

/// What the budget gate currently allows for one node.
enum class BudgetGate : uint8_t {
  kAllow,      ///< tick normally
  kThrottled,  ///< skip ticks until the cooldown decays (advanceTick)
  kSuspended,  ///< no more ticks until reload; logged with a reason
};

/// One node's quota + throttle state. Pure accounting: the engine's tick
/// path calls beginTick()/endTick() (or recordUsage()) around the
/// extension's onTick; the gate then says whether the next tick may run.
/// Siblings are independent nodes, so throttling one never affects another.
class ExtensionBudget {
 public:
  ExtensionBudget(std::string path, uint64_t tickQuotaUs, uint64_t memoryQuotaBytes);

  const std::string& path() const { return path_; }
  uint64_t tickQuotaUs() const { return tickQuotaUs_; }
  uint64_t memoryQuotaBytes() const { return memoryQuotaBytes_; }
  BudgetGate gate() const { return gate_; }
  uint32_t overrunStreak() const { return overrunStreak_; }
  uint32_t throttleCount() const { return throttleCount_; }
  uint64_t accountedMemoryBytes() const { return memoryBytes_; }
  const std::string& suspendReason() const { return suspendReason_; }

  /// Carve a child's quota out of a parent quota by share percent (0-100).
  /// childTickQuota(kDefaultModTickBudgetUs, 25) == 256.
  static uint64_t childTickQuota(uint64_t parentQuotaUs, uint32_t sharePercent);
  static uint64_t childMemoryQuota(uint64_t parentQuotaBytes, uint32_t sharePercent);

  /// Warn-and-clamp for sibling shares (see the header contract above).
  /// Returns one effective share per decl, in decl order; each 0-100 and the
  /// sum <= 100. Over-100 sums are scaled down proportionally (floor) and
  /// the effective values are logged. Never rejects: budget oversubscription
  /// is a config error, not a security violation.
  static std::vector<uint32_t> clampShares(const LithoLatticeExtensible* facet);

  /// Real-time measurement around one onTick call.
  void beginTick();
  /// Ends the tick begun by beginTick(); measures elapsed µs and records it.
  void endTick();
  /// Direct accounting (tests, and the hardening agent's measurement seam).
  void recordUsage(uint64_t elapsedUs);
  /// Extension-reported memory delta (bytes, signed; clamped at 0 total).
  void noteMemoryBytes(int64_t delta);
  /// Once per engine tick, even for throttled nodes: decays the cooldown.
  void advanceTick();

 private:
  void onOverrun(const char* kind);
  void enterThrottled(const char* kind);
  void enterSuspended(const char* kind);

  std::string path_;
  uint64_t tickQuotaUs_;
  uint64_t memoryQuotaBytes_;
  uint64_t memoryBytes_ = 0;
  BudgetGate gate_ = BudgetGate::kAllow;
  uint32_t overrunStreak_ = 0;
  uint32_t throttleCount_ = 0;
  uint32_t cooldownTicks_ = 0;
  bool probation_ = false;
  std::string suspendReason_;
  std::chrono::steady_clock::time_point tickStart_{};
  bool tickOpen_ = false;
};

}  // namespace litholattice
