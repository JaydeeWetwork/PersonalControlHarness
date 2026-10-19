// SHA-512 (FIPS 180-4), clean-room implementation for LithoLattice.
// Used internally by Ed25519 (RFC 8032 mandates SHA-512). Verified against
// the NIST test vectors in crypto_selftest.cpp.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace litholattice::crypto {

class Sha512 {
 public:
  Sha512();
  void update(const uint8_t* data, size_t len);
  std::array<uint8_t, 64> final();

 private:
  uint64_t h_[8];
  __uint128_t total_ = 0;  // bytes fed so far
  uint8_t buf_[128];
  size_t buflen_ = 0;
  void compress(const uint8_t block[128]);
};

/// One-shot helper.
inline std::array<uint8_t, 64> sha512(const uint8_t* data, size_t len) {
  Sha512 s;
  s.update(data, len);
  return s.final();
}

}  // namespace litholattice::crypto
