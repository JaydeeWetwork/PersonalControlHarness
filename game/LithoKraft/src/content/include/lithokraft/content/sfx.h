// LithoKraft's procedural sound effects: recipes for the engine's effects-bus synth.
#pragma once
#include <cstddef>
#include <cstdint>
#include "lithokraft/engine.h"
#include "litholattice/audio/audio_engine.h"

namespace lithokraft {

enum class Sfx : uint8_t { Place, Break, Click, PulsorFire };

/// Effects-bus voice and note of a sound effect (`strength` tints the Pulsor sweep).
struct SfxNote {
  size_t voice;
  NoteSpec note;
};
SfxNote sfxRecipe(Sfx sfx, int strength);

/// The audio command that plays `sfx` at `gain` (a note on the effects bus).
inline AudioCommand sfxCommand(Sfx sfx, float gain, int strength = 0) {
  SfxNote s = sfxRecipe(sfx, strength);
  s.note.gain *= gain;
  return AudioCommand::noteOn(Bus::Effects, s.voice, s.note);
}

}  // namespace lithokraft
