#include "litholattice/core/process_memory.h"
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <cstdio>
#endif

namespace litholattice {

uint64_t processWorkingSetBytes() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return uint64_t(pmc.WorkingSetSize);
  return 0;
#else
  std::FILE* f = std::fopen("/proc/self/statm", "r");
  if (!f) return 0;
  unsigned long pages = 0, resident = 0;
  const int n = std::fscanf(f, "%lu %lu", &pages, &resident);
  std::fclose(f);
  return n == 2 ? uint64_t(resident) * 4096u : 0;
#endif
}

}  // namespace litholattice
