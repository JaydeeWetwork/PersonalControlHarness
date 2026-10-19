// Headless checks of the game's UI integration (--selftest): HUD draws through HudUiRenderer
// (ordering, clipping), the AngelScript UI bindings, the script editor's save + hot reload,
// and the HUD / hotbar / inventory screens.
#pragma once
#include "lithokraft/game/engine.h"

namespace lithokraft {
/// Prints one PASS / FAIL line per check; returns the number of failures.
int runGameUiSelfTest();
}  // namespace lithokraft
