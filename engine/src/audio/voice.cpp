#include "litholattice/audio/voice.h"
#include <algorithm>
#include <cmath>

namespace litholattice {

void Envelope::configure(const Adsr& adsr, float sampleRate) {
  adsr_ = adsr;
  sampleRate_ = sampleRate;
}

void Envelope::gateOn() { stage_ = Stage::Attack; }

void Envelope::gateOff() {
  if (stage_ == Stage::Idle) return;
  stage_ = Stage::Release;
  releaseStep_ = level_ / std::max(1.f, adsr_.release * sampleRate_);
}

float Envelope::next() {
  auto perSample = [&](float seconds) { return 1.f / std::max(1.f, seconds * sampleRate_); };
  switch (stage_) {
    case Stage::Idle: level_ = 0.f; break;
    case Stage::Attack:
      level_ += perSample(adsr_.attack);
      if (level_ >= 1.f) { level_ = 1.f; stage_ = Stage::Decay; }
      break;
    case Stage::Decay:
      level_ -= (1.f - adsr_.sustain) * perSample(adsr_.decay);
      if (level_ <= adsr_.sustain) {
        level_ = adsr_.sustain;
        stage_ = adsr_.sustain > 0.f ? Stage::Sustain : Stage::Idle;
      }
      break;
    case Stage::Sustain: level_ = adsr_.sustain; break;
    case Stage::Release:
      level_ -= releaseStep_;
      if (level_ <= 0.f) { level_ = 0.f; stage_ = Stage::Idle; }
      break;
  }
  return level_;
}

int Lfsr::step() {
  // Game Boy CH4: XOR bits 0 and 1, shift right, feed back into bit 14 (and bit 6 in short mode).
  const uint16_t fb = (reg_ ^ (reg_ >> 1)) & 1u;
  reg_ = uint16_t((reg_ >> 1) | (fb << 14));
  if (short_) reg_ = uint16_t((reg_ & ~(1u << 6)) | (fb << 6));
  return (~reg_) & 1;
}

namespace {
// Upper bound for NoteSpec::hold: an automatic release after this many seconds.
// Anything larger is a stuck note (and int64_t(huge * sampleRate) is UB).
constexpr float kMaxHoldSeconds = 3600.f;  // one hour: beyond any musical use
}  // namespace

void Voice::noteOn(const NoteSpec& note) {
  note_ = note;
  // Untrusted-NoteSpec hardening. A NaN/negative/huge frequency is UB in the
  // float->uint32 phase increment on the first advancePhase(), and a huge
  // frequency makes the noise-clock loop hang the audio thread; a NaN sweep
  // poisons freq_ the same way a NaN frequency does.
  double freq = note.frequency;
  if (!std::isfinite(freq)) freq = 440.0;
  freq_ = std::clamp(freq, 1.0, double(sampleRate_) * 0.45);
  double sweep = double(note.sweep);
  if (!std::isfinite(sweep)) sweep = 0.0;
  sweepMul_ = std::pow(2.0, std::clamp(sweep, -96.0, 96.0) / 12.0 / sampleRate_);
  float hold = note.hold;
  if (!std::isfinite(hold) || hold < 0.f) hold = 0.f;
  hold = std::min(hold, kMaxHoldSeconds);
  holdLeft_ = hold > 0.f ? int64_t(double(hold) * double(sampleRate_)) : -1;
  lfsr_.setShortMode(note.shortNoise);
  env_.configure(note.adsr, sampleRate_);
  env_.gateOn();
}

float Voice::oscillator() {
  const float p = float(phase_) * 0x1p-32f;
  switch (note_.waveform) {
    case Waveform::Pulse: return p < dutyFraction(note_.duty) ? 1.f : -1.f;
    case Waveform::Triangle: return 4.f * std::abs(p - 0.5f) - 1.f;
    case Waveform::Saw: return 2.f * p - 1.f;
    case Waveform::Wave: return float(table_[size_t(p * 32.f) & 31]) / 7.5f - 1.f;
    case Waveform::Noise: return noiseBit_ ? 1.f : -1.f;
  }
  return 0.f;
}

void Voice::advancePhase() {
  phase_ += uint32_t(freq_ / sampleRate_ * 0x1p32);
  if (note_.waveform == Waveform::Noise) {  // the register is clocked 16x per "period"
    noisePhase_ += 16.0 * freq_ / sampleRate_;
    for (; noisePhase_ >= 1.0; noisePhase_ -= 1.0) noiseBit_ = lfsr_.step();
  }
  freq_ = std::clamp(freq_ * sweepMul_, 10.0, sampleRate_ * 0.45);
}

void Voice::advance() {
  // Same state transitions as render(), minus the sample: hold countdown,
  // envelope step, then the phase/noise/sweep step. Bit-identical resume.
  if (!active()) return;
  if (holdLeft_ > 0 && --holdLeft_ == 0) env_.gateOff();
  env_.next();
  advancePhase();
}

float Voice::render() {
  if (!active()) return 0.f;
  if (holdLeft_ > 0 && --holdLeft_ == 0) env_.gateOff();
  const float s = oscillator() * env_.next() * note_.gain;
  advancePhase();
  return s;
}

}  // namespace litholattice
