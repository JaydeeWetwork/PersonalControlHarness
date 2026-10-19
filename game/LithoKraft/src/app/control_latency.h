#pragma once
namespace lithokraft {
/// --control-latency mode: input-to-application latency measurement for
/// SimThread control edits (Command edits carrying a sequence index; the
/// fixture simulation stamps each application on the sim thread). Prints
/// the latency distribution in ms and in world ticks at 512 TPS.
/// Compiles against the pre-serialization SimThread API on purpose: the
/// same harness measures the old path and the serialized path.
int runControlLatency();
}  // namespace lithokraft
