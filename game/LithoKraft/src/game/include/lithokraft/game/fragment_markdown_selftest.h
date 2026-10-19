// Fragment markdown renderer self-test runner header. LF line endings.
#pragma once

namespace lithokraft {

/// Runs the Fragment markdown renderer self-test (extraction, every block
/// and inline element, edge cases, view modes, checklist toggle).
/// Returns the failure count.
int runFragmentMarkdownSelfTest();

}  // namespace lithokraft
