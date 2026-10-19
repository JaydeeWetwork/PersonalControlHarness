// Effect chain for the audio engine: a small Schroeder-style reverb used as an
// insert on the effects bus, plus the bypass policy that skips it when the
// sound cannot be heard or is too far away to matter.
//
// BYPASS THRESHOLD (audibility reasoning):
//   kEffectBypassDistance = 48 blocks.
//   The spatial model attenuates as 1/(1+d/16): at 48 blocks the direct sound
//   sits at 25% (-12 dB) of its close-range level. A reverb wet send is mixed
//   roughly 12-18 dB below the direct signal, so beyond 48 blocks the entire
//   tail lands around -24 to -30 dB - under the simultaneous-masking floor of
//   game audio at normal listening levels, and well past the 32-block
//   occlusion-field radius where spatial answers already go conservative.
//   Skipping the filter chain there saves ~12 flops per sample with no
//   perceptible difference. Virtualized voices contribute exactly zero to the
//   mix, so their effect sends are always skipped.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace litholattice {

/// Voices farther than this (in blocks) skip the effect chain: their reverb
/// tail is below the masking floor (see the reasoning above).
constexpr float kEffectBypassDistance = 48.0f;

/// Bypass policy: skip reverb/effect processing when the voice is virtualized
/// (inaudible by definition) or beyond kEffectBypassDistance.
inline bool effectBypassed(bool virtualized, float distanceBlocks) {
  return virtualized || distanceBlocks > kEffectBypassDistance;
}

/// Small Schroeder-style reverb: two feedback combs in parallel, one allpass
/// in series. All delay lines are allocated in the constructor; process()
/// never allocates and never locks, so it is safe on the audio thread.
/// Enable/bypass are driven by the AudioEngine's atomics (game thread writes,
/// audio thread reads); every other method is audio-thread only.
class EffectChain {
 public:
  explicit EffectChain(float sampleRate = 48000.f);

  /// 0 = dry (passthrough), 1 = fully wet. Default 0.35.
  void setWet(float wet);
  /// In-place mono processing of `frames` samples.
  void process(float* io, size_t frames);
  /// Flush delay lines (call on teleport / scene change).
  void clear();
  /// Frames actually filtered. Atomic so the game thread can prove the bypass
  /// path was taken (stays 0 while bypassed).
  uint64_t framesProcessed() const { return framesProcessed_.load(std::memory_order_relaxed); }

 private:
  struct Comb {
    std::vector<float> buf;
    size_t idx = 0;
    float feedback = 0.84f;
    float damp = 0.2f;
    float filter = 0.f;
    float tick(float in);
  };
  struct Allpass {
    std::vector<float> buf;
    size_t idx = 0;
    float feedback = 0.5f;
    float tick(float in);
  };
  float sampleRate_;
  float wet_ = 0.35f;
  Comb combA_, combB_;
  Allpass ap_;
  std::atomic<uint64_t> framesProcessed_{0};
};

}  // namespace litholattice
