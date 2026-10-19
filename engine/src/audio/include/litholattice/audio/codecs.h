// Codec stubs: format sniffing + minimal decoders feeding the async streaming
// pipeline (see streaming.h: AudioStreamer::DecodeFn).
//
// WHAT: WAV PCM decodes fully by hand (it is just a header + raw samples).
// FLAC / Opus / Vorbis parse their containers and headers today so the engine
// can report format info (sample rate, channels, duration) and reject garbage
// early; actual frame/packet decoding is stubbed for the builder, with the
// vendoring choice documented per codec in the report.
// WHY: the game needs music/long samples in compressed formats, but pulling
// in libopus/libvorbis/dr_flac is a build-system + license decision that
// belongs to the builder, not to a stub. These stubs draw the seam: the
// IAudioDecoder interface is stable, the builder only fills in decode().
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "litholattice/audio/streaming.h"

namespace litholattice {

/// Audio container/codec kinds the engine can sniff.
enum class AudioCodec : uint8_t { kUnknown = 0, kWav, kFlac, kOpus, kVorbis };

/// Identify the codec from magic bytes. Needs at most the first few hundred
/// bytes; returns kUnknown when nothing matches. Never reads past `size`.
/// (Ogg-based formats are told apart by scanning the first page for
/// "OpusHead" vs the vorbis identification header.)
AudioCodec sniffAudioCodec(const uint8_t* data, size_t size);

/// What open() learned about the stream.
struct AudioFormatInfo {
  uint32_t sampleRate = 0;    ///< Hz; 0 = unknown
  uint16_t channels = 0;      ///< 0 = unknown
  uint16_t bitsPerSample = 0;  ///< 0 for lossy/compressed codecs
  uint64_t totalFrames = 0;   ///< 0 = unknown
  bool valid = false;
};

/// One-shot decoder: open() sniffs + parses headers, decode() streams
/// interleaved STEREO float frames (the AudioStreamer::DecodeFn contract).
/// Decoders borrow the source buffer (no copy) - the caller must keep `data`
/// alive until the decoder is destroyed. Decoders are NOT thread-safe; run
/// one instance per worker thread. decode() fills up to maxFrames and returns
/// frames written; a short write (0 at the very end) is end-of-stream.
/// On error decode() returns 0 and sets err.
class IAudioDecoder {
 public:
  virtual ~IAudioDecoder() = default;
  virtual AudioCodec codec() const = 0;
  /// False for the container-only stubs (FLAC/Opus/Vorbis today): format()
  /// is valid after open(), but decode() always reports "not implemented".
  virtual bool implemented() const = 0;
  virtual bool open(const uint8_t* data, size_t size, std::string& err) = 0;
  virtual AudioFormatInfo format() const = 0;
  virtual size_t decode(float* outStereo, size_t maxFrames, std::string& err) = 0;
};

/// Sniff + construct the matching decoder. Returns nullptr (err set) when the
/// format is unknown or the headers don't parse.
std::unique_ptr<IAudioDecoder> makeAudioDecoder(const uint8_t* data, size_t size,
                                                std::string& err);

/// Adapts an IAudioDecoder to the AudioStreamer::DecodeFn contract. DecodeFn
/// has no error channel, so this checks implemented() UP FRONT: wiring a stub
/// decoder (FLAC/Opus/Vorbis today) into a streamer can never fail as silent
/// end-of-stream — the stub reason is logged once here, at wire-up time, and
/// the returned functor reports EOS. A null decoder is likewise logged and
/// reports EOS. Decode-time errors from a real decoder are logged (once per
/// stream: the first short read ends the stream).
/// The decoder must outlive the returned functor; the functor itself runs on
/// the streamer's worker thread (never the audio thread).
AudioStreamer::DecodeFn adaptDecoderToDecodeFn(IAudioDecoder* decoder);

}  // namespace litholattice
