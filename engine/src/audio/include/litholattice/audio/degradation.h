// Graceful degradation under CPU pressure.
//
// WHAT: the audio callback measures its own wall time against its budget
// (frames / sampleRate) and sheds load in three escalating stages instead of
// glitching when the machine cannot keep up:
//   level 1 - virtualize aggressively: release-stage tails below
//             quietThreshold keep their timeline but skip mixing (inaudible
//             by construction, so nothing is lost).
//   level 2 - reduce voice count: voices at index >= half the pool are
//             virtualized. Timelines preserved; half the mix cost.
//   level 3 - halve render resolution: voices render every other sample and
//             silently advance the skipped sample, then the output is
//             zero-order-held back to full rate. Timeline and pitch stay
//             exact; the cost is mild aliasing instead of a dropout.
// WHY: a missed audio deadline is a click/pop the player hears. Each stage
// trades something inaudible-or-tolerable for CPU, in order of increasing
// audible cost, and every stage preserves voice timelines (virtualization),
// so recovery is seamless.
//
// HYSTERESIS: escalation needs `escalateAfter` consecutive over-threshold
// callbacks; recovery needs `recoverAfter` consecutive callbacks under half
// the warn threshold, so the level does not flap. With no pressure the level
// stays 0 and nothing changes - the default policy reproduces current
// behavior exactly. All thresholds are configurable via DegradationPolicy.
#pragma once
#include <chrono>

namespace litholattice {

struct DegradationPolicy {
  float warnLoad = 0.70f;        ///< sustained load above this -> level 1
  float criticalLoad = 0.90f;    ///< sustained load above this -> level 2
  float overloadLoad = 1.00f;    ///< sustained load above this -> level 3
  int escalateAfter = 8;         ///< consecutive over-threshold callbacks before escalating
  int recoverAfter = 64;         ///< consecutive calm callbacks before de-escalating one level
  float quietThreshold = 0.02f;  ///< level 1: virtualize release tails below this effective level
};

/// Tracks callback load vs budget and reports the current shed level (0..3).
/// Deterministic and allocation-free: safe to drive from the audio thread, a
/// test, or the game thread. The AudioEngine owns one and applies the actions.
class LoadShedder {
 public:
  explicit LoadShedder(DegradationPolicy policy = {}) : policy_(policy) {}

  void setPolicy(const DegradationPolicy& p) { policy_ = p; }
  const DegradationPolicy& policy() const { return policy_; }

  /// elapsed = measured callback time, budget = frames/sampleRate for the callback.
  /// Returns the new shed level (0..3).
  int update(std::chrono::nanoseconds elapsed, std::chrono::nanoseconds budget) {
    const double load =
        budget.count() > 0 ? double(elapsed.count()) / double(budget.count()) : 0.0;
    lastLoad_ = load;
    const int want = load >= policy_.overloadLoad     ? 3
                     : load >= policy_.criticalLoad   ? 2
                     : load >= policy_.warnLoad       ? 1
                                                     : 0;
    if (want > level_) {
      underCount_ = 0;
      if (++overCount_ >= policy_.escalateAfter) {
        level_ = want;
        overCount_ = 0;
      }
    } else {
      overCount_ = 0;
      if (want < level_ && load < double(policy_.warnLoad) * 0.5) {
        if (++underCount_ >= policy_.recoverAfter) {
          --level_;
          underCount_ = 0;
        }
      } else {
        underCount_ = 0;
      }
    }
    return level_;
  }

  int level() const { return level_; }
  double lastLoad() const { return lastLoad_; }
  void reset() {
    level_ = 0;
    overCount_ = 0;
    underCount_ = 0;
    lastLoad_ = 0.0;
  }

 private:
  DegradationPolicy policy_;
  int level_ = 0;
  int overCount_ = 0, underCount_ = 0;
  double lastLoad_ = 0.0;
};

}  // namespace litholattice
