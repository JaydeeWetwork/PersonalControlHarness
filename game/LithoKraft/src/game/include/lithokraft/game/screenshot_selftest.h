// Screenshot self-test runner header. LF line endings.
#pragma once

namespace lithokraft {

/// Runs the screenshot self-test (command parsing, capture pipeline, file
/// helpers, Fragment gallery round-trip). Returns the failure count.
int runScreenshotSelfTest();

}  // namespace lithokraft
