#include "lithokraft/game/sound_director.h"
#include <algorithm>
#include <cmath>
#include "lithokraft/content/music.h"
#include "lithokraft/content/sfx.h"
#include "litholattice/world/chunk.h"

namespace lithokraft {
namespace {

float distanceGain(const SoundEvent& e, const Vec3& l) {
  const float dx = e.x + 0.5f - l.x, dy = e.y + 0.5f - l.y, dz = e.z + 0.5f - l.z;
  const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
  return std::clamp(1.f - (d - 4.f) / (SoundDirector::kHearing - 4.f), 0.f, 1.f);
}

/// Tone Block voice: SID3 pulse 25% with a short attack and a held sustain.
NoteSpec toneNote(int strength, float gain) {
  return {noteFrequency(noteForStrength(strength)), Waveform::Pulse, Duty::Quarter, {0.005f, 0.15f, 0.6f, 0.12f}, 0.45f * gain};
}

}  // namespace

SoundDirector::SoundDirector(AudioEngine& audio) : audio_(audio), tones_(audio.music().voiceCount()) {}

void SoundDirector::play(std::span<const SoundEvent> events, const Vec3& listener) {
  for (const SoundEvent& e : events) {
    const float gain = distanceGain(e, listener);
    switch (e.cue) {
      case SoundCue::Place: audio_.post(sfxCommand(Sfx::Place, gain)); break;
      case SoundCue::Click: audio_.post(sfxCommand(Sfx::Click, gain)); break;
      case SoundCue::PulsorFire: audio_.post(sfxCommand(Sfx::PulsorFire, gain, e.value)); break;
      case SoundCue::Break:
        audio_.post(sfxCommand(Sfx::Break, gain));
        playTone({SoundCue::Tone, 0, e.x, e.y, e.z}, 0.f);  // a broken Tone Block stops sounding
        break;
      case SoundCue::Tone: playTone(e, gain); break;
    }
  }
}

void SoundDirector::playTone(const SoundEvent& e, float gain) {
  const uint64_t key = uint64_t(packPos(e.x, e.y, e.z));
  if (e.value > 0) {
    audio_.post(AudioCommand::noteOn(Bus::Music, tones_.acquire(key), toneNote(e.value, gain)));
  } else if (const auto voice = tones_.release(key)) {
    audio_.post(AudioCommand::noteOff(Bus::Music, *voice));
  }
}

void SoundDirector::stopAll() {
  for (size_t v = 0; v < audio_.music().voiceCount(); ++v) audio_.post(AudioCommand::noteOff(Bus::Music, v));
  tones_.clear();
}

}  // namespace lithokraft
