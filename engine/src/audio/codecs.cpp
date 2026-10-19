// Codec stubs: sniffing + WAV PCM decode + FLAC/Opus/Vorbis container parsing.
// See codecs.h for the design. Frame decoding for the compressed codecs is
// the builder's job (library choices documented in the report).
#include "litholattice/audio/codecs.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "litholattice/core/log.h"

namespace litholattice {
namespace {

// ---- byte readers (all bounds-checked by callers) ----
uint16_t rd16le(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
uint32_t rd32le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t rd16be(const uint8_t* p) {
  return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}
uint32_t rd24be(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) |
         static_cast<uint32_t>(p[2]);
}

bool hasMagic(const uint8_t* d, size_t n, size_t at, const char* magic, size_t m) {
  return at + m <= n && std::memcmp(d + at, magic, m) == 0;
}

// memmem over the first `limit` bytes.
bool contains(const uint8_t* d, size_t n, const char* magic, size_t m) {
  const size_t limit = std::min(n, size_t(4096));
  if (limit < m) return false;
  for (size_t i = 0; i + m <= limit; ++i)
    if (std::memcmp(d + i, magic, m) == 0) return true;
  return false;
}

// ---- Ogg page parsing (shared by Opus/Vorbis) ----
// Locates the first packet of the first page. Returns false on any malformed
// structure. Checksum is NOT verified (stub-grade parsing).
bool firstOggPacket(const uint8_t* d, size_t n, size_t& pktOff, size_t& pktLen) {
  if (!hasMagic(d, n, 0, "OggS", 4) || n < 27) return false;
  if (d[4] != 0) return false;  // stream_structure_version
  const uint8_t segs = d[26];
  if (27u + segs > n) return false;
  size_t off = 27 + segs;
  size_t len = 0;
  for (uint8_t i = 0; i < segs; ++i) {
    const uint8_t s = d[27 + i];
    if (off + s > n) return false;
    len += s;
    off += s;
    if (s < 255) break;  // packet ends at the first short segment
  }
  if (len == 0) return false;
  pktOff = 27 + segs;
  pktLen = len;
  return true;
}

// ---- WAV ----
class WavDecoder : public IAudioDecoder {
 public:
  AudioCodec codec() const override { return AudioCodec::kWav; }
  bool implemented() const override { return true; }

  bool open(const uint8_t* data, size_t size, std::string& err) override {
    data_ = data;
    if (size < 12 || !hasMagic(data, size, 0, "RIFF", 4) ||
        !hasMagic(data, size, 8, "WAVE", 4)) {
      err = "wav: not a RIFF/WAVE file";
      return false;
    }
    bool haveFmt = false, haveData = false;
    size_t off = 12;
    while (off + 8 <= size) {
      const uint32_t chunkSize = rd32le(data + off + 4);
      size_t payload = off + 8;
      if (payload + chunkSize > size) break;  // truncated tail: stop, don't read past
      if (hasMagic(data, size, off, "fmt ", 4)) {
        if (chunkSize < 16) {
          err = "wav: fmt chunk too small";
          return false;
        }
        const uint16_t audioFormat = rd16le(data + payload);
        channels_ = rd16le(data + payload + 2);
        sampleRate_ = rd32le(data + payload + 4);
        bitsPerSample_ = rd16le(data + payload + 14);
        if (audioFormat != 1 && audioFormat != 3) {
          err = "wav: unsupported format tag (only PCM=1 and IEEE float=3)";
          return false;
        }
        float_ = (audioFormat == 3);
        const bool bitsOk = float_ ? (bitsPerSample_ == 32 || bitsPerSample_ == 64)
                                   : (bitsPerSample_ == 8 || bitsPerSample_ == 16 ||
                                      bitsPerSample_ == 24 || bitsPerSample_ == 32);
        if (!bitsOk) {
          err = "wav: unsupported bits-per-sample for this format";
          return false;
        }
        if (channels_ == 0 || channels_ > 2) {
          err = "wav: only mono/stereo supported";
          return false;
        }
        if (sampleRate_ == 0) {
          err = "wav: zero sample rate";
          return false;
        }
        bytesPerFrame_ = size_t(channels_) * bitsPerSample_ / 8;
        haveFmt = true;
      } else if (hasMagic(data, size, off, "data", 4)) {
        dataOff_ = payload;
        dataLen_ = chunkSize;
        haveData = true;
      }
      off = payload + chunkSize + (chunkSize & 1);  // chunks pad to even sizes
    }
    if (!haveFmt) {
      err = "wav: no fmt chunk";
      return false;
    }
    if (!haveData) {
      err = "wav: no data chunk";
      return false;
    }
    totalFrames_ = dataLen_ / bytesPerFrame_;
    pos_ = 0;
    return true;
  }

  AudioFormatInfo format() const override {
    AudioFormatInfo f;
    f.sampleRate = sampleRate_;
    f.channels = channels_;
    f.bitsPerSample = bitsPerSample_;
    f.totalFrames = totalFrames_;
    f.valid = true;
    return f;
  }

  size_t decode(float* outStereo, size_t maxFrames, std::string& err) override {
    (void)err;
    size_t frames = 0;
    while (frames < maxFrames && pos_ < totalFrames_) {
      const uint8_t* p = data_ + dataOff_ + pos_ * bytesPerFrame_;
      float ch[2] = {0.f, 0.f};
      for (uint16_t c = 0; c < channels_; ++c)
        ch[c] = sampleToFloat(p + c * bitsPerSample_ / 8);
      outStereo[frames * 2] = ch[0];
      outStereo[frames * 2 + 1] = channels_ > 1 ? ch[1] : ch[0];  // mono -> dual
      ++frames;
      ++pos_;
    }
    return frames;
  }

 private:
  float sampleToFloat(const uint8_t* p) const {
    if (float_) {
      if (bitsPerSample_ == 32) {
        float v;
        std::memcpy(&v, p, 4);
        return v;
      }
      double v;
      std::memcpy(&v, p, 8);
      return static_cast<float>(v);
    }
    switch (bitsPerSample_) {
      case 8:
        return (static_cast<float>(p[0]) - 128.f) / 128.f;  // unsigned PCM
      case 16: {
        int16_t v;
        std::memcpy(&v, p, 2);
        return static_cast<float>(v) / 32768.f;
      }
      case 24: {
        const int32_t v = (static_cast<int32_t>(p[2]) << 24) |
                          (static_cast<int32_t>(p[1]) << 16) |
                          (static_cast<int32_t>(p[0]) << 8);
        return static_cast<float>(v) / 2147483648.f;
      }
      default: {  // 32-bit signed
        int32_t v;
        std::memcpy(&v, p, 4);
        return static_cast<float>(v) / 2147483648.f;
      }
    }
  }

  const uint8_t* data_ = nullptr;
  size_t dataOff_ = 0, dataLen_ = 0, pos_ = 0, totalFrames_ = 0;
  size_t bytesPerFrame_ = 0;
  uint32_t sampleRate_ = 0;
  uint16_t channels_ = 0, bitsPerSample_ = 0;
  bool float_ = false;
};

// ---- FLAC (container parse only; frame decode is the builder's job) ----
class FlacDecoder : public IAudioDecoder {
 public:
  AudioCodec codec() const override { return AudioCodec::kFlac; }
  bool implemented() const override { return false; }

  bool open(const uint8_t* data, size_t size, std::string& err) override {
    if (size < 4 || !hasMagic(data, size, 0, "fLaC", 4)) {
      err = "flac: missing fLaC marker";
      return false;
    }
    size_t off = 4;
    bool haveStreamInfo = false;
    while (off + 4 <= size) {
      const uint8_t hdr = data[off];
      const uint8_t type = hdr & 0x7F;
      const uint32_t len = rd24be(data + off + 1);
      off += 4;
      if (off + len > size) {
        err = "flac: metadata block overruns the buffer";
        return false;
      }
      if (type == 0) {  // STREAMINFO
        if (len < 34) {
          err = "flac: STREAMINFO too short";
          return false;
        }
        const uint8_t* s = data + off;
        // bytes 10..17: 20-bit rate | 3-bit (ch-1) | 5-bit (bps-1) | 36-bit total
        sampleRate_ = (static_cast<uint32_t>(s[10]) << 12) |
                      (static_cast<uint32_t>(s[11]) << 4) |
                      (static_cast<uint32_t>(s[12]) >> 4);
        channels_ = static_cast<uint16_t>(((s[12] >> 1) & 0x7) + 1);
        bitsPerSample_ = static_cast<uint16_t>((((s[12] & 0x1) << 4) | (s[13] >> 4)) + 1);
        totalFrames_ = (static_cast<uint64_t>(s[13] & 0xF) << 32) |
                       (static_cast<uint64_t>(rd16be(s + 14)) << 16) |
                       static_cast<uint64_t>(rd16be(s + 16));
        if (sampleRate_ == 0 || channels_ == 0) {
          err = "flac: STREAMINFO has zero rate/channels";
          return false;
        }
        haveStreamInfo = true;
      }
      off += len;
      if (hdr & 0x80) break;  // last-metadata-block flag
    }
    if (!haveStreamInfo) {
      err = "flac: no STREAMINFO block";
      return false;
    }
    return true;
  }

  AudioFormatInfo format() const override {
    AudioFormatInfo f;
    f.sampleRate = sampleRate_;
    f.channels = channels_;
    f.bitsPerSample = bitsPerSample_;
    f.totalFrames = totalFrames_;
    f.valid = true;
    return f;
  }

  size_t decode(float* /*outStereo*/, size_t /*maxFrames*/, std::string& err) override {
    // TODO(builder): vendor dr_flac (public domain / MIT) and decode frames
    // here. The STREAMINFO above already gives rate/channels/bps/total.
    err = "flac: frame decoding not implemented (stub; see builder notes)";
    return 0;
  }

 private:
  uint32_t sampleRate_ = 0;
  uint16_t channels_ = 0, bitsPerSample_ = 0;
  uint64_t totalFrames_ = 0;
};

// ---- Opus in Ogg (header parse only) ----
class OpusDecoder : public IAudioDecoder {
 public:
  AudioCodec codec() const override { return AudioCodec::kOpus; }
  bool implemented() const override { return false; }

  bool open(const uint8_t* data, size_t size, std::string& err) override {
    size_t pktOff = 0, pktLen = 0;
    if (!firstOggPacket(data, size, pktOff, pktLen)) {
      err = "opus: not an Ogg page";
      return false;
    }
    const uint8_t* p = data + pktOff;
    if (pktLen < 19 || !hasMagic(p, pktLen, 0, "OpusHead", 8)) {
      err = "opus: first packet is not OpusHead";
      return false;
    }
    channels_ = p[9];
    preSkip_ = rd16le(p + 10);
    if (channels_ == 0 || channels_ > 8) {
      err = "opus: bad channel count in OpusHead";
      return false;
    }
    return true;
  }

  AudioFormatInfo format() const override {
    AudioFormatInfo f;
    f.sampleRate = 48000;  // Opus decodes at 48 kHz internally, always
    f.channels = channels_;
    f.bitsPerSample = 0;  // compressed
    f.totalFrames = 0;    // needs the (optional) OpusTags duration; builder
    f.valid = true;
    return f;
  }

  size_t decode(float* /*outStereo*/, size_t /*maxFrames*/, std::string& err) override {
    // TODO(builder): link libopus (BSD) or vendor a minimal decoder; honor
    // preSkip_ (drop that many leading samples) and resample 48 kHz -> the
    // engine rate if they differ.
    (void)preSkip_;
    err = "opus: packet decoding not implemented (stub; see builder notes)";
    return 0;
  }

 private:
  uint16_t channels_ = 0, preSkip_ = 0;
};

// ---- Vorbis in Ogg (header parse only) ----
class VorbisDecoder : public IAudioDecoder {
 public:
  AudioCodec codec() const override { return AudioCodec::kVorbis; }
  bool implemented() const override { return false; }

  bool open(const uint8_t* data, size_t size, std::string& err) override {
    size_t pktOff = 0, pktLen = 0;
    if (!firstOggPacket(data, size, pktOff, pktLen)) {
      err = "vorbis: not an Ogg page";
      return false;
    }
    const uint8_t* p = data + pktOff;
    if (pktLen < 28 || p[0] != 0x01 || !hasMagic(p, pktLen, 1, "vorbis", 6)) {
      err = "vorbis: first packet is not the identification header";
      return false;
    }
    if (rd32le(p + 7) != 0) {
      err = "vorbis: bad identification version";
      return false;
    }
    channels_ = p[11];
    sampleRate_ = rd32le(p + 12);
    if (channels_ == 0 || channels_ > 8 || sampleRate_ == 0) {
      err = "vorbis: bad channels/rate in identification header";
      return false;
    }
    return true;
  }

  AudioFormatInfo format() const override {
    AudioFormatInfo f;
    f.sampleRate = sampleRate_;
    f.channels = channels_;
    f.bitsPerSample = 0;  // compressed
    f.totalFrames = 0;    // needs granule-position scan; builder
    f.valid = true;
    return f;
  }

  size_t decode(float* /*outStereo*/, size_t /*maxFrames*/, std::string& err) override {
    // TODO(builder): vendor stb_vorbis (public domain / MIT) or link
    // libvorbisfile (BSD); handle chained streams and sample-rate conversion.
    err = "vorbis: packet decoding not implemented (stub; see builder notes)";
    return 0;
  }

 private:
  uint32_t sampleRate_ = 0;
  uint16_t channels_ = 0;
};

}  // namespace

AudioCodec sniffAudioCodec(const uint8_t* data, size_t size) {
  if (!data || size == 0) return AudioCodec::kUnknown;
  if (hasMagic(data, size, 0, "RIFF", 4) && hasMagic(data, size, 8, "WAVE", 4))
    return AudioCodec::kWav;
  if (hasMagic(data, size, 0, "fLaC", 4)) return AudioCodec::kFlac;
  if (hasMagic(data, size, 0, "OggS", 4)) {
    if (contains(data, size, "OpusHead", 8)) return AudioCodec::kOpus;
    if (contains(data, size, "\x01vorbis", 7)) return AudioCodec::kVorbis;
  }
  return AudioCodec::kUnknown;
}

std::unique_ptr<IAudioDecoder> makeAudioDecoder(const uint8_t* data, size_t size,
                                                std::string& err) {
  std::unique_ptr<IAudioDecoder> dec;
  switch (sniffAudioCodec(data, size)) {
    case AudioCodec::kWav: dec = std::make_unique<WavDecoder>(); break;
    case AudioCodec::kFlac: dec = std::make_unique<FlacDecoder>(); break;
    case AudioCodec::kOpus: dec = std::make_unique<OpusDecoder>(); break;
    case AudioCodec::kVorbis: dec = std::make_unique<VorbisDecoder>(); break;
    case AudioCodec::kUnknown: break;
  }
  if (!dec) {
    err = "codec: unrecognized format";
    return nullptr;
  }
  if (!dec->open(data, size, err)) return nullptr;
  return dec;
}

AudioStreamer::DecodeFn adaptDecoderToDecodeFn(IAudioDecoder* decoder) {
  if (!decoder) {
    logError("audio: adaptDecoderToDecodeFn got a null decoder; the stream will be silent");
    return [](float* /*out*/, size_t /*maxFrames*/) { return size_t(0); };
  }
  if (!decoder->implemented()) {
    // Up-front check: a stub decoder must never reach a streamer as silent
    // end-of-stream. The reason is logged once here (wire-up time, any
    // thread); the functor itself just reports EOS on the worker thread.
    logError(
        "audio: codec %d is a container-only stub (frame decoding not implemented); "
        "the stream will be silent until a real decoder is wired",
        static_cast<int>(decoder->codec()));
    return [](float* /*out*/, size_t /*maxFrames*/) { return size_t(0); };
  }
  return [decoder](float* out, size_t maxFrames) {
    std::string err;
    const size_t got = decoder->decode(out, maxFrames, err);
    // Logged at most once per stream in practice: the first short read ends it.
    if (!err.empty()) logError("audio: decode failed: %s", err.c_str());
    return got;
  };
}

}  // namespace litholattice
