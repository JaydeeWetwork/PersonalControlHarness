// Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
// concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
// from the mathematical definition; no third-party code referenced. Underlying patent
// expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
// ambition of No Man's Sky (Hello Games).

// Self-test entry point for the Litho Formula voxelizer
// (litho_formula_voxelize_selftest.cpp). Wired into runProcgenSelfTest.
#pragma once

namespace litholattice::procgen::litho_formula {

/// Runs the voxelizer acceptance checks (sphere volume, watertight fill,
/// resolution handling, star-smoke, determinism). Prints one PASS/FAIL line
/// per check; returns the number of failures.
int runLithoFormulaVoxelizeSelfTest();

}  // namespace litholattice::procgen::litho_formula
