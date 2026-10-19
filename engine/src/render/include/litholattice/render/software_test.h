// Standalone software-renderer smoke test (renders a real quad through
// renderMesh and asserts on the pixels; home of the model-dither, AA
// ladder, ASCII and Systerian checks, JD 2026-10-13). Defined in
// render/software_test.cpp; wired into litholattice_tests (ctest
// engine_render) — before 2026-10-13 the TU was compiled nowhere and
// its checks silently never ran.
#pragma once

namespace litholattice {

/// Returns 0 on success, 1 on the first failed check (prints its own
/// FAIL lines as it goes).
int testSoftwareRenderer();

}  // namespace litholattice
