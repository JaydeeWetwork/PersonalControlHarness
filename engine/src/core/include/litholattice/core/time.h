#pragma once
#include <chrono>

namespace litholattice {
using Clock = std::chrono::steady_clock;
inline double secondsBetween(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }
inline double msBetween(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }
}  // namespace litholattice
