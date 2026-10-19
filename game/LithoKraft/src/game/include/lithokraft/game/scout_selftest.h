// Scout self-test: the `locatebiome` / `genmap` lithoscript commands (parsing,
// the spiral biome search, the overview-map core, the performance guard) plus the
// Wafer structure framework integration (determinism, chunk-local safety,
// never-overwrite, marker agreement). Headless: no GL context, no window.
// Uses the real Wafer generator JSON (built-in) and the game block registry.
// LF line endings.
#pragma once

namespace lithokraft {

/// Runs the scout self-test; returns the failure count.
int runScoutSelfTest();

}  // namespace lithokraft
