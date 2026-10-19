// lithokraft_scout_selftest: headless scout tests (no window, no GL context).
// Exit code = failure count. LF line endings.
#include <cstdio>

#include "lithokraft/game/scout_selftest.h"

int main() {
  const int fails = lithokraft::runScoutSelfTest();
  std::printf("exit: %d\n", fails);
  return fails ? 1 : 0;
}
