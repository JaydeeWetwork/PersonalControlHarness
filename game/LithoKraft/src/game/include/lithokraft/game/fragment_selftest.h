// Fragment state self-test runner header. LF line endings.
#pragma once

namespace lithokraft {

/// Runs the Fragment state self-test (unlock state machine, serialize
/// round-trip, malformed-input handling, Spark-upgrade simulation).
/// Returns the failure count.
int runFragmentSelfTest();

}  // namespace lithokraft
