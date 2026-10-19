#include "litholattice/net/quantize.h"

namespace litholattice {
namespace net {
namespace quantize {

void ByteWriter::putVarint(int32_t v) {
  // DDNet-style: sign in bit 6 of the first byte, 6 magnitude bits there,
  // then 7 magnitude bits per continuation byte.
  uint32_t mag;
  uint8_t sign = 0;
  if (v < 0) {
    sign = 0x40;
    // Avoid UB on INT32_MIN: negate in 64-bit.
    mag = static_cast<uint32_t>(-(static_cast<int64_t>(v)));
  } else {
    mag = static_cast<uint32_t>(v);
  }
  uint8_t b0 = static_cast<uint8_t>(mag & 0x3F) | sign;
  mag >>= 6;
  if (mag) b0 |= 0x80;
  buf.push_back(b0);
  while (mag) {
    uint8_t b = static_cast<uint8_t>(mag & 0x7F);
    mag >>= 7;
    if (mag) b |= 0x80;
    buf.push_back(b);
  }
}

void ByteWriter::putUVarint(uint32_t v) {
  do {
    uint8_t b = static_cast<uint8_t>(v & 0x7F);
    v >>= 7;
    if (v) b |= 0x80;
    buf.push_back(b);
  } while (v);
}

void ByteWriter::putUVarint64(uint64_t v) {
  do {
    uint8_t b = static_cast<uint8_t>(v & 0x7F);
    v >>= 7;
    if (v) b |= 0x80;
    buf.push_back(b);
  } while (v);
}

bool ByteReader::getU8(uint8_t& v) {
  if (p_ == end_) return false;
  v = *p_++;
  return true;
}

bool ByteReader::getU16LE(uint16_t& v) {
  if (remaining() < 2) return false;
  v = static_cast<uint16_t>(p_[0] | (p_[1] << 8));
  p_ += 2;
  return true;
}

bool ByteReader::getU32LE(uint32_t& v) {
  if (remaining() < 4) return false;
  v = static_cast<uint32_t>(p_[0]) | (static_cast<uint32_t>(p_[1]) << 8) |
      (static_cast<uint32_t>(p_[2]) << 16) | (static_cast<uint32_t>(p_[3]) << 24);
  p_ += 4;
  return true;
}

bool ByteReader::getVarint(int32_t& v) {
  uint8_t b;
  if (!getU8(b)) return false;
  const bool neg = (b & 0x40) != 0;
  uint32_t mag = b & 0x3F;
  unsigned shift = 6;
  while (b & 0x80) {
    if (shift >= 32) return false;  // malformed: would overflow int32
    if (!getU8(b)) return false;
    mag |= static_cast<uint32_t>(b & 0x7F) << shift;
    shift += 7;
  }
  if (neg) {
    // mag == 2^31 encodes INT32_MIN; anything larger is malformed.
    if (mag > 0x80000000u) return false;
    v = (mag == 0x80000000u) ? INT32_MIN : -static_cast<int32_t>(mag);
  } else {
    if (mag > 0x7FFFFFFFu) return false;
    v = static_cast<int32_t>(mag);
  }
  return true;
}

bool ByteReader::getUVarint(uint32_t& v) {
  // At most 5 bytes (5 x 7 = 35 bits >= 32); a 6th byte is malformed.
  // 64-bit accumulator so the shifts can never overflow.
  uint64_t mag = 0;
  unsigned shift = 0;
  for (int i = 0; i < 5; ++i) {
    uint8_t b;
    if (!getU8(b)) return false;
    mag |= static_cast<uint64_t>(b & 0x7F) << shift;
    shift += 7;
    if (!(b & 0x80)) {
      if (mag > UINT32_MAX) return false;
      v = static_cast<uint32_t>(mag);
      return true;
    }
  }
  return false;
}

bool ByteReader::getUVarint64(uint64_t& v) {
  // At most 10 bytes (10 x 7 = 70 bits >= 64); an 11th byte is malformed.
  // The 10th byte may only carry bit 63: bit 0 set, bits 1-7 clear (bit 7 is
  // the continuation flag, so a set bit 7 here is also malformed).
  // Non-minimal encodings are rejected (review F-L2): a multi-byte form
  // whose FINAL group is zero ({0x85,0x00} for 5, {0x80,0x00} for 0) is not
  // canonical — two byte strings decoding to one value would break any
  // byte-equality/transcript-signing scheme. The in-tree encoder
  // (putUVarint64) is minimal by construction, so nothing it produces is
  // affected. On rejection `v` is left untouched, like the other failures.
  uint64_t mag = 0;
  unsigned shift = 0;
  for (int i = 0; i < 10; ++i) {
    uint8_t b;
    if (!getU8(b)) return false;
    if (i == 9) {
      if (b & 0xFE) return false;
      if ((b & 0x01) == 0) return false;  // 10-byte form with bit 63 clear: non-minimal
      v = mag | (static_cast<uint64_t>(b & 0x01) << 63);
      return true;
    }
    mag |= static_cast<uint64_t>(b & 0x7F) << shift;
    shift += 7;
    if (!(b & 0x80)) {
      if (i > 0 && (b & 0x7F) == 0) return false;  // final zero group: non-minimal
      v = mag;
      return true;
    }
  }
  return false;
}
// --- smallest-three quaternion packing -------------------------------------

namespace {
// Bound for the three stored components: the dropped (largest) component has
// |c| >= 0.5, so each stored component satisfies |c| <= sqrt(1 - 0.25).
constexpr double kQuatBound = 0.7071067811865476;  // 1/sqrt(2)
}  // namespace

uint32_t packQuat(double x, double y, double z, double w) {
  // Non-finite input -> identity quaternion encoding. static_cast<uint32_t>(NaN)
  // is UB; never let NaN reach the floor/cast below.
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(w)) {
    x = 0.0; y = 0.0; z = 0.0; w = 1.0;
  }
  const double c[4] = {x, y, z, w};
  unsigned idx = 0;
  double best = 0.0;
  for (unsigned i = 0; i < 4; ++i) {
    const double a = std::fabs(c[i]);
    if (a > best) {
      best = a;
      idx = i;
    }
  }
  // Sign trick: q == -q as a rotation; make the largest component positive.
  const double s = (c[idx] < 0.0) ? -1.0 : 1.0;
  uint32_t packed = idx & 0x3u;
  unsigned slot = 0;
  for (unsigned i = 0; i < 4; ++i) {
    if (i == idx) continue;
    double v = s * c[i];
    // Clamp defensively: a non-unit input quaternion must not overflow 9 bits.
    if (v > kQuatBound) v = kQuatBound;
    if (v < -kQuatBound) v = -kQuatBound;
    const auto q = static_cast<uint32_t>(std::floor((v + kQuatBound) * (511.0 / (2.0 * kQuatBound)) + 0.5));
    packed |= (q & 0x1FFu) << (2 + 9 * slot);
    ++slot;
  }
  return packed;
}

void unpackQuat(uint32_t packed, double& x, double& y, double& z, double& w) {
  const unsigned idx = packed & 0x3u;
  double c[4] = {0.0, 0.0, 0.0, 0.0};
  double sumSq = 0.0;
  unsigned slot = 0;
  for (unsigned i = 0; i < 4; ++i) {
    if (i == idx) continue;
    const uint32_t q = (packed >> (2 + 9 * slot)) & 0x1FFu;
    c[i] = static_cast<double>(q) * (2.0 * kQuatBound / 511.0) - kQuatBound;
    sumSq += c[i] * c[i];
    ++slot;
  }
  // Dropped component is the positive root (see packQuat sign trick).
  double rest = 1.0 - sumSq;
  c[idx] = rest > 0.0 ? std::sqrt(rest) : 0.0;
  x = c[0];
  y = c[1];
  z = c[2];
  w = c[3];
}

}  // namespace quantize
}  // namespace net
}  // namespace litholattice
