// Clean-room base64 + minimal PNG encoder for the MCP senses surface.
// No third-party code: both are written from the format specs.
#include "litholattice/mcp/mcp_senses.h"

#include <array>

namespace litholattice {
namespace mcp {
namespace {

// --- CRC-32 (ISO 3309, PNG chunk checksums) --------------------------------
// Fix 2026-10-11 (regression, LOW): the table was filled by an unsynchronized
// lazy init (formal data race on concurrent first captures; benign in practice
// since writes are idempotent, and unreachable today with single-threaded
// transports). A function-local static is thread-safe by construction (C++11
// magic statics).
const std::array<uint32_t, 256>& crcTable() {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
    return t;
  }();
  return table;
}

uint32_t crc32(const uint8_t* data, size_t len) {
  const auto& table = crcTable();
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

// --- Adler-32 (zlib stream checksum) ---------------------------------------
uint32_t adler32(const uint8_t* data, size_t len) {
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < len; ++i) {
    a = (a + data[i]) % 65521;
    b = (b + a) % 65521;
  }
  return (b << 16) | a;
}

void putU32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 24));
  out.push_back(uint8_t(v >> 16));
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

void putChunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data,
              size_t len) {
  putU32(out, uint32_t(len));
  const size_t typePos = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data, data + len);
  putU32(out, crc32(out.data() + typePos, 4 + len));
}

}  // namespace

std::string base64Encode(const uint8_t* data, size_t len) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t n = uint32_t(data[i]) << 16 |
                       (i + 1 < len ? uint32_t(data[i + 1]) << 8 : 0) |
                       (i + 2 < len ? uint32_t(data[i + 2]) : 0);
    out.push_back(kAlphabet[(n >> 18) & 63]);
    out.push_back(kAlphabet[(n >> 12) & 63]);
    out.push_back(i + 1 < len ? kAlphabet[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < len ? kAlphabet[n & 63] : '=');
  }
  return out;
}

bool encodePngRgb(int width, int height, const uint8_t* rgb,
                  std::vector<uint8_t>& out) {
  if (width <= 0 || height <= 0 || !rgb) return false;
  if (width > 4096 || height > 4096) return false;  // sanity: never huge
  out.clear();

  // Signature.
  static const uint8_t kSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  out.insert(out.end(), kSig, kSig + 8);

  // IHDR: width, height, bit depth 8, color type 2 (RGB), rest 0.
  uint8_t ihdr[13];
  ihdr[0] = uint8_t(width >> 24); ihdr[1] = uint8_t(width >> 16);
  ihdr[2] = uint8_t(width >> 8);  ihdr[3] = uint8_t(width);
  ihdr[4] = uint8_t(height >> 24); ihdr[5] = uint8_t(height >> 16);
  ihdr[6] = uint8_t(height >> 8);  ihdr[7] = uint8_t(height);
  ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  putChunk(out, "IHDR", ihdr, 13);

  // IDAT: zlib stream (header 0x78 0x01 = 32K window, fastest) wrapping
  // DEFLATE stored blocks (BTYPE=00). One filter byte (0 = None) starts
  // every scanline.
  const size_t rowBytes = size_t(width) * 3;
  const size_t rawLen = (rowBytes + 1) * size_t(height);
  std::vector<uint8_t> raw;
  raw.reserve(rawLen);
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb + size_t(y) * rowBytes, rgb + size_t(y + 1) * rowBytes);
  }

  std::vector<uint8_t> zlib;
  zlib.reserve(rawLen + 16 + (rawLen / 65535 + 1) * 5);
  zlib.push_back(0x78);
  zlib.push_back(0x01);
  size_t pos = 0;
  while (pos < rawLen) {
    const size_t n = rawLen - pos > 65535 ? 65535 : rawLen - pos;
    const bool last = pos + n == rawLen;
    zlib.push_back(last ? 0x01 : 0x00);  // BFINAL + BTYPE=00 (stored)
    zlib.push_back(uint8_t(n & 0xFF));
    zlib.push_back(uint8_t(n >> 8));
    zlib.push_back(uint8_t((~n) & 0xFF));
    zlib.push_back(uint8_t((~n) >> 8));
    zlib.insert(zlib.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  }
  const uint32_t ad = adler32(raw.data(), rawLen);
  zlib.push_back(uint8_t(ad >> 24));
  zlib.push_back(uint8_t(ad >> 16));
  zlib.push_back(uint8_t(ad >> 8));
  zlib.push_back(uint8_t(ad));
  putChunk(out, "IDAT", zlib.data(), zlib.size());

  // IEND.
  putChunk(out, "IEND", nullptr, 0);
  return true;
}

}  // namespace mcp
}  // namespace litholattice
