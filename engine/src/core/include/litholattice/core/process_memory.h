// Process memory for perf logs: working set / resident size in bytes (0 if unknown).
#pragma once
#include <cstdint>

namespace litholattice {
uint64_t processWorkingSetBytes();
}  // namespace litholattice
