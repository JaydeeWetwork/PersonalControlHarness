// McpTokenBucket implementation: continuous fractional refill from a
// monotonic millisecond clock. Clean-room.
#include "litholattice/mcp/rate_limit.h"

#include <chrono>

namespace litholattice::mcp {

int64_t McpTokenBucket::steadyClockMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

McpTokenBucket::McpTokenBucket(uint64_t ratePerSec, uint64_t burst, ClockFn clock)
    : ratePerSec_(ratePerSec), burst_(burst), clock_(std::move(clock)), tokens_(static_cast<double>(burst)) {}

void McpTokenBucket::refill(int64_t nowMs) {
  if (!started_) {
    started_ = true;
    lastMs_ = nowMs;
    return;
  }
  if (nowMs <= lastMs_) return;  // clock did not advance (or went backwards): no refill, no penalty
  const double elapsed = static_cast<double>(nowMs - lastMs_);
  tokens_ += elapsed * static_cast<double>(ratePerSec_) / 1000.0;
  if (tokens_ > static_cast<double>(burst_)) tokens_ = static_cast<double>(burst_);
  lastMs_ = nowMs;
}

bool McpTokenBucket::take(uint64_t n) {
  refill(clock_());
  if (static_cast<double>(n) > tokens_) return false;
  tokens_ -= static_cast<double>(n);
  return true;
}

uint64_t McpTokenBucket::balance() {
  refill(clock_());
  return static_cast<uint64_t>(tokens_);
}

void McpTokenBucket::setClock(ClockFn clock) {
  clock_ = std::move(clock);
  started_ = false;  // re-stamp on the next take()/balance()
}

}  // namespace litholattice::mcp
