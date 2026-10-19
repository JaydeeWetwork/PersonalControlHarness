// lithokraft_screenshot_selftest: headless screenshot tests (no window, no
// GL context). Exit code = failure count. LF line endings.
#include <cstdio>

#include "lithokraft/game/screenshot_selftest.h"

int main() {
  const int fails = lithokraft::runScreenshotSelfTest();
  std::printf("exit: %d\n", fails);
  return fails ? 1 : 0;
}
