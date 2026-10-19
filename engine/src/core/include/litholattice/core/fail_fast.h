// Production failure policy: fail-fast on sim corruption (JD directive 2026-10-11).
//
// Sim corruption can lead to world corruption, and corrupted worlds are
// unacceptable. On corruption detection the engine LOGs the report (what, where,
// tick, hash state) and then ABORTs (halts). It never silently continues a
// corrupted sim.
//
// PRODUCTION-BOUNDARY CONTRACT (for the builder):
//   The deterministic scheduler (src/sim/det_sched.cpp) does NOT abort: on a
//   system-job exception it throws
//     std::runtime_error("det-sched: a system job threw; tick aborted "
//                        "(world may be half-updated)")
//   and stops the tick. That throw is the fail-fast signal and it MUST NOT be
//   swallowed. The production tick loop owns the conversion into a death:
//
//   1. If the production tick loop calls DeterministicScheduler::runTick
//      directly (headless harness / SimThread integration), the catch site is
//      the innermost loop body around the runTick call: catch std::exception,
//      then call simFailFastFromException(e, "det_sched", <running tick>).
//      Logging-and-continuing is a violation: the world may be half-updated,
//      so continuing would write a corrupted world.
//
//   2. SimThread::run HAS an exception boundary (landed 2026-10-12;
//      widened to the whole loop body 2026-10-13, review CH-H6): the
//      entire loop body — the tick pipeline, the paused branch's
//      applyEdits, publishEvents/takeEvents, the stats calls
//      (activeCount/inspect) and the castRay fallback — runs under
//      tickGuard, which converts any escaping std::exception (and any
//      non-std throw) via simFailFastFromException with subsystem
//      "sim_thread" and tick = the running world tick. Nothing on the sim
//      thread may call a game hook outside that guard. Do not
//      log-and-continue there either.
//
//   2b. The Timing Handshake has the matching WORKER boundary (landed
//      with the P0a fixes, review C3): a pull worker never lets a
//      component exception escape its thread — runItem catches it,
//      records the failure, and the authority thread converts it via
//      simFailFastFromException at the next runPhase boundary, before
//      any commit. Bare-authority affine items are covered the same way
//      inside runPhase itself.
//
//   3. Corruption detectors (hash checks, invariant checks) call simFailFast
//      directly at the detection site: the sim is already known-bad, so there
//      is nothing left to unwind for.
//
// Deliberately, there is NO hook that lets production code continue after a
// report: any such hook would violate the directive. The only split is for
// testability: formatSimCorruptionReport / simStateCorrupted /
// simCorruptionFromException are pure and unit-tested in-process, while the
// death path itself is covered by a POSIX subprocess death test (see
// testFailFast in tests/engine_tests.cpp) because calling it would abort the
// test binary.
#pragma once

#include <cstdint>
#include <exception>
#include <string>

namespace litholattice {

/// Tick index value meaning "unknown / not applicable".
inline constexpr uint64_t kUnknownTick = UINT64_MAX;
/// Hash value meaning "no hash captured" (hash fields are 0 when n/a).
inline constexpr uint64_t kNoHash = 0;

/// What was detected, where, at which tick, and what the state looked like.
struct SimCorruptionReport {
  std::string what;       // short tag: "hash-mismatch", "exception-in-tick", ...
  std::string subsystem;  // system/subsystem name: "det_sched", "signal", "sim_thread", ...
  std::string file;       // detection-site file; empty when unknown
  int line = 0;           // detection-site line; 0 when unknown
  uint64_t tick = kUnknownTick;     // tick index at detection
  uint64_t expectedHash = kNoHash;  // expected state hash; kNoHash when n/a
  uint64_t actualHash = kNoHash;    // observed state hash; kNoHash when n/a
  std::string detail;     // free-form detail; empty when none
};

/// Pure: render the report as one multi-line block. Unit-testable, never aborts.
std::string formatSimCorruptionReport(const SimCorruptionReport& r);

/// Pure: corruption detector for state-hash checks. Equal hashes => clean.
bool simStateCorrupted(uint64_t expectedHash, uint64_t actualHash);

/// Pure: build a report from a caught exception (e.g. the deterministic
/// scheduler's fail-fast throw). subsystem names the tick driver, tick is the
/// tick that was running (kUnknownTick when not known).
SimCorruptionReport simCorruptionFromException(const std::exception& e, const char* subsystem,
                                              uint64_t tick);

/// Death path: emit the report (raw stderr first, then the engine log) and
/// halt via std::abort(). No recovery, no exceptions escaping, no continuation.
[[noreturn]] void simFailFast(const SimCorruptionReport& r);

/// Death path for the production-boundary contract: convert a caught exception
/// (the scheduler's fail-fast throw) into a report and halt. This is what the
/// production tick loop calls instead of swallowing the exception.
[[noreturn]] void simFailFastFromException(const std::exception& e, const char* subsystem,
                                           uint64_t tick);

}  // namespace litholattice
