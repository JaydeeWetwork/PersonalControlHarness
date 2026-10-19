// Internal: allocation-attribution state shared by the AngelScript memory shims
// (script_engine.cpp) and the global operator new/delete hook
// (script_memory_hook.cpp). NOT part of the public engine API: include it only
// from engine-internal translation units.
#pragma once

#include <cstddef>

namespace litholattice {

class ScriptEngine;

// The mod whose hooks are currently executing on this thread (scripts run on
// the main thread only, per mod_safety.h). While set, both the AngelScript
// memory shims and the global operator new/delete hook attribute allocations
// to it, charging its memory quota. Null outside mod execution.
extern thread_local ScriptEngine* tlsScriptAllocOwner;

// Reentrancy guard for the global operator-new hook. While nonzero the hook
// still writes its per-allocation header (so delete stays balanced) and still
// charges the owner, but skips quota enforcement: enforcement may log, and
// logging allocates, which would recurse into the hook.
extern thread_local unsigned tlsQuotaHookDepth;

// RAII: attribute this thread's allocations to `engine` until scope end.
struct ModAttribution {
  explicit ModAttribution(ScriptEngine* e) : prev(tlsScriptAllocOwner) { tlsScriptAllocOwner = e; }
  ~ModAttribution() { tlsScriptAllocOwner = prev; }
  ScriptEngine* prev;
};

// RAII: hold the operator-new hook's reentrancy guard for a scope.
struct QuotaHookGuard {
  QuotaHookGuard() { ++tlsQuotaHookDepth; }
  ~QuotaHookGuard() { --tlsQuotaHookDepth; }
};

namespace detail {

// Shared implementation behind the replaceable global allocation functions.
// In script_memory_hook.cpp (single definition); declared here so the hook
// translation unit is the only place that defines the global operators.
void* quotaAllocate(std::size_t n, std::size_t alignment, bool nothrow);
void quotaDeallocate(void* p) noexcept;

}  // namespace detail
}  // namespace litholattice
