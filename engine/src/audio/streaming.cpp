// Async sample streaming: the worker thread decodes chunks, the audio thread
// plays them. See streaming.h for the design.
#include "litholattice/audio/streaming.h"

#include <algorithm>
#include <cstring>

namespace litholattice {

AudioStreamer::AudioStreamer(size_t chunkFrames, size_t maxQueuedChunks)
    : chunkFrames_(chunkFrames ? chunkFrames : 1),
      maxQueued_(maxQueuedChunks ? maxQueuedChunks : 1) {
  pending_.reserve(maxQueued_ + 1);
}

AudioStreamer::~AudioStreamer() { stop(); }

void AudioStreamer::play(DecodeFn decode) {
  stop();  // joins any running worker and requests the audio-side reset
  decode_ = std::move(decode);  // no worker running: game-thread owned again
  running_.store(true, std::memory_order_relaxed);
  worker_ = std::thread(&AudioStreamer::workerMain, this);
}

void AudioStreamer::stop() {
  running_.store(false, std::memory_order_relaxed);
  if (worker_.joinable()) worker_.join();
  decode_ = nullptr;
  // The worker is joined: no producer can touch queue_ now, so the game
  // thread drains the stale chunks itself (drainInto is mutex-guarded, so
  // this also serializes safely against the audio thread's concurrent
  // drain). The counters are repaired for exactly the chunks removed here.
  std::vector<Chunk> stale;
  queue_.drainInto(stale);
  for (const auto& c : stale) {
    queued_.fetch_sub(1, std::memory_order_relaxed);
    buffered_.fetch_sub(c.frames, std::memory_order_relaxed);
  }
  // The read cursor is audio-thread-owned: request the reset; the audio
  // thread applies it at the top of the next render(), never here.
  pendingReset_.store(true, std::memory_order_release);
}

void AudioStreamer::workerMain() {
  workerId_ = std::this_thread::get_id();
  // Decode target: one persistent buffer per worker thread, grown once. The
  // old code resized a fresh vector per chunk (malloc + a full zero-fill of
  // chunkFrames_*2 floats per chunk, then the decoder overwrote it); the
  // chunk now takes a copy of just the decoded frames, and the scratch
  // buffer itself is reused across chunks. (Fully eliminating the per-chunk
  // allocation would need a buffer return channel from the audio thread;
  // the audio side already recycles pending_ storage across drains.)
  thread_local std::vector<float> scratch;
  if (scratch.size() < chunkFrames_ * 2) scratch.resize(chunkFrames_ * 2);
  for (;;) {
    if (!running_.load(std::memory_order_relaxed)) break;
    if (queued_.load(std::memory_order_relaxed) >= maxQueued_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    const size_t got = decode_ ? decode_(scratch.data(), chunkFrames_) : 0;
    Chunk c;
    c.frames = std::min(got, chunkFrames_);
    c.pcm.assign(scratch.data(), scratch.data() + c.frames * 2);
    c.last = got < chunkFrames_;  // short chunk (incl. 0) = end of stream
    queued_.fetch_add(1, std::memory_order_relaxed);
    buffered_.fetch_add(c.frames, std::memory_order_relaxed);
    queue_.push(std::move(c));
    if (c.last) break;
  }
  running_.store(false, std::memory_order_relaxed);
}

size_t AudioStreamer::render(float* out, size_t frames) {
  // Apply a pending play()/stop() reset on the audio thread only. The read
  // cursor (pending_/chunkIdx_/frameOff_) is never mutated from the game
  // thread, so the old vector cannot be cleared while the loop below
  // iterates it: the game-thread/audio-thread race is gone.
  if (pendingReset_.exchange(false, std::memory_order_acquire)) {
    // Repair the counters for the dropped (never-consumed) pending chunks.
    size_t dropped = 0, droppedFrames = 0;
    if (chunkIdx_ < pending_.size()) {
      dropped = pending_.size() - chunkIdx_;
      droppedFrames = pending_[chunkIdx_].frames - frameOff_;
      for (size_t i = chunkIdx_ + 1; i < pending_.size(); ++i)
        droppedFrames += pending_[i].frames;
    }
    queued_.fetch_sub(dropped, std::memory_order_relaxed);
    buffered_.fetch_sub(droppedFrames, std::memory_order_relaxed);
    pending_.clear();  // capacity kept, no audio-thread allocation
    chunkIdx_ = 0;
    frameOff_ = 0;
    finished_.store(false, std::memory_order_relaxed);
  }
  queue_.drainInto(pending_);  // O(1) swap under a short lock; decode never runs here
  size_t written = 0;
  while (written < frames && chunkIdx_ < pending_.size()) {
    Chunk& c = pending_[chunkIdx_];
    const size_t avail = c.frames - frameOff_;
    const size_t take = std::min(avail, frames - written);
    if (take > 0) {
      std::memcpy(out + written * 2, c.pcm.data() + frameOff_ * 2,
                  take * 2 * sizeof(float));
      written += take;
      frameOff_ += take;
      buffered_.fetch_sub(take, std::memory_order_relaxed);
    }
    if (frameOff_ >= c.frames) {
      ++chunkIdx_;
      frameOff_ = 0;
      queued_.fetch_sub(1, std::memory_order_relaxed);
      if (c.last) finished_.store(true, std::memory_order_relaxed);
    }
  }
  // Underrun or past EOS: silence, never a block. (All-bits-zero is 0.0f.)
  std::memset(out + written * 2, 0, (frames - written) * 2 * sizeof(float));
  if (chunkIdx_ == pending_.size() && chunkIdx_ > 0) {
    pending_.clear();  // recycle: capacity kept, no audio-thread allocation
    chunkIdx_ = 0;
  }
  return written;
}

}  // namespace litholattice
