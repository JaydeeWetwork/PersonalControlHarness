#pragma once
#include <cstdint>
#include <string>
namespace lithokraft {
/// --demo-test mode: IDemoRecord proofs (record a scripted SimThread
/// session, non-perturbation, bit-identical replay in legacy and
/// handshake modes). Returns 0 on success, 1 on failure.
int runDemoTests();
/// Selftest section: pure IDemoRecord unit checks (round trip through a
/// bare ControlStream, the fail-closed / recovery matrix, the analysis
/// queries). Returns the number of failed checks; every check prints a
/// [PASS]/[FAIL] line prefixed "demo:".
int runDemoSelfTest();
/// --demo-dump FILE [--ticks LO HI]: parse a demo file and print its
/// session, status, per-kind / per-source counts, tick span and trailer;
/// with --ticks, also list the events in the inclusive range.
int runDemoDump(const std::string& file, bool hasRange, uint64_t lo, uint64_t hi);
/// --demo-verify FILE: print the verdict; exit 0 if complete, 2 if
/// truncated-but-recovered, 1 on a hard error.
int runDemoVerify(const std::string& file);
}  // namespace lithokraft
