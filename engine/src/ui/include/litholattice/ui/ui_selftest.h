// Headless checks of the ui module (--selftest): layout, theme JSON, focus and keyboard
// navigation, text editing, widget events, popups / modals, the JSON widget factory.
#pragma once

namespace litholattice::ui {
/// Prints one PASS / FAIL line per check; returns the number of failures.
int runUiSelfTest();
}  // namespace litholattice::ui
