// Headless checks of the index module: inverted-index basics, document
// replacement, removal, UTF-8 tokenization safety, empty-query behavior,
// query determinism, the maxResults cap, and a latency sanity check.
#pragma once

namespace litholattice::index {
/// Prints one PASS / FAIL line per check; returns the number of failures.
int runIndexSelfTest();
}  // namespace litholattice::index
