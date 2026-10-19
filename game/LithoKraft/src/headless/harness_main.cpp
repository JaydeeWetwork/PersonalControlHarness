// Headless game harness entry point: runs the harness self-tests, exit code = failures.
// CI runs this as the `lithokraft_headless_harness` ctest.
#include <cstdio>
#include "headless_harness.h"

int main() {
  const int fails = lithokraft::runHeadlessHarnessSelfTest();
  std::printf("headless harness: %s (%d failure%s)\n", fails == 0 ? "OK" : "FAILED", fails,
              fails == 1 ? "" : "s");
  return fails == 0 ? 0 : 1;
}
