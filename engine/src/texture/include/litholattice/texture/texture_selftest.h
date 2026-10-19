#pragma once
// Headless checks for the texture module (recipe parse/round-trip,
// determinism, seamless tiling, cache, naive-vs-optimised benchmarks).
namespace litholattice {
int runTextureSelfTest();  // returns failure count
}  // namespace litholattice
