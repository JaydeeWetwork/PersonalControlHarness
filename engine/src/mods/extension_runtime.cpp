// ExtensionRuntime: budget tree + per-extension fault domains.
// See extension_runtime.h for the contracts.
#include "litholattice/mods/extension_runtime.h"

#include <exception>

#include "litholattice/core/log.h"

namespace litholattice {

void ExtensionRuntime::sync() {
  // Registry paths sort parents before children (a parent path is always a
  // strict prefix of its children's), so one ordered pass builds the quota
  // tree top-down.
  std::set<std::string> live;
  for (const std::string& path : loader_.registry().allPaths()) {
    live.insert(path);
    crashed_.erase(path);  // live again (reloaded or still running): clear the mark
    if (budgets_.count(path)) continue;  // keep gate state for survivors
    const ExtensionContext* ctx = loader_.contextFor(path);
    uint64_t parentTick = kDefaultModTickBudgetUs;
    uint64_t parentMem = kDefaultModMemoryBudgetBytes;
    uint32_t share = 100;
    if (ctx && !ctx->parentPath.empty()) {
      const auto pit = budgets_.find(ctx->parentPath);
      if (pit != budgets_.end()) {
        parentTick = pit->second.tickQuotaUs();
        parentMem = pit->second.memoryQuotaBytes();
      }
      share = ctx->budgetShare;
    }
    budgets_.emplace(
        path, ExtensionBudget(path, ExtensionBudget::childTickQuota(parentTick, share),
                              ExtensionBudget::childMemoryQuota(parentMem, share)));
  }
  // Drop unloaded nodes. Their crash marks are KEPT as diagnostic history:
  // crashed(path) stays true until the path reloads (see the erase above).
  for (auto it = budgets_.begin(); it != budgets_.end();) {
    if (!live.count(it->first)) {
      it = budgets_.erase(it);
    } else {
      ++it;
    }
  }
}

ExtensionRuntime::TickOutcome ExtensionRuntime::tick(
    const std::string& path, const std::function<void()>& onTick) {
  const auto it = budgets_.find(path);
  if (it == budgets_.end()) return TickOutcome::kUnknown;
  ExtensionBudget& budget = it->second;
  switch (budget.gate()) {
    case BudgetGate::kThrottled:
      return TickOutcome::kThrottled;
    case BudgetGate::kSuspended:
      return TickOutcome::kSuspended;
    case BudgetGate::kAllow:
      break;
  }
  budget.beginTick();
  try {
    onTick();
  } catch (const std::exception& e) {
    onCrash(path, e.what());
    return TickOutcome::kCrashed;
  } catch (...) {
    onCrash(path, "unknown exception");
    return TickOutcome::kCrashed;
  }
  budget.endTick();
  return TickOutcome::kTicked;
}

void ExtensionRuntime::advanceTick() {
  for (auto& [path, budget] : budgets_) budget.advanceTick();
}

void ExtensionRuntime::onCrash(const std::string& path, const char* reason) {
  // Contain: mark it, run its onUnload exactly once, and unload its subtree.
  // ExtensionLoader::unloadMod invokes onUnload for every node it removes
  // (children-first) and never deletes save data; siblings and the parent
  // are untouched, so they keep ticking.
  logError("mods: extension '%s' crashed in onTick (%s): containing — "
           "onUnload runs, subtree unloads, parent and siblings survive",
           path.c_str(), reason ? reason : "(null)");
  crashed_.insert(path);
  loader_.unloadMod(path);
  sync();  // drop the crashed subtree's budget nodes (keeps survivors' state)
}

const ExtensionBudget* ExtensionRuntime::budgetFor(const std::string& path) const {
  const auto it = budgets_.find(path);
  return it == budgets_.end() ? nullptr : &it->second;
}

ExtensionBudget* ExtensionRuntime::budgetFor(const std::string& path) {
  const auto it = budgets_.find(path);
  return it == budgets_.end() ? nullptr : &it->second;
}

}  // namespace litholattice
