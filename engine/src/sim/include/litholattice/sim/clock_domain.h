// Clock domains: groups of game logic that tick at their own power-of-two rate (the game declares
// them in SimConfig; LithoKraft: lithographed chips, e.g. EUV at 512 Hz). The tick scheduler drives
// all of them from one master clock with integer dividers. The per-domain logic is a stub so far.
#pragma once
#include <cstdint>
#include <string_view>

namespace litholattice {

constexpr int kMaxDomainHz = 512;  // highest supported domain rate

// A clock domain = logic ticking at one rate. All rates are powers of two, so the sim runs one
// master clock = max(rate) and drives each domain with an integer divider. STUB: no logic yet.
struct ClockDomain {
  std::string_view name;
  int hz = 0;
  bool enabled = true;
  uint64_t ticks = 0, windowTicks = 0;
  float achievedHz = 0.f;
  uint64_t state = 0;  // placeholder for the compiled chip's state vector
  void step() { state = state * 6364136223846793005ULL + 1442695040888963407ULL; }
};

}  // namespace litholattice
