// Frame pacer: an optional frame-rate cap for the render loop
// (P3-3, Appendix D M3).
//
// WHAT: FramePacer is a deadline pacer. The host calls beginFrame() at the
//   top of each frame and endFrameAndWait() after presenting; when a cap
//   is set, the latter sleeps until the frame's deadline (start + 1/cap).
//   Cap 0 (the default) disables pacing entirely — endFrameAndWait() is a
//   no-op and vsync remains the only pacing, exactly as before.
// WHY: the review verified there was NO frame-rate limiter anywhere: with
//   vsync off the main loop spins at full rate (the software rasterizer
//   busy-burns a core at 100%). JD's perf doctrine (2026-10-12): pacing
//   and frame-time variance matter, raw FPS does not — so the cap is a
//   pacing tool, default OFF, never a sim input (it touches wall-clock
//   presentation only; sim determinism is untouched).
// HOW: header-only, monotonic clock. A frame that overruns its deadline
//   does NOT try to catch up (the next deadline re-bases on that frame's
//   start + period, clamped to "now"), so a hitch costs one late frame,
//   not a spiral of short sleeps.
//
// INTEGRATION (remaining wiring, game-owned files): the game registers a
// `render.fps_cap` cvar (integer FPS, 0 = off, clamp 0..1000) beside the
// other render cvars in settings_cvars.cpp, stores it in Settings, and
// drives one FramePacer from the main loop in main.cpp:
// pacer.setCapFps(settings.fpsCap) when the cvar changes, beginFrame()
// at loop top, endFrameAndWait() right after present/swap. Until that
// wiring lands, this class is engine-tested but inert in the game.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

namespace litholattice {

class FramePacer {
 public:
  using Clock = std::chrono::steady_clock;

  /// Set the cap in frames per second. 0 (or negative) = uncapped
  /// (default). Values above 1000 are clamped (sub-millisecond deadlines
  /// are below sleep granularity on every supported platform).
  void setCapFps(int fps) {
    capFps_ = std::clamp(fps, 0, 1000);
    period_ = capFps_ > 0 ? std::chrono::duration_cast<Clock::duration>(
                                std::chrono::duration<double>(1.0 / capFps_))
                          : Clock::duration::zero();
  }
  int capFps() const { return capFps_; }
  bool enabled() const { return capFps_ > 0; }

  /// Mark the start of a frame. Call once per frame, before rendering.
  void beginFrame(Clock::time_point now = Clock::now()) { frameStart_ = now; }

  /// After presenting: sleep until frameStart_ + period. Returns the time
  /// actually slept (zero when uncapped, when beginFrame was never
  /// called, or when the frame already overran its deadline).
  Clock::duration endFrameAndWait(Clock::time_point now = Clock::now()) {
    if (!enabled() || frameStart_ == Clock::time_point{}) return Clock::duration::zero();
    const auto deadline = frameStart_ + period_;
    if (now >= deadline) return Clock::duration::zero();  // overrun: no catch-up debt
    std::this_thread::sleep_until(deadline);
    return deadline - now;
  }

 private:
  int capFps_ = 0;  ///< DEFAULT OFF (JD perf doctrine: vsync-only unless asked)
  Clock::duration period_{};
  Clock::time_point frameStart_{};
};

}  // namespace litholattice
