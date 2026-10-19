// ExtensionRuntime (worker 3): the per-tick engine for loaded extensions.
// Owns the hierarchical ExtensionBudget tree (rebuilt from the
// ExtensionLoader's registry + contexts) and runs each extension's onTick
// inside a fault domain.
//
// Crash isolation, one level down (JD's call, 2026-10-11): an extension that
// throws across its IMod boundary is CONTAINED — the runtime catches it,
// runs the extension's onUnload exactly once, unloads its subtree, and the
// parent and siblings keep ticking. Honest boundary: v1 catches C++
// exceptions. A hard crash (SIGSEGV, abort(), stack smash) cannot be
// contained in-process — that needs a process boundary, which arrives with
// DLL-era loading; the sim's own fail-fast rule (log + abort on sim
// corruption) still applies to real memory corruption.
//
// Budget enforcement: tick() consults the extension's budget gate first
// (throttled/suspended nodes are skipped without running), then measures the
// tick into the budget. Sibling nodes are independent: throttling one never
// affects another or the parent.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>

#include "litholattice/mods/extension_budget.h"
#include "litholattice/mods/extension_loader.h"

namespace litholattice {

class ExtensionRuntime {
 public:
  enum class TickOutcome : uint8_t {
    kTicked,     ///< onTick ran (inside the fault domain)
    kThrottled,  ///< budget gate said skip this tick
    kSuspended,  ///< budget gate suspended this node until reload
    kCrashed,    ///< onTick threw: contained, onUnload ran, subtree unloaded
    kUnknown,    ///< path is not a loaded extension
  };

  explicit ExtensionRuntime(ExtensionLoader& loader) : loader_(loader) {}

  /// Rebuilds the budget tree from the loader's registry + contexts.
  /// Preserves gate state (throttle/suspend) for nodes that are still loaded;
  /// drops nodes that were unloaded. Call after loadMod()/unloadMod().
  void sync();

  /// One extension tick: budget gate -> fault domain -> accounting.
  /// onTick is the extension's tick callback (usually its IMod onTick).
  TickOutcome tick(const std::string& path, const std::function<void()>& onTick);

  /// Once per engine tick: decays throttle cooldowns across all nodes.
  void advanceTick();

  /// True once path has crashed. The mark is diagnostic history: it persists
  /// after the crashed subtree unloads and clears only when the path reloads.
  bool crashed(const std::string& path) const { return crashed_.count(path) != 0; }
  const ExtensionBudget* budgetFor(const std::string& path) const;
  ExtensionBudget* budgetFor(const std::string& path);
  size_t budgetNodeCount() const { return budgets_.size(); }

 private:
  void onCrash(const std::string& path, const char* reason);

  ExtensionLoader& loader_;
  std::map<std::string, ExtensionBudget> budgets_;  // by dotted path
  std::set<std::string> crashed_;
};

}  // namespace litholattice
