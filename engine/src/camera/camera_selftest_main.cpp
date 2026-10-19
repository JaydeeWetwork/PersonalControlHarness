// Standalone runner for the camera module self-test (ctest: engine_camera).
#include <cstdio>

#include "litholattice/camera/camera_selftest.h"

int main() {
  const int fails = litholattice::camera::runCameraSelfTest();
  std::printf("litholattice_camera_selftest: %s\n", fails ? "FAILED" : "OK");
  return fails ? 1 : 0;
}
