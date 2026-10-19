// Headless checks of the console module: the engine-log tap, lithoscript
// (commands, typed cvars, aliases, exec sandbox, `;` sequences, tokenizer),
// the AngelScript exposure (permission-gated), and the dropdown
// DebugConsole (tilde toggle, confinement, history).
#pragma once

namespace litholattice::console {
/// Prints one PASS / FAIL line per check; returns the number of failures.
int runConsoleSelfTest();
}  // namespace litholattice::console
