// Headless checks of procgen (--selftest): noise ranges and continuity, determinism across
// threads, graph JSON round trip and validation, biome selection, pipeline stages.
#pragma once
#include "litholattice/core/threading.h"

namespace litholattice::procgen {
/// Prints one PASS / FAIL line per check; returns the number of failures.
int runProcgenSelfTest(JobPool& pool);
/// Structure registry, placement solver, structure block (struct_registry_selftest.cpp).
int runStructRegistrySelfTest();
}  // namespace litholattice::procgen
