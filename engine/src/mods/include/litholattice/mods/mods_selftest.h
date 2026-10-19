// Headless checks of the mods module (--selftest): IExtensible facet query,
// dotted identity paths, the extension tree, and depth-cap enforcement.
#pragma once

namespace litholattice::mods {
/// Prints one PASS / FAIL line per check; returns the number of failures.
int runModsSelfTest();
}  // namespace litholattice::mods
