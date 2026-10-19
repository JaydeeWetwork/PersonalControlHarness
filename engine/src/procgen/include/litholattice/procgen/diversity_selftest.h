// Ecosystem diversity self-test: ground cover, rock formations, biome transitions,
// Worley ore veins, fauna hooks, dimension palettes (JSON-only), snow caps, sand drifts.
// Every feature: JSON loads, placement sanity, determinism (two runs, identical buffers).
#pragma once

namespace litholattice::procgen {
/// Prints one PASS / FAIL line per check; returns the number of failures.
int runDiversitySelfTest();
}  // namespace litholattice::procgen
