// lithokraft_fragment_selftest: headless Fragment state tests (no window,
// no GL context). Exit code = failure count. LF line endings.
#include <cstdio>

#include "lithokraft/game/fragment_selftest.h"

int main() {
  const int fails = lithokraft::runFragmentSelfTest();
  std::printf("exit: %d\n", fails);
  return fails ? 1 : 0;
}
