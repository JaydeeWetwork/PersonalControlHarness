// lithokraft_markdown_selftest: headless Fragment markdown renderer tests
// (no window, no GL context). Exit code = failure count. LF line endings.
#include <cstdio>

#include "lithokraft/game/fragment_markdown_selftest.h"

int main() {
  const int fails = lithokraft::runFragmentMarkdownSelfTest();
  std::printf("exit: %d\n", fails);
  return fails ? 1 : 0;
}
