#include "litholattice/audio/audio_engine.h"
#include <algorithm>
#include <span>

namespace litholattice {

AudioEngine::AudioEngine(float sampleRate)
    : sampleRate_(sampleRate),
      music_(SynthPreset::sid3(), sampleRate),
      effects_(SynthPreset::dmg(), sampleRate),
      effect_(sampleRate),
      tap_(std::make_unique<float[]>(kTapCapacityFrames)) {}  // zero-initialised

bool AudioEngine::post(const AudioCommand& cmd) {
  if (!enabled()) return false;
  const bool critical = cmd.op == AudioCommand::Op::NoteOff ||
                        cmd.op == AudioCommand::Op::SetMasterGain;
  if (!critical && pendingCommands_.load(std::memory_order_relaxed) >=
                       kCommandQueueDepth - kCriticalReserve) {
    droppedCommands_.fetch_add(1, std::memory_order_relaxed);
    return false;  // burst: keep the reserve for NoteOff/SetMasterGain
  }
  if (!queue_.tryPush(cmd)) {
    droppedCommands_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  pendingCommands_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void AudioEngine::apply(const AudioCommand& cmd) {
  // The shedder's engine-virtualized bitmask tracks only voices the ENGINE
  // virtualized. Any game-driven change of a voice's flag (Virtualize command
  // or a fresh NoteOn, which starts audible) takes ownership away from the
  // shedder, so a later step-down never clears a game-virtualized voice.
  const auto shedBit = [](uint8_t voice) {
    return voice < 64 ? (uint64_t(1) << voice) : uint64_t(0);
  };
  const int bi = cmd.bus == Bus::Music ? 0 : 1;
  switch (cmd.op) {
    case AudioCommand::Op::NoteOn:
      shedVirtualized_[bi] &= ~shedBit(cmd.voice);
      bus(cmd.bus).noteOn(cmd.voice, cmd.note);
      break;
    case AudioCommand::Op::NoteOff: bus(cmd.bus).noteOff(cmd.voice); break;
    case AudioCommand::Op::SetMasterGain: master_ = std::clamp(cmd.value, 0.f, 1.f); break;
    case AudioCommand::Op::Virtualize:
      shedVirtualized_[bi] &= ~shedBit(cmd.voice);
      bus(cmd.bus).setVirtualized(cmd.voice, cmd.value != 0.f);
      break;
  }
}

void AudioEngine::applyShedActions() {
  // Runs on the audio thread at the start of each render(), from the level
  // the previous render measured. Engine-virtualized voices are tracked in
  // shedVirtualized_ (one bitmask per synth); when the shedder steps DOWN,
  // exactly those voices are un-virtualized. Game-virtualized flags are
  // never in the mask (apply() removes them), so the game keeps full
  // ownership of the voices it virtualized itself.
  const int level = shedder_.level();
  const float quiet = shedder_.policy().quietThreshold;
  Synth* synths[2] = {&music_, &effects_};
  for (int bi = 0; bi < 2; ++bi) {
    Synth* s = synths[bi];
    uint64_t& mask = shedVirtualized_[bi];
    s->setRateDivider(level >= 3 ? 2 : 1);
    const size_t cap = level >= 2 ? s->voiceCount() / 2 : s->voiceCount();
    for (size_t i = 0; i < s->voiceCount() && i < 64; ++i) {
      const uint64_t bit = uint64_t(1) << i;
      if (i >= cap) {
        // Level >= 2 cap: record the voice only if the engine is the one
        // virtualizing it (a game-virtualized voice keeps the game's flag).
        if (!s->virtualized(i)) mask |= bit;
        s->setVirtualized(i, true);
        continue;
      }
      // Step-down: clear exactly the voices the engine virtualized. Falls
      // through to the quiet-tail test so a still-quiet tail is immediately
      // re-virtualized at level >= 1 instead of popping audible for a tick.
      if (mask & bit) {
        s->setVirtualized(i, false);
        mask &= ~bit;
      }
      if (level >= 1 && !s->virtualized(i)) {
        Voice& v = s->voice(i);
        // Only release-stage tails: a note in attack/decay/sustain is never
        // auto-virtualized, so no audible note can get stuck virtualized.
        if (v.active() && v.envelope().stage() == Envelope::Stage::Release &&
            v.envelope().level() * v.gain() < quiet) {
          s->setVirtualized(i, true);
          mask |= bit;
        }
      }
    }
  }
}

void AudioEngine::render(float* out, size_t frames, int channels) {
  const auto t0 = std::chrono::steady_clock::now();
  while (const auto cmd = queue_.tryPop()) {
    pendingCommands_.fetch_sub(1, std::memory_order_relaxed);
    apply(*cmd);
  }
  applyShedActions();
  for (size_t done = 0; done < frames;) {
    const size_t n = std::min(kBlock, frames - done);
    const std::span<float> block(mix_.data(), n);
    const std::span<float> fx(mixFx_.data(), n);
    std::fill(block.begin(), block.end(), 0.f);
    std::fill(fx.begin(), fx.end(), 0.f);
    music_.mixInto(block);
    effects_.mixInto(fx);
    // Effect bypass: skipped for virtualized/distant voices (the game
    // thread sets the flag from effectBypassed()); bypassed by default.
    if (!effectBypassed_.load(std::memory_order_relaxed)) effect_.process(fx.data(), n);
    simd::addInto(block.data(), fx.data(), n);
    // NaN/Inf hardening: float WAV samples (or a hostile NoteSpec) can put
    // non-finite values into the mix; scrub them to 0 here so neither the
    // device nor the MCP tap ever sees them. One pass per block.
    simd::scrubNonFinite(block.data(), n);
    if (tapEnabled_.load(std::memory_order_relaxed)) {
      size_t head = tapHead_.load(std::memory_order_relaxed);
      for (size_t i = 0; i < n; ++i) tap_[head++ % kTapCapacityFrames] = block[i];
      tapHead_.store(head, std::memory_order_release);
      // Overflow drops the oldest: keep at most capacity frames.
      const size_t tail = tapTail_.load(std::memory_order_relaxed);
      if (head - tail > kTapCapacityFrames)
        tapTail_.store(head - kTapCapacityFrames, std::memory_order_relaxed);
    }
    simd::applyGainClampDuplicate(out + done * size_t(channels), block.data(), n, master_, channels);
    done += n;
  }
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  const auto budget =
      std::chrono::nanoseconds(int64_t(double(frames) / double(sampleRate_) * 1e9));
  updateShed(elapsed, budget);
}

size_t AudioEngine::drainTap(float* out, size_t maxFrames) {
  if (!out || maxFrames == 0) return 0;
  const size_t head = tapHead_.load(std::memory_order_acquire);
  size_t tail = tapTail_.load(std::memory_order_relaxed);
  if (head < tail) tail = head;  // defensive: never run backwards
  // Only the last capacity frames are valid; a slow reader drops the rest.
  const size_t oldest = head > kTapCapacityFrames ? head - kTapCapacityFrames : 0;
  if (tail < oldest) tail = oldest;
  const size_t avail = head - tail;
  const size_t n = avail < maxFrames ? avail : maxFrames;
  for (size_t i = 0; i < n; ++i) out[i] = tap_[(tail + i) % kTapCapacityFrames];
  tapTail_.store(tail + n, std::memory_order_release);
  return n;
}

}  // namespace litholattice
