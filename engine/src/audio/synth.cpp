#include "litholattice/audio/synth.h"
#include <bit>

namespace litholattice {

SynthPreset SynthPreset::sid3() {
  constexpr uint8_t kAll = waveformBit(Waveform::Pulse) | waveformBit(Waveform::Triangle) |
                           waveformBit(Waveform::Saw) | waveformBit(Waveform::Noise);
  return {"SID3", {{"voice 1", kAll}, {"voice 2", kAll}, {"voice 3", kAll}}};
}

SynthPreset SynthPreset::dmg() {
  return {"DMG",
          {{"pulse 1", waveformBit(Waveform::Pulse)},
           {"pulse 2", waveformBit(Waveform::Pulse)},
           {"wave", waveformBit(Waveform::Wave)},
           {"noise", waveformBit(Waveform::Noise)}}};
}

Synth::Synth(SynthPreset preset, float sampleRate)
    : preset_(std::move(preset)),
      voices_(preset_.voices.size(), Voice(sampleRate)),
      virtual_(preset_.voices.size(), 0) {}

void Synth::setVirtualized(size_t voice, bool v) {
  if (voice < virtual_.size()) virtual_[voice] = v ? 1 : 0;
}

size_t Synth::virtualizedCount() const {
  size_t n = 0;
  for (char v : virtual_) n += size_t(v != 0);
  return n;
}

void Synth::noteOn(size_t voice, NoteSpec note) {
  if (voice >= voices_.size()) return;
  const uint8_t allowed = preset_.voices[voice].waveforms;
  if (!(allowed & waveformBit(note.waveform))) note.waveform = Waveform(std::countr_zero(allowed));
  voices_[voice].noteOn(note);
  virtual_[voice] = 0;  // a fresh note starts audible; re-virtualize explicitly if still inaudible
}

void Synth::noteOff(size_t voice) {
  if (voice < voices_.size()) voices_[voice].noteOff();
}

void Synth::mixInto(std::span<float> out) {
  const size_t n = out.size();
  if (scratch_.size() < n) scratch_.resize(n);
  for (size_t vi = 0; vi < voices_.size(); ++vi) {
    Voice& v = voices_[vi];
    if (!v.active()) continue;
    if (virtual_[vi]) {
      // Virtualized: the timeline advances, nothing is mixed.
      for (size_t i = 0; i < n; ++i) v.advance();
      continue;
    }
    if (rateDivider_ == 1) {
      for (size_t i = 0; i < n; ++i) scratch_[i] = v.render();
      simd::addInto(out.data(), scratch_.data(), n);
    } else {
      // Degradation level 3: render at half resolution. The skipped
      // samples still advance the timeline (pitch/timing stay exact),
      // then the output is zero-order-held back to full rate: mild
      // aliasing instead of a dropout.
      const size_t half = n / 2;
      for (size_t i = 0; i < half; ++i) {
        scratch_[i] = v.render();
        v.advance();
      }
      if (n & 1u) scratch_[half] = v.render();
      simd::addIntoDuplicated(out.data(), scratch_.data(), n);
    }
  }
}

}  // namespace litholattice
