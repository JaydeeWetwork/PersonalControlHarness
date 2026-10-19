// Verbose thread/job-system debug logging, compile-flag-gated.
//
// Define LITHOLATTICE_THREAD_DEBUG (CMake option LITHOLATTICE_THREAD_DEBUG, off by
// default) to instrument the threading machinery: job lifecycle (submit / steal /
// start / finish + duration tripwire), lock acquire/release with the documented lock
// order, thread wake/sleep transitions, per-deque depths, thread naming and affinity
// assignments, and input-pump queue behavior.
//
// When the flag is OFF every macro below expands to nothing: zero overhead, no
// runtime branches in hot paths (verified: the instrumented self-tests build and pass
// both ways). When ON, output goes to stderr via a dedicated leaf lock (see the lock
// order in job_system.h: the debug-log lock is L3, the absolute leaf — it is only
// ever taken to emit one line and never held across other work).
#pragma once

#include <cstdint>

namespace litholattice {
namespace detail {

#ifdef LITHOLATTICE_THREAD_DEBUG
/// Emit one debug line. `tag` selects the subsystem ("job", "lock", "thread", "input").
/// Cheap: guarded by the compile-time flag at every call site, so disabled builds pay
/// nothing. The sink itself takes the leaf debug-log lock and releases it per line.
void threadLog(const char* tag, const char* fmt, ...);
#endif
/// Monotonic microsecond clock for debug timestamps. Declared unconditionally so
/// `if constexpr (kThreadDebug)` call sites compile both ways; defined (and only
/// ever called) when LITHOLATTICE_THREAD_DEBUG is on.
uint64_t threadDebugNowUs();
/// Next job id for lifecycle tracing. Same unconditional-declaration rule.
uint64_t threadDebugNextJobId();

}  // namespace detail
}  // namespace litholattice

#ifdef LITHOLATTICE_THREAD_DEBUG
#define LITHO_THREAD_LOG(tag, ...) \
  ::litholattice::detail::threadLog((tag), __VA_ARGS__)
#else
// Zero-overhead when disabled: no code, no branch, arguments never evaluated.
#define LITHO_THREAD_LOG(tag, ...) ((void)0)
#endif
