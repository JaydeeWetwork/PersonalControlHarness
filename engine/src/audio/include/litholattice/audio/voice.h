// One oscillator voice of the retro synth: waveform generator + ADSR envelope + gain.
// Pure DSP, no allocation and no locks, so it runs inside the audio callback.
#pragma once
#include <array>
#include <cstdint>

namespace litholattice {

/// Oscillator shapes. `Wave` plays a 32-step 4-bit table (Game Boy style wave channel).
enum class Waveform : uint8_t { Pulse, Triangle, Saw, Noise, Wave };

/// Bit for `w` in a waveform mask (see VoiceSlot).
constexpr uint8_t waveformBit(Waveform w) { return uint8_t(1u << unsigned(w)); }

/// Pulse duty cycles, as on the NES / Game Boy pulse channels.
enum class Duty : uint8_t { Eighth, Quarter, Half, ThreeQuarters };  // 12.5 / 25 / 50 / 75 %

/// Fraction of the period the pulse is high.
constexpr float dutyFraction(Duty d) {
  constexpr float k[] = {0.125f, 0.25f, 0.5f, 0.75f};
  // Bounds-checked: a corrupt/foreign Duty value must not OOB-read.
  const unsigned i = unsigned(d);
  return i < 4 ? k[i] : 0.5f;
}

/// 32 samples of 4 bits (0..15) for the Wave waveform.
using WaveTable = std::array<uint8_t, 32>;

/// Envelope times in seconds; sustain is a level 0..1.
struct Adsr {
  float attack = 0.005f, decay = 0.1f, sustain = 0.7f, release = 0.1f;
};

/// Linear-segment ADSR. gateOn starts the attack; gateOff releases from the current level.
class Envelope {
 public:
  enum class Stage : uint8_t { Idle, Attack, Decay, Sustain, Release };

  void configure(const Adsr& adsr, float sampleRate);
  void gateOn();
  void gateOff();
  /// Advances one sample and returns the level (0..1).
  float next();

  Stage stage() const { return stage_; }
  float level() const { return level_; }

 private:
  Adsr adsr_;
  float sampleRate_ = 48000.f;
  float level_ = 0.f, releaseStep_ = 0.f;
  Stage stage_ = Stage::Idle;
};

/// Linear feedback shift register noise (15-bit, or the metallic 7-bit "short" mode).
class Lfsr {
 public:
  /// Clocks the register once and returns the output bit.
  int step();
  void setShortMode(bool s) { short_ = s; }
  void reset() { reg_ = 0x7fff; }

 private:
  uint16_t reg_ = 0x7fff;
  bool short_ = false;
};

/// Everything needed to start a note on a voice. Trivially copyable (sent through the command queue).
struct NoteSpec {
  double frequency = 440.0;         ///< Hz (for noise: register clock / 16)
  Waveform waveform = Waveform::Pulse;
  Duty duty = Duty::Half;
  Adsr adsr{};
  float gain = 0.5f;                ///< 0..1
  float sweep = 0.f;                ///< pitch slide in semitones per second (+ up, - down)
  float hold = 0.f;                 ///< > 0: release automatically after this many seconds
  bool shortNoise = false;          ///< 7-bit LFSR
};

/// A single synth voice.
class Voice {
 public:
  explicit Voice(float sampleRate = 48000.f) : sampleRate_(sampleRate) {}

  /// Starts (or retriggers) a note; the phase is kept so retriggers do not click.
  void noteOn(const NoteSpec& note);
  /// Enters the release stage.
  void noteOff() { env_.gateOff(); }
  /// Sets the table played by Waveform::Wave.
  void setWaveTable(const WaveTable& t) { table_ = t; }

  /// Renders one sample (silence when idle).
  float render();

  /// Advances the timeline one sample without emitting audio (virtualized
  /// voice): the hold countdown, envelope, phase, noise register and sweep
  /// advance exactly as in render(), so un-virtualizing resumes
  /// bit-identically. Cheaper than render() (no sample multiply) but not free.
  void advance();

  bool active() const { return env_.stage() != Envelope::Stage::Idle; }
  Waveform waveform() const { return note_.waveform; }
  double frequency() const { return freq_; }
  /// Note gain 0..1 (for the degradation shedder's quiet-tail test).
  float gain() const { return note_.gain; }
  const Envelope& envelope() const { return env_; }

 private:
  float oscillator();
  /// Phase / noise / sweep step shared by render() and advance().
  void advancePhase();

  float sampleRate_;
  NoteSpec note_{};
  Envelope env_;
  Lfsr lfsr_;
  WaveTable table_{0, 2, 4, 6, 8, 10, 12, 14, 15, 15, 14, 13, 12, 11, 10, 9,
                   8, 7, 6, 5, 4, 3, 2, 1, 0, 0, 1, 2, 3, 4, 5, 6};
  uint32_t phase_ = 0;            // 0.32 fixed point, wraps exactly like a hardware counter
  double freq_ = 0.0, sweepMul_ = 1.0;
  double noisePhase_ = 0.0;
  int noiseBit_ = 1;
  int64_t holdLeft_ = 0;
};

}  // namespace litholattice
