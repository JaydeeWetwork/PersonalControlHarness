// Per-connection rate limiting for the MCP server (Phase 4, 2026-10-11).
//
// One mechanism (token bucket), two caps:
//   1. tool-call cap: every tools/call on the server consumes one token.
//      Exceeded -> the call is refused with an isError result naming the
//      limit (never a JSON-RPC protocol error).
//   2. write cap: every world_set_block consumes one token. Same shape.
//
// Why a token bucket: the burst allowance absorbs legitimate batch use (an
// agent placing a wall of blocks, a host listing then calling tools), while
// the sustained rate bounds a runaway client. All constants are powers of
// two (JD's convention); the numbers are documented in MCP-GAME-SEAM.md §9.
//
// Survey note (locked): mod_interface.h documents kPermWorldWrite as
// "setBlock (rate-limited)", but no reusable rate-limiter class exists in
// the engine — the closest precedents are the token buckets in
// net/confinement.h (per-mod bytes/sec + packets/sec) and the per-mod
// CPU/memory quota state machines. So the MCP module ships its own minimal
// bucket following that pattern rather than pretending to reuse one.
//
// Threading: one bucket per McpServer, and McpServer dispatch is
// single-threaded (see server.h layering), so the bucket is NOT internally
// synchronized — the server serializes every call.
//
// Time: a game-provided monotonic millisecond clock, fakeable in tests
// (same discipline as net/confinement.h). Never wall-clock-dependent logic
// beyond the clock itself: refills are computed from elapsed ms, so the sim
// stays deterministic — rate limiting is an I/O-edge policy, not sim state.
#pragma once

#include <cstdint>
#include <functional>

namespace litholattice::mcp {

/// Sustained tool-call rate per connection (per session on HTTP, per the
/// shared identity for stateless traffic). Generous on purpose: read tools
/// are cheap; the write tools carry their own tighter bucket below.
inline constexpr uint64_t kMcpToolCallLimitPerSec = 256;
/// Burst allowance for tool calls: absorbs a host's list-then-call batch.
inline constexpr uint64_t kMcpToolCallBurst = 512;

/// Sustained world_set_block rate per connection. Block placement is the
/// most abuse-sensitive write: 32/sec sustained lets a building agent work
/// steadily without letting a runaway client rewrite the world at wire
/// speed.
inline constexpr uint64_t kMcpBlockWriteLimitPerSec = 32;
/// Burst allowance for block writes: one 64-block batch per refill window.
inline constexpr uint64_t kMcpBlockWriteBurst = 64;

/// Sustained player-control rate per connection. Control frames are consumed
/// at the game's frame pump (the natural limiter: one staged batch per
/// frame), but the tool-call path needs its own cap so a model cannot
/// vibrate the player by spamming press/release pairs at wire speed.
/// 120/sec sustained sits above any real frame rate and far below abuse.
inline constexpr uint64_t kMcpControlLimitPerSec = 120;
/// Burst allowance for control frames: absorbs a model's think-act batch.
inline constexpr uint64_t kMcpControlBurst = 240;

/// Sustained vision rate per pilot: rendered frames are the most expensive
/// thing an agent can request (a full software-renderer pass each). 4/sec
/// keeps the see-think-act loop alive without melting the machine.
inline constexpr uint64_t kMcpVisionLimitPerSec = 4;
/// Burst allowance for vision: a couple of quick re-frames after acting.
inline constexpr uint64_t kMcpVisionBurst = 8;

/// Sustained audio rate per pilot: 8 chunks/sec at tier 0 (1024 frames @
/// 8 kHz = 128 ms per chunk) is full coverage with headroom; higher tiers
/// carry more seconds per chunk, so the same call rate stays bounded.
inline constexpr uint64_t kMcpAudioLimitPerSec = 8;
/// Burst allowance for audio chunks.
inline constexpr uint64_t kMcpAudioBurst = 16;

/// How long world_set_block waits for the sim thread to apply the write
/// before failing the call (isError, never a hang). A few ticks at any
/// supported TPS is tens of ms; 2 s is generous headroom for a loaded sim.
inline constexpr int kMcpSimWriteTimeoutMs = 2000;

/// Classic token bucket. `take(n)` consumes n tokens when available and
/// returns true; otherwise it consumes nothing and returns false. Tokens
/// refill continuously at ratePerSec up to `burst` (the bucket starts full).
class McpTokenBucket {
 public:
  /// Monotonic milliseconds (std::chrono::steady_clock in production, a fake
  /// in tests).
  using ClockFn = std::function<int64_t()>;

  McpTokenBucket(uint64_t ratePerSec, uint64_t burst, ClockFn clock = steadyClockMs);

  /// Consumes n tokens when the bucket holds them (true); otherwise false
  /// with the bucket unchanged. n == 0 always succeeds.
  bool take(uint64_t n = 1);
  /// Current whole-token balance (after refilling to now).
  uint64_t balance();
  /// Replaces the clock (tests). Resets the last-refill stamp to the new
  /// clock's now so a clock swap never grants a spurious refill.
  void setClock(ClockFn clock);

  uint64_t ratePerSec() const { return ratePerSec_; }
  uint64_t burst() const { return burst_; }

  /// Default production clock: steady_clock milliseconds since an arbitrary
  /// epoch. Monotonic, never wall-clock.
  static int64_t steadyClockMs();

 private:
  void refill(int64_t nowMs);

  uint64_t ratePerSec_;
  uint64_t burst_;
  ClockFn clock_;
  double tokens_;      ///< fractional: refills are continuous, take() is whole
  int64_t lastMs_ = 0;
  bool started_ = false;
};

}  // namespace litholattice::mcp
