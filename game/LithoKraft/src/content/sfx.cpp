// Procedural sound effects on the DMG effects bus: short envelopes plus pitch sweeps,
// the way NES / Game Boy games built their SFX from the sweep and length units.
#include "lithokraft/content/sfx.h"
#include "lithokraft/content/music.h"

namespace lithokraft {

SfxNote sfxRecipe(Sfx sfx, int strength) {
  NoteSpec n;
  switch (sfx) {
    case Sfx::Place:  // short falling blip
      n = {660.0, Waveform::Pulse, Duty::Quarter, {0.001f, 0.05f, 0.f, 0.02f}, 0.45f, -30.f, 0.05f};
      return {1, n};
    case Sfx::Break:  // noise burst
      n = {1500.0, Waveform::Noise, Duty::Half, {0.001f, 0.12f, 0.f, 0.04f}, 0.5f, -12.f, 0.12f};
      return {3, n};
    case Sfx::Click:  // thin, very short tick
      n = {1800.0, Waveform::Pulse, Duty::Eighth, {0.0005f, 0.015f, 0.f, 0.01f}, 0.35f, 0.f, 0.015f};
      return {0, n};
    case Sfx::PulsorFire: {  // rising "pew" on the wave channel, pitched by the pulse strength
      const int note = noteForStrength(strength > 0 ? strength : 25) + 12;
      n = {noteFrequency(note), Waveform::Wave, Duty::Half, {0.001f, 0.12f, 0.f, 0.03f}, 0.4f, 36.f, 0.1f};
      return {2, n};
    }
  }
  return {0, n};
}

}  // namespace lithokraft
