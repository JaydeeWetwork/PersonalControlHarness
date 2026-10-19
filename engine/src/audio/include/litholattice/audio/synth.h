// A polyphonic retro synth: a fixed set of voices laid out by a chip-style preset.
#pragma once
#include <span>
#include <string_view>
#include <vector>
#include "litholattice/audio/voice.h"
#include "litholattice/audio/simd_mix.h"

namespace litholattice {

/// What one voice of a preset can play.
struct VoiceSlot {
  std::string_view role;  ///< e.g. "pulse 1", "wave", "noise"
  uint8_t waveforms;      ///< mask of waveformBit()s the slot supports
};

/// Voice layout of a synth.
struct SynthPreset {
  std::string_view name;
  std::vector<VoiceSlot> voices;

  /// SID-style: 3 voices, each pulse (4 duties) / triangle / saw / noise with full ADSR.
  static SynthPreset sid3();
  /// Game Boy style: pulse 1, pulse 2, 4-bit wave table, LFSR noise.
  static SynthPreset dmg();
};

/// Owns the voices of one preset and mixes them.
class Synth {
 public:
  Synth(SynthPreset preset, float sampleRate);

  /// Starts a note on `voice`; a waveform the slot cannot play falls back to its first supported one.
  void noteOn(size_t voice, NoteSpec note);
  void noteOff(size_t voice);

  /// Virtualizes `voice`: inaudible (beyond hearing range / fully occluded /
  /// zero gain). A virtualized voice keeps its timeline - the envelope,
  /// phase, noise and sweep keep advancing via Voice::advance() - but its
  /// samples are never mixed, so it costs a fraction of a live voice.
  /// Un-virtualizing resumes bit-identically. Audio thread only (drive it
  /// through AudioCommand::virtualize from the game thread).
  void setVirtualized(size_t voice, bool v);
  bool virtualized(size_t voice) const {
    return voice < virtual_.size() && virtual_[voice] != 0;
  }
  size_t virtualizedCount() const;

  /// Render resolution divider for graceful degradation level 3:
  /// 1 = full rate (default), 2 = half rate (voices render every other
  /// sample and silently advance the skipped one; output is zero-order
  /// held back to full rate). Audio thread only.
  void setRateDivider(int d) { rateDivider_ = d == 2 ? 2 : 1; }
  int rateDivider() const { return rateDivider_; }

  /// Adds `out.size()` mono samples of every voice into `out`.
  /// Each voice renders into a scratch block and is accumulated with the
  /// SIMD addInto() (bit-identical to the old per-sample loop).
  void mixInto(std::span<float> out);

  size_t voiceCount() const { return voices_.size(); }
  const Voice& voice(size_t i) const { return voices_[i]; }
  Voice& voice(size_t i) { return voices_[i]; }
  const SynthPreset& preset() const { return preset_; }

 private:
  SynthPreset preset_;
  std::vector<Voice> voices_;
  std::vector<char> virtual_;   // per-voice virtualization flag (audio thread only)
  std::vector<float> scratch_;  // per-voice render scratch for the SIMD batch mix
  int rateDivider_ = 1;
};

}  // namespace litholattice
