// Equal-tempered note helpers (MIDI note numbers, A4 = 69 = 440 Hz).
#pragma once
#include <cmath>
#include <string>

namespace litholattice {

/// Equal-tempered frequency of MIDI note `note` (A4 = 69 = 440 Hz).
inline double noteFrequency(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

/// Note name such as "C4" or "D#7" for a MIDI note number ("-" for a negative note).
inline std::string noteName(int note) {
  static constexpr const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  if (note < 0) return "-";
  return std::string(kNames[note % 12]) + std::to_string(note / 12 - 1);
}

}  // namespace litholattice
