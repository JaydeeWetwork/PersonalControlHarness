// Audio engine: two synth buses fed by a lock-free command queue.
//
//   game thread  --post(AudioCommand)-->  SpscQueue  --render()-->  audio callback
//
// render() drains the queue, applies the commands and mixes; it never locks or allocates.
// The music bus is a SID3 synth (Tone Blocks), the effects bus a DMG synth (procedural SFX).
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include "litholattice/audio/synth.h"
#include "litholattice/audio/effects.h"
#include "litholattice/audio/degradation.h"
#include "litholattice/audio/simd_mix.h"
#include "litholattice/core/spsc_queue.h"

namespace litholattice {

enum class Bus : uint8_t { Music, Effects };

/// One message from the game thread to the audio callback.
struct AudioCommand {
  enum class Op : uint8_t { NoteOn, NoteOff, SetMasterGain, Virtualize };
  Op op = Op::NoteOff;
  Bus bus = Bus::Music;
  uint8_t voice = 0;
  float value = 0.f;  ///< SetMasterGain: gain
  NoteSpec note{};

  static AudioCommand noteOn(Bus bus, size_t voice, const NoteSpec& n) { return {Op::NoteOn, bus, uint8_t(voice), 0.f, n}; }
  static AudioCommand noteOff(Bus bus, size_t voice) { return {Op::NoteOff, bus, uint8_t(voice), 0.f, {}}; }
  static AudioCommand masterGain(float g) { return {Op::SetMasterGain, Bus::Music, 0, g, {}}; }
  /// Virtualize (v=true) or un-virtualize a voice: inaudible voices keep
  /// their timeline but skip mixing. Game thread posts, audio thread applies.
  static AudioCommand virtualize(Bus bus, size_t voice, bool v) {
    return {Op::Virtualize, bus, uint8_t(voice), v ? 1.f : 0.f, {}};
  }
};

class AudioEngine {
 public:
  static constexpr float kDefaultSampleRate = 48000.f;

  explicit AudioEngine(float sampleRate = kDefaultSampleRate);

  /// Game thread (single producer). Returns false if output is disabled, if the
  /// queue is full, or (for non-critical commands) if only the critical reserve
  /// is left. Overflow policy: the last kCriticalReserve slots are reserved for
  /// NoteOff/SetMasterGain — a dropped NoteOff is a stuck note — while other
  /// commands are refused early under bursts. FIFO order is always preserved.
  /// droppedCommands() counts refused posts (telemetry for stuck-note hunts).
  bool post(const AudioCommand& cmd);
  size_t droppedCommands() const { return droppedCommands_.load(std::memory_order_relaxed); }
  /// Commands are dropped while disabled (e.g. --mute or no audio device).
  void setEnabled(bool on) { enabled_.store(on, std::memory_order_relaxed); }
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

  /// Audio thread (single consumer): applies queued commands, then writes `frames` interleaved
  /// frames of `channels` channels (mono mix duplicated) to `out`. Real-time safe.
  void render(float* out, size_t frames, int channels);

  float sampleRate() const { return sampleRate_; }
  /// Direct access for offline rendering in tests (not thread-safe against render()).
  const Synth& music() const { return music_; }
  const Synth& effects() const { return effects_; }
  Synth& music() { return music_; }
  Synth& effects() { return effects_; }

  /// Effect insert on the effects bus (see effects.h). Bypassed by default,
  /// which reproduces the old dry behavior exactly. The game thread clears
  /// the bypass when at least one active voice is audible and near enough
  /// (see effectBypassed() / kEffectBypassDistance).
  void setEffectBypassed(bool b) { effectBypassed_.store(b, std::memory_order_relaxed); }
  bool effectBypassed() const { return effectBypassed_.load(std::memory_order_relaxed); }
  /// Frames the effect chain actually filtered (stays 0 while bypassed).
  uint64_t effectFramesProcessed() const { return effect_.framesProcessed(); }

  /// Agent-audio tap (MCP `audio.capture_chunk`): when armed, render()
  /// copies the final mono mix (post-shed, pre-master-gain) into a lock-free
  /// ring the game drains. Real-time safe: no locks, no allocation on the
  /// audio thread. The tap hears exactly what the player hears - the mix is
  /// already positional (the game computes per-voice distance gain from the
  /// listener before posting), so this is the pilot's ears, not a global
  /// mix. Pre-master-gain by design: master is the user's volume knob; the
  /// agent gets the content, not the knob.
  static constexpr size_t kTapCapacityFrames = 96000;  // 2 s @ 48 kHz
  void setTapEnabled(bool on) { tapEnabled_.store(on, std::memory_order_relaxed); }
  bool tapEnabled() const { return tapEnabled_.load(std::memory_order_relaxed); }
  /// Game/MCP thread (single reader): drains up to maxFrames mono samples
  /// into `out`. Returns frames actually drained. Overflow on the writer
  /// drops the oldest (a tap, not a recorder).
  size_t drainTap(float* out, size_t maxFrames);

  /// Graceful degradation under CPU pressure (see degradation.h). The
  /// level stays 0 with no pressure, reproducing current behavior.
  void setDegradationPolicy(const DegradationPolicy& p) { shedder_.setPolicy(p); }
  int shedLevel() const { return shedder_.level(); }
  double lastAudioLoad() const { return shedder_.lastLoad(); }
  /// Advance the shedder with an explicit measurement. render() calls this
  /// with the real callback time vs budget; tests drive it directly.
  /// Returns the new shed level (0..3).
  int updateShed(std::chrono::nanoseconds elapsed, std::chrono::nanoseconds budget) {
    return shedder_.update(elapsed, budget);
  }

 private:
  void apply(const AudioCommand& cmd);
  /// Applies the current shed level to the synths (audio thread): virtualize
  /// quiet release tails (level 1), cap the voice count (level 2), halve the
  /// render resolution (level 3). Voices the ENGINE virtualizes are tracked in
  /// shedVirtualized_ and auto-cleared on step-down; game-virtualized flags
  /// are never touched (apply() drops them from the mask).
  void applyShedActions();
  Synth& bus(Bus b) { return b == Bus::Music ? music_ : effects_; }

  static constexpr size_t kBlock = 512;
  float sampleRate_;
  Synth music_, effects_;
  static constexpr size_t kCommandQueueDepth = 1024;  // power of two (SpscQueue)
  static constexpr size_t kCriticalReserve = 64;      // slots reserved for NoteOff/SetMasterGain
  SpscQueue<AudioCommand, kCommandQueueDepth> queue_;
  // Heuristic queue depth (every successful push is popped exactly once, so
  // this self-corrects; tryPush() stays the hard gate). Lets post() refuse
  // non-critical commands before they eat the critical reserve.
  std::atomic<size_t> pendingCommands_{0};
  std::atomic<size_t> droppedCommands_{0};
  std::array<float, kBlock> mix_{};
  std::array<float, kBlock> mixFx_{};  // effects-bus insert buffer (effect bypass)
  EffectChain effect_;
  float master_ = 0.6f;
  std::atomic<bool> enabled_{true};
  std::atomic<bool> effectBypassed_{true};
  LoadShedder shedder_;
  // Per-synth bitmask of engine-virtualized voices (audio thread only).
  // applyShedActions() sets bits when it virtualizes; apply() clears them on
  // any game-driven flag change; step-down clears exactly the set bits.
  uint64_t shedVirtualized_[2] = {0, 0};
  // Tap ring: audio thread writes tapHead_, drainTap() reads tapTail_.
  // Single writer / single reader; overflow drops the oldest.
  std::array<float, kTapCapacityFrames> tap_{};
  std::atomic<size_t> tapHead_{0};
  std::atomic<size_t> tapTail_{0};
  std::atomic<bool> tapEnabled_{false};
};

}  // namespace litholattice
