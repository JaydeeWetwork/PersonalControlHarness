// Anim module self-test entry point.
#pragma once

namespace litholattice::anim {

/// Runs the anim module checks: rig generation (Worker 1) + IK solvers (Worker 2).
/// Returns the failure count.
int runAnimSelfTest();

}  // namespace litholattice::anim
