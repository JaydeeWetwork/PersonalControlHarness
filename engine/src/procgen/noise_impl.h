// Private to procgen: factories for the base noise types and shared lattice helpers.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include "litholattice/procgen/noise.h"
#include "litholattice/procgen/seed.h"

namespace litholattice::procgen::detail {

std::unique_ptr<INoise> makePerlin(uint64_t seed);
std::unique_ptr<INoise> makeOpenSimplex2(uint64_t seed, bool smooth);
std::unique_ptr<INoise> makeValue(uint64_t seed);
std::unique_ptr<INoise> makeCellular(const NoiseSettings& settings);
std::unique_ptr<INoise> makeWhite(uint64_t seed);

/// floor() as int, correct for negative inputs.
inline int32_t fastFloor(float v) {
  const int32_t i = int32_t(v);
  return v < float(i) ? i - 1 : i;
}

/// Quintic fade 6t^5 - 15t^4 + 10t^3 (C2 continuous).
inline float quintic(float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }
/// Cubic smoothstep 3t^2 - 2t^3 (C1 continuous).
inline float smooth(float t) { return t * t * (3.f - 2.f * t); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

/// Clamps a result to [-1, 1] (guards the documented range against rounding at the extremes).
inline float clampUnit(float v) { return std::clamp(v, -1.f, 1.f); }

}  // namespace litholattice::procgen::detail
