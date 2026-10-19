#pragma once
#include <string>
namespace lithokraft {
/// --control-test mode: control-serialization proofs (determinism of the
/// stamped stream through the real SimThread in both modes, stream format
/// round-trips, ordering/accounting checks, the 20k-tick scripted soak).
/// Returns the number of failed checks (0 = all pass), like
/// runHandshakeTests.
int runControlTests();
/// --control-scenario TICKS legacy|handshake: the circuit-scene scenario
/// driven through the serialized control stream on the real SimThread
/// (paused + stepped, tick-indexed stamped events), printing the outcome
/// hash. The no-input baseline prints the same scenario with an empty
/// stream. Returns 0 on success.
int runControlScenario(int ticks, const std::string& mode);
/// Selftest section: pure ControlStream unit checks (stamping, ordering,
/// accounting, format round-trip). Returns the number of failed checks;
/// every check prints a [PASS]/[FAIL] line prefixed "control:".
int runControlSelfTest();
}  // namespace lithokraft
