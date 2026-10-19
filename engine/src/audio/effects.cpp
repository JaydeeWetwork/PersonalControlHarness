// Schroeder-style reverb: two feedback combs in parallel, one allpass in series.
// All delay lines are sized once in the constructor; process() is
// allocation-free and lock-free (audio-thread safe).
#include "litholattice/audio/effects.h"

#include <algorithm>

namespace litholattice {

float EffectChain::Comb::tick(float in) {
  const float out = buf[idx];
  filter = out * (1.f - damp) + filter * damp;  // lowpass in the feedback loop
  buf[idx] = in + filter * feedback;
  idx = (idx + 1) % buf.size();
  return out;
}

float EffectChain::Allpass::tick(float in) {
  const float bufOut = buf[idx];
  const float out = -in + bufOut;
  buf[idx] = in + bufOut * feedback;
  idx = (idx + 1) % buf.size();
  return out;
}

EffectChain::EffectChain(float sampleRate) : sampleRate_(sampleRate) {
  // Prime-ish delays (ms) at 48 kHz; scaled by the real sample rate.
  auto make = [&](float ms) {
    return std::vector<float>(size_t(sampleRate_ * ms / 1000.f) + 1, 0.f);
  };
  combA_.buf = make(29.7f);
  combB_.buf = make(37.1f);
  ap_.buf = make(11.3f);
}

void EffectChain::setWet(float wet) { wet_ = std::clamp(wet, 0.f, 1.f); }

void EffectChain::process(float* io, size_t frames) {
  if (wet_ <= 0.f) return;  // dry passthrough: no CPU, no counter
  for (size_t i = 0; i < frames; ++i) {
    const float dry = io[i];
    const float rev = ap_.tick((combA_.tick(dry) + combB_.tick(dry)) * 0.5f);
    io[i] = dry * (1.f - wet_) + rev * wet_;
  }
  framesProcessed_.fetch_add(frames, std::memory_order_relaxed);
}

void EffectChain::clear() {
  for (auto* b : {&combA_.buf, &combB_.buf, &ap_.buf})
    std::fill(b->begin(), b->end(), 0.f);
  combA_.filter = 0.f;
  combA_.idx = combB_.idx = ap_.idx = 0;
}

}  // namespace litholattice
