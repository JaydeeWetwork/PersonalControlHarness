// LithoKraft pitch convention shared by the sim (Tone Block readout) and the game's sounds.
// Signal strength 1..64 maps to 64 semitones starting at C2 (MIDI 36): s=25 is middle C,
// s=34 is A4 = 440 Hz, +12 is one octave. Strength 0 means silent.
#pragma once
#include "lithokraft/engine.h"
#include "litholattice/audio/notes.h"

namespace lithokraft {

/// MIDI note number for signal strength `s` (1..64), or -1 for silence (s <= 0).
constexpr int noteForStrength(int s) { return s <= 0 ? -1 : s + 35; }

}  // namespace lithokraft
