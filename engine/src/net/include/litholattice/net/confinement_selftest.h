// Headless checks of the net confinement layer (--selftest): the per-mod
// rate limiter (ModNetGate), the send-path guard (NetConfinement), and the
// confined multiplexer (ConfinedChannelMux), including adversarial checks
// (budget overruns -> throttled, sustained abuse -> suspended, unregistered
// mods and missing gates fail closed).
#pragma once

namespace litholattice::net {
/// Prints one PASS / FAIL line per check; returns the number of failures.
int runNetConfinementSelfTest();
}  // namespace litholattice::net
