// ExtensionBudget: hierarchical per-tick budgets. See extension_budget.h.
#include "litholattice/mods/extension_budget.h"

#include "litholattice/core/log.h"

namespace litholattice {

ExtensionBudget::ExtensionBudget(std::string path, uint64_t tickQuotaUs,
                                 uint64_t memoryQuotaBytes)
    : path_(std::move(path)),
      tickQuotaUs_(tickQuotaUs),
      memoryQuotaBytes_(memoryQuotaBytes) {}

uint64_t ExtensionBudget::childTickQuota(uint64_t parentQuotaUs, uint32_t sharePercent) {
  if (sharePercent > 100) sharePercent = 100;
  return parentQuotaUs * sharePercent / 100;
}

uint64_t ExtensionBudget::childMemoryQuota(uint64_t parentQuotaBytes,
                                           uint32_t sharePercent) {
  if (sharePercent > 100) sharePercent = 100;
  return parentQuotaBytes * sharePercent / 100;
}

std::vector<uint32_t> ExtensionBudget::clampShares(const LithoLatticeExtensible* facet) {
  std::vector<uint32_t> out;
  if (!facet) return out;
  uint64_t sum = 0;
  for (uint32_t i = 0; i < facet->extensionCount; ++i) {
    const uint32_t share =
        facet->extensions[i].budgetShare > 100 ? 100 : facet->extensions[i].budgetShare;
    out.push_back(share);
    sum += share;
  }
  if (sum <= 100) return out;  // the common case: shares fit, parent keeps the rest
  // Warn-and-clamp: scale down proportionally (floor) so the tree still loads.
  logError("mods: budget shares sum to %llu > 100; scaling down proportionally",
           static_cast<unsigned long long>(sum));
  for (uint32_t i = 0; i < out.size(); ++i) {
    const uint32_t scaled = static_cast<uint32_t>(out[i] * 100 / sum);
    logError("mods:   extension '%s': declared %u -> effective %u",
             facet->extensions[i].id ? facet->extensions[i].id : "(null)", out[i],
             scaled);
    out[i] = scaled;
  }
  return out;
}

void ExtensionBudget::beginTick() {
  tickStart_ = std::chrono::steady_clock::now();
  tickOpen_ = true;
}

void ExtensionBudget::endTick() {
  if (!tickOpen_) return;
  tickOpen_ = false;
  const auto elapsed = std::chrono::steady_clock::now() - tickStart_;
  recordUsage(
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
}

void ExtensionBudget::recordUsage(uint64_t elapsedUs) {
  if (gate_ == BudgetGate::kSuspended) return;
  const bool cpuOver = elapsedUs > tickQuotaUs_;
  const bool memOver = memoryBytes_ > memoryQuotaBytes_;
  if (!cpuOver && !memOver) {
    overrunStreak_ = 0;
    return;
  }
  onOverrun(cpuOver ? "cpu" : "memory");
}

void ExtensionBudget::noteMemoryBytes(int64_t delta) {
  if (delta < 0 && static_cast<uint64_t>(-delta) > memoryBytes_) {
    memoryBytes_ = 0;
  } else {
    memoryBytes_ = static_cast<uint64_t>(static_cast<int64_t>(memoryBytes_) + delta);
  }
}

void ExtensionBudget::advanceTick() {
  if (gate_ != BudgetGate::kThrottled || cooldownTicks_ == 0) return;
  if (--cooldownTicks_ == 0) {
    gate_ = BudgetGate::kAllow;
    probation_ = true;  // one more short overrun streak now suspends
    logInfo("mods: budget for '%s' resumed after throttle cooldown (on probation)",
            path_.c_str());
  }
}

void ExtensionBudget::onOverrun(const char* kind) {
  ++overrunStreak_;
  const uint32_t limit = probation_ ? kBudgetProbationStreak : kBudgetThrottleStreak;
  if (overrunStreak_ < limit) return;
  overrunStreak_ = 0;
  if (probation_) {
    enterSuspended(kind);
  } else {
    enterThrottled(kind);
  }
}

void ExtensionBudget::enterThrottled(const char* kind) {
  gate_ = BudgetGate::kThrottled;
  cooldownTicks_ = kBudgetThrottleCooldownTicks;
  ++throttleCount_;
  logError(
      "mods: budget overrun (%s) for '%s': throttled for %u ticks "
      "(quota %llu us/tick, %llu bytes)",
      kind, path_.c_str(), kBudgetThrottleCooldownTicks,
      static_cast<unsigned long long>(tickQuotaUs_),
      static_cast<unsigned long long>(memoryQuotaBytes_));
}

void ExtensionBudget::enterSuspended(const char* kind) {
  gate_ = BudgetGate::kSuspended;
  suspendReason_ = std::string("sustained budget overrun (") + kind +
                   ") after throttle probation";
  logError("mods: budget for '%s' SUSPENDED: %s", path_.c_str(), suspendReason_.c_str());
}

}  // namespace litholattice
