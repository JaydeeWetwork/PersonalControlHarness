// Turns sim sound events into audio commands: procedural SFX on the effects bus, Tone Blocks on
// the SID3 music bus (one voice per sounding block, oldest stolen when more than three play).
#pragma once
#include "lithokraft/game/engine.h"
#include <span>
#include <vector>
#include "litholattice/audio/audio_engine.h"
#include "litholattice/audio/voice_allocator.h"
#include "litholattice/core/math.h"
#include "lithokraft/content/signal.h"

namespace lithokraft {

class SoundDirector {
 public:
  explicit SoundDirector(AudioEngine& audio);

  /// Plays `events`; volume falls off with distance from `listener` (silent beyond kHearing).
  void play(std::span<const SoundEvent> events, const Vec3& listener);
  /// Releases every Tone voice (dimension switch).
  void stopAll();

  static constexpr float kHearing = 48.f;  ///< blocks

 private:
  void playTone(const SoundEvent& e, float gain);

  AudioEngine& audio_;
  VoiceAllocator tones_;
};

}  // namespace lithokraft
