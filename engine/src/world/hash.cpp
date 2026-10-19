// See hash.h. CRC-32/ISO-HDLC, byte-wise and platform-deterministic.
// The table is generated at startup (same construction as
// litholattice::storage::crc32) rather than hand-written.
#include "litholattice/world/hash.h"

#include <array>

namespace litholattice {

uint32_t crc32(const uint8_t* data, size_t n) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

}  // namespace litholattice
