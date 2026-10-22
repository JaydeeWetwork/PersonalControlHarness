// uint128.h — minimal 128-bit unsigned arithmetic for the clean-room Ed25519
// field/scalar code. MSVC has no __uint128_t, so the field arithmetic goes
// through these helpers; GCC/Clang fast-path the 64x64 multiply through the
// native type. No third-party code.
#pragma once
#include <cstdint>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>  // _umul128
#endif

namespace litholattice::crypto {

/// Unsigned 128-bit value as (hi, lo). Only the operations Ed25519 needs
/// (64x64->128 multiply, add/sub of two 64-bit limbs with carry/borrow).
struct U128 {
  uint64_t lo = 0;
  uint64_t hi = 0;
};

/// 64 x 64 -> 128.
inline U128 mul64(uint64_t a, uint64_t b) {
  U128 r;
#if defined(_MSC_VER) && !defined(__clang__)
  r.lo = _umul128(a, b, &r.hi);
#else
  const unsigned __int128 t = (unsigned __int128)a * (unsigned __int128)b;
  r.lo = (uint64_t)t;
  r.hi = (uint64_t)(t >> 64);
#endif
  return r;
}

/// (a * b + c + carry) -> 128. a*b is a full 128-bit product; c and carry are
/// 64-bit addends folded in with carry propagation.
inline U128 muladd(uint64_t a, uint64_t b, uint64_t c, uint64_t carry) {
  U128 r = mul64(a, b);
  uint64_t old = r.lo;
  r.lo += c;
  if (r.lo < old) ++r.hi;
  old = r.lo;
  r.lo += carry;
  if (r.lo < old) ++r.hi;
  return r;
}

/// (a + b + c) -> 128.
inline U128 add3(uint64_t a, uint64_t b, uint64_t c) {
  U128 r;
  r.lo = a;
  uint64_t old = r.lo;
  r.lo += b;
  if (r.lo < old) ++r.hi;
  old = r.lo;
  r.lo += c;
  if (r.lo < old) ++r.hi;
  return r;
}

/// (a - b - borrow) -> 128. `borrow` is 0 or 1. On underflow the result's high
/// word is all-ones, so `r.hi & 1` recovers the borrow-out exactly like
/// `(uint64_t)((__uint128_t)a - b - borrow >> 64) & 1` does on GCC/Clang.
inline U128 sub3(uint64_t a, uint64_t b, uint64_t borrow) {
  U128 r;
  const uint64_t lo1 = a - b;
  const uint64_t borrow1 = (a < b) ? 1u : 0u;
  r.lo = lo1 - borrow;
  const uint64_t borrow2 = (lo1 < borrow) ? 1u : 0u;
  r.hi = (borrow1 || borrow2) ? ~uint64_t(0) : 0u;
  return r;
}

}  // namespace litholattice::crypto
