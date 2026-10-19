// Audio output device interface. Backends (miniaudio today) pull samples from an AudioEngine on
// their own real-time thread for as long as the AudioOutput object lives (RAII).
#pragma once
#include <memory>
#include <string>

namespace litholattice {

class AudioEngine;

class AudioOutput {
 public:
  virtual ~AudioOutput() = default;
  /// Human-readable device / backend name for the log.
  virtual std::string description() const = 0;
};

/// Opens the default playback device (stereo, float, engine sample rate) and starts pulling from
/// `engine`, which must outlive the returned object. Returns nullptr and sets `err` on failure.
std::unique_ptr<AudioOutput> openAudioOutput(AudioEngine& engine, std::string* err);

}  // namespace litholattice
