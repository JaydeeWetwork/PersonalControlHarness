// Async sample streaming: long samples / music decode on a worker thread, never
// on the audio thread.
//
// WHAT: a DecodeFn (the codec: wav/vorbis/whatever the game plugs in) runs on
// a dedicated worker thread, filling fixed-size PCM chunks. Finished chunks
// cross to the audio thread through a HandoffQueue (MPSC: the worker pushes,
// the audio thread swaps the whole vector out in O(1) - see
// core/threading.h). The audio thread then plays the chunks sequentially from
// pre-reserved buffers: no allocation, no blocking, no decode on the callback.
// WHY: decoding a long sample on the audio callback stalls it and the player
// hears a dropout. Decoupling decode (worker) from playback (audio thread)
// with a bounded readahead queue means the callback never waits for the codec.
//
// BACKPRESSURE: the worker decodes at most maxQueuedChunks ahead, then sleeps;
// the audio thread's consumption re-opens the window. Memory stays bounded no
// matter how far ahead the worker could run.
//
// UNDERRUN: if the audio thread outruns the decoder, render() zero-fills the
// shortfall and reports how many valid frames it wrote - silence instead of a
// block. finished() tells the game the stream played out completely.
#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>
#include "litholattice/core/threading.h"

namespace litholattice {

class AudioStreamer {
 public:
  /// Fills up to maxFrames interleaved stereo float frames into `out`;
  /// returns frames written. Returning < maxFrames (0 at the very end)
  /// signals end-of-stream. Runs ONLY on the worker thread.
  using DecodeFn = std::function<size_t(float* out, size_t maxFrames)>;

  explicit AudioStreamer(size_t chunkFrames = 4096, size_t maxQueuedChunks = 8);
  ~AudioStreamer();

  AudioStreamer(const AudioStreamer&) = delete;
  AudioStreamer& operator=(const AudioStreamer&) = delete;

  /// Game thread: install the decoder and start the worker. Replaces any
  /// previous source (stops it first). The read cursor is audio-thread-owned:
  /// play() never touches pending_/chunkIdx_/frameOff_ itself; it requests a
  /// reset that the audio thread applies at the top of the next render().
  void play(DecodeFn decode);
  /// Game thread: stop the worker and drop buffered audio. As with play(),
  /// the audio-side read cursor is reset by the audio thread itself.
  void stop();

  /// Audio thread (real-time safe once play() has returned): writes up to
  /// `frames` interleaved stereo frames into `out`, zero-filling past EOS or
  /// on underrun. Returns the number of frames containing stream audio.
  /// Never allocates, never blocks, never calls the decoder.
  size_t render(float* out, size_t frames);

  /// True once EOS was published by the worker AND fully consumed here.
  bool finished() const { return finished_.load(std::memory_order_relaxed); }
  bool playing() const { return running_.load(std::memory_order_relaxed); }
  /// Decoded-but-unplayed frames (worker estimate + local queue).
  size_t bufferedFrames() const { return buffered_.load(std::memory_order_relaxed); }
  /// Worker thread id, for the self-test's "decode never runs on the audio
  /// thread" check.
  std::thread::id workerId() const { return workerId_; }

 private:
  struct Chunk {
    std::vector<float> pcm;  // interleaved stereo
    size_t frames = 0;
    bool last = false;
  };
  void workerMain();

  const size_t chunkFrames_;
  const size_t maxQueued_;
  HandoffQueue<Chunk> queue_;     // worker -> audio thread (decoded chunks)
  std::vector<Chunk> pending_;    // audio-thread side, pre-reserved
  size_t chunkIdx_ = 0;           // audio thread only
  size_t frameOff_ = 0;           // audio thread only
  DecodeFn decode_;               // set before the worker starts
  std::thread worker_;
  std::thread::id workerId_;
  std::atomic<bool> running_{false};
  std::atomic<bool> finished_{false};
  std::atomic<size_t> queued_{0};    // chunks published but not yet consumed
  std::atomic<size_t> buffered_{0};  // decoded frames not yet consumed
  // Set by play()/stop() on the game thread; the audio thread applies the
  // reset at the top of the next render(). pending_, chunkIdx_ and frameOff_
  // are ONLY ever mutated on the audio thread, so the old vector can never
  // be cleared while render()'s drain loop iterates it.
  std::atomic<bool> pendingReset_{false};
};

}  // namespace litholattice
