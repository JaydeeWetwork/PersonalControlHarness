// Camera module self-test entry point.
#pragma once

namespace litholattice::camera {

/// Runs the camera module checks. Returns the failure count.
int runCameraSelfTest();

}  // namespace litholattice::camera
