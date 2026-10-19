// SHA-256 (FIPS 180-4), clean-room implementation for LithoLattice.
// Used for the canonical mod-directory hash. No external dependency;
// verified against the NIST test vectors in crypto_selftest.cpp.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace litholattice::crypto {

class Sha256 {
 public:
  Sha256();
  void update(const uint8_t* data, size_t len);
  void update(const char* s);  // NUL-terminated convenience
  std::array<uint8_t, 32> final();

 private:
  uint32_t h_[8];
  uint64_t total_ = 0;  // bytes fed so far
  uint8_t buf_[64];
  size_t buflen_ = 0;
  void compress(const uint8_t block[64]);
};

/// One-shot helper.
inline std::array<uint8_t, 32> sha256(const uint8_t* data, size_t len) {
  Sha256 s;
  s.update(data, len);
  return s.final();
}

}  // namespace litholattice::crypto
