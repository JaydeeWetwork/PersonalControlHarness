#pragma once

namespace litholattice {
/// Offline checks of the synth (no audio device): pitch, duty, envelope, noise, presets, queue,
/// SFX. Prints PASS/FAIL lines and returns the number of failures.
int runAudioSelfTest();
}  // namespace litholattice
