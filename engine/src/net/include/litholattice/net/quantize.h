// Fixed-point quantization and byte-buffer primitives for the net layer.
//
// Everything on the wire is integer: positions/velocities are Q10 fixed point
// (1/1024 block), orientations use Fiedler's "smallest three" packing, and all
// signed deltas go through DDNet-style variable-length integers (continuation
// bit per byte, sign bit in the first byte). No floats cross the wire, so
// there is no float drift between client and server: decode is a pure
// function of the bytes.
//
// Research basis: engine optimization survey section 6.2 (Fiedler, "Snapshot
// Compression": quantize positions to fixed point, smallest-three quaternions,
// 1-bit at-rest flags) and 6.10 (DDNet: delta -> varints -> Huffman; this
// layer implements the delta -> varint stages, Huffman is future work).
//
// DETERMINISM: encode is a pure function of the input values. Quantization
// uses round-to-nearest via std::floor (IEEE-754, correctly rounded, so the
// same double always maps to the same int32 on every platform). The sim's
// determinism is untouched: this layer only observes sim state.
//
// ENDIANNESS: little-endian on the wire (x86/ARM are LE; matches the engine's
// save format convention). Documented here so a future big-endian port knows
// exactly what to flip.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace litholattice {
namespace net {
namespace quantize {

// ---------------------------------------------------------------------------
// Byte buffers (transport-agnostic: the game drives these over UDP later).
// ---------------------------------------------------------------------------

/// Append-only little-endian byte buffer.
class ByteWriter {
 public:
  std::vector<uint8_t> buf;

  void putU8(uint8_t v) { buf.push_back(v); }
  void putU16LE(uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >> 8));
  }
  void putU32LE(uint32_t v) {
    for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  void putBytes(const uint8_t* p, size_t n) { buf.insert(buf.end(), p, p + n); }

  /// Signed variable-length integer, DDNet-style: bit 7 of each byte is the
  /// continuation flag, bit 6 of the FIRST byte is the sign, remaining bits
  /// are magnitude (6 bits in byte 0, then 7 bits per byte). Canonical:
  /// always uses the minimal number of bytes. Max 5 bytes for int32.
  void putVarint(int32_t v);
  /// Unsigned variant (no sign bit; 7 data bits in byte 0). Max 5 bytes.
  void putUVarint(uint32_t v);
  /// Unsigned 64-bit variant (LEB128): 7 data bits per byte, continuation in bit 7.
  /// Max 10 bytes. This is the wire encoding for 64-bit stable IDs (Infinite
  /// Registry, protocol v3): small ids cost 1 byte ("pack the zeros, shift on
  /// transmit"); only astronomically large ids cost 10.
  void putUVarint64(uint64_t v);

  size_t size() const { return buf.size(); }
  const uint8_t* data() const { return buf.data(); }
};

/// Bounds-checked little-endian reader. Every get returns false (and leaves
/// the out-param untouched) on overrun instead of reading out of bounds.
class ByteReader {
 public:
  ByteReader(const uint8_t* d, size_t n) : p_(d), end_(d + n) {}

  bool getU8(uint8_t& v);
  bool getU16LE(uint16_t& v);
  bool getU32LE(uint32_t& v);
  bool getVarint(int32_t& v);
  bool getUVarint(uint32_t& v);
  /// Inverse of putUVarint64. Strict: >10 bytes, a 10th byte carrying bits beyond
  /// bit 63, or truncated input all fail (out-param untouched).
  bool getUVarint64(uint64_t& v);

  bool eof() const { return p_ == end_; }
  size_t remaining() const { return static_cast<size_t>(end_ - p_); }

 private:
  const uint8_t* p_;
  const uint8_t* end_;
};

// ---------------------------------------------------------------------------
// Fixed-point positions and velocities.
// ---------------------------------------------------------------------------

/// Q format: 10 fractional bits, i.e. 1 unit = 1/1024 block. int32 range is
/// +/-2,097,152 blocks, covering every LithoKraft dimension (the Cosmic is
/// +/-262,144). Quantization error is bounded: |q/1024 - d| <= 1/2048 block.
constexpr int kPosQ = 10;
constexpr int32_t kPosScale = 1 << kPosQ;

/// double (blocks) -> Q10 int32. Round-to-nearest, deterministic (IEEE floor).
/// Saturates on overflow/non-finite input: float->int cast of NaN or an
/// out-of-range double is UB.
inline int32_t posToFixed(double blocks) {
  if (!std::isfinite(blocks)) return 0;
  const double v = std::floor(blocks * kPosScale + 0.5);
  if (v >= 2147483647.0) return INT32_MAX;
  if (v <= -2147483648.0) return INT32_MIN;
  return static_cast<int32_t>(v);
}
/// Q10 int32 -> double (blocks). Exact: q / 1024 is always representable.
inline double fixedToPos(int32_t q) { return static_cast<double>(q) / kPosScale; }

/// Velocity: same Q10, units of blocks per tick (512 TPS sim). Saturating, like posToFixed.
inline int32_t velToFixed(double blocksPerTick) {
  if (!std::isfinite(blocksPerTick)) return 0;
  const double v = std::floor(blocksPerTick * kPosScale + 0.5);
  if (v >= 2147483647.0) return INT32_MAX;
  if (v <= -2147483648.0) return INT32_MIN;
  return static_cast<int32_t>(v);
}
inline double fixedToVel(int32_t q) { return static_cast<double>(q) / kPosScale; }

// ---------------------------------------------------------------------------
// Orientation: smallest-three quaternion packing (Fiedler).
// ---------------------------------------------------------------------------

/// Packs a unit quaternion into 29 bits (vs 128 for 4 floats): 2-bit index
/// of the largest-magnitude component + 9 bits each for the other three.
/// Sign trick: q and -q are the same rotation, so negate the whole quaternion
/// when the largest component is negative; the dropped component is then
/// always positive and reconstructed as +sqrt(1 - sum of squares).
/// The other three components live in [-1/sqrt(2), 1/sqrt(2)]; 9 bits over
/// that range gives per-component error < 0.0014 (test asserts < 0.002).
uint32_t packQuat(double x, double y, double z, double w);
/// Inverse of packQuat. sqrt is IEEE correctly-rounded, so the reconstruction
/// is deterministic for a given packed value on every platform.
void unpackQuat(uint32_t packed, double& x, double& y, double& z, double& w);

}  // namespace quantize
}  // namespace net
}  // namespace litholattice
