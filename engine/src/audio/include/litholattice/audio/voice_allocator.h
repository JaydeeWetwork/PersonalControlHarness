// Maps long-lived sound sources (e.g. Tone Blocks by position) onto a fixed number of voices,
// stealing the least recently started voice when all are busy. Game thread only.
#pragma once
#include <cstdint>
#include <optional>
#include <vector>

namespace litholattice {

class VoiceAllocator {
 public:
  explicit VoiceAllocator(size_t voices) : slots_(voices) {}

  /// Voice for `key`: its current one, else a free one, else the oldest (stolen).
  size_t acquire(uint64_t key);
  /// Frees the voice held by `key`; returns it, or nullopt if the key had none (e.g. stolen).
  std::optional<size_t> release(uint64_t key);
  /// Forgets every key.
  void clear();

  /// Marks a voice virtualized (inaudible: its timeline is kept but it
  /// contributes nothing to the mix). Game thread only, like the rest of
  /// this class; mirror the flags the audio thread holds on the Synth.
  void setVirtualized(size_t voice, bool v);
  bool virtualized(size_t voice) const;

 private:
  struct Slot {
    std::optional<uint64_t> key;
    uint64_t started = 0;
    bool virtualized = false;
  };
  std::vector<Slot> slots_;
  uint64_t clock_ = 0;
};

}  // namespace litholattice
