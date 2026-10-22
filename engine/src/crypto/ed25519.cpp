// Ed25519 (RFC 8032), clean-room implementation.
// Field: radix-2^64, 4 limbs, p = 2^255 - 19. Curve: Edwards25519
//   -x^2 + y^2 = 1 + d*x^2*y^2, d = -121665 * inv(121666) mod p.
// No third-party code.
#include "litholattice/crypto/ed25519.h"

#include <cstring>

#include "litholattice/crypto/sha512.h"
#include "litholattice/crypto/uint128.h"

namespace litholattice::crypto::ed25519 {
namespace {

// ---------- field arithmetic, p = 2^255 - 19 ----------
struct Fe {
  uint64_t w[4] = {0, 0, 0, 0};  // little-endian, radix 2^64
};

constexpr uint64_t kP[4] = {0xffffffffffffffedULL, 0xffffffffffffffffULL,
                            0xffffffffffffffffULL, 0x7fffffffffffffffULL};

// Reduce 8-limb (512-bit) t mod p into r. Uses 2^256 = 38 (mod p).
void fe_reduce(Fe& r, const uint64_t t[8]) {
  uint64_t w[4];
  uint64_t carry = 0;
  for (int i = 0; i < 4; ++i) {
    U128 s = muladd(t[i + 4], 38, t[i], carry);
    w[i] = s.lo;
    carry = s.hi;
  }
  // carry * 2^256 = carry * 38
  U128 c = mul64(carry, 38);
  U128 s = add3(w[0], c.lo, 0);
  w[0] = s.lo;
  uint64_t cc = s.hi + c.hi;
  for (int i = 1; i < 4 && cc; ++i) {
    U128 s2 = add3(w[i], cc, 0);
    w[i] = s2.lo;
    cc = s2.hi;
  }
  // w < 2^257 now; subtract p until < p (at most 2 iterations).
  for (int k = 0; k < 3; ++k) {
    bool ge = w[3] > kP[3] || (w[3] == kP[3] && (w[2] > kP[2] || (w[2] == kP[2] && (w[1] > kP[1] || (w[1] == kP[1] && w[0] >= kP[0])))));
    if (!ge) break;
    uint64_t borrow = 0;
    for (int i = 0; i < 4; ++i) {
      U128 d = sub3(w[i], kP[i], borrow);
      w[i] = d.lo;
      borrow = d.hi & 1u;
    }
  }
  std::memcpy(r.w, w, sizeof w);
}

void fe_add(Fe& r, const Fe& a, const Fe& b) {
  uint64_t t[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  uint64_t carry = 0;
  for (int i = 0; i < 4; ++i) {
    U128 s = add3(a.w[i], b.w[i], carry);
    t[i] = s.lo;
    carry = s.hi;
  }
  t[4] = carry;
  fe_reduce(r, t);
}

void fe_sub(Fe& r, const Fe& a, const Fe& b) {
  // r = a - b mod p = a + (p - b)
  Fe pb;
  std::memcpy(pb.w, kP, sizeof kP);
  Fe negb;
  {
    uint64_t borrow = 0;
    for (int i = 0; i < 4; ++i) {
      U128 d = sub3(pb.w[i], b.w[i], borrow);
      negb.w[i] = d.lo;
      borrow = d.hi & 1u;
    }
  }
  fe_add(r, a, negb);
}

void fe_mul(Fe& r, const Fe& a, const Fe& b) {
  uint64_t t[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 4; ++i) {
    uint64_t carry = 0;
    for (int j = 0; j < 4; ++j) {
      U128 s = muladd(a.w[i], b.w[j], t[i + j], carry);
      t[i + j] = s.lo;
      carry = s.hi;
    }
    t[i + 4] = carry;
  }
  fe_reduce(r, t);
}

void fe_sq(Fe& r, const Fe& a) { fe_mul(r, a, a); }

bool fe_equal(const Fe& a, const Fe& b) {
  return a.w[0] == b.w[0] && a.w[1] == b.w[1] && a.w[2] == b.w[2] && a.w[3] == b.w[3];
}

bool fe_is_zero(const Fe& a) { return (a.w[0] | a.w[1] | a.w[2] | a.w[3]) == 0; }

void fe_from_bytes(Fe& r, const uint8_t b[32]) {
  for (int i = 0; i < 4; ++i) {
    r.w[i] = 0;
    for (int j = 0; j < 8; ++j) r.w[i] |= (uint64_t)b[i * 8 + j] << (8 * j);
  }
  // reduce in case input >= p (only low bit 255 matters for our uses)
  r.w[3] &= 0x7fffffffffffffffULL;
}

void fe_to_bytes(uint8_t b[32], const Fe& a) {
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 8; ++j) b[i * 8 + j] = (uint8_t)(a.w[i] >> (8 * j));
}

// a^(e) via square-and-multiply; e is 256-bit little-endian bytes.
void fe_pow(Fe& r, const Fe& a, const uint8_t e[32]) {
  Fe result;
  result.w[0] = 1;
  Fe base;
  std::memcpy(base.w, a.w, sizeof base.w);
  for (int i = 0; i < 256; ++i) {
    if ((e[i / 8] >> (i % 8)) & 1) {
      Fe t;
      fe_mul(t, result, base);
      std::memcpy(result.w, t.w, sizeof result.w);
    }
    Fe t2;
    fe_sq(t2, base);
    std::memcpy(base.w, t2.w, sizeof base.w);
  }
  std::memcpy(r.w, result.w, sizeof r.w);
}

void fe_inv(Fe& r, const Fe& a) {
  // a^(p-2), p-2 = 2^255 - 21
  uint8_t e[32] = {0};
  e[0] = 0xeb;  // 2^255 - 21 = ...11101011 low byte; rest 0xff except top
  for (int i = 1; i < 31; ++i) e[i] = 0xff;
  e[31] = 0x7f;  // top bit clear (2^255 - 21 < 2^255)
  fe_pow(r, a, e);
}

// ---------- Edwards25519 ----------
// -x^2 + y^2 = 1 + d x^2 y^2, d = -121665 * inv(121666) mod p
struct Point {
  Fe x, y;
  bool inf = true;  // identity = (0,1)
};

Fe& curve_d() {
  static Fe d;
  static bool init = false;
  if (!init) {
    Fe n, m, inv, neg;
    n.w[0] = 121665;
    m.w[0] = 121666;
    fe_inv(inv, m);
    fe_mul(d, n, inv);
    // negate: d = p - d
    Fe p;
    std::memcpy(p.w, kP, sizeof kP);
    fe_sub(neg, p, d);
    std::memcpy(d.w, neg.w, sizeof d.w);
    init = true;
  }
  return d;
}

void point_add(Point& r, const Point& p, const Point& q) {
  if (p.inf) {
    r = q;
    return;
  }
  if (q.inf) {
    r = p;
    return;
  }
  Fe& d = curve_d();
  Fe x1y2, y1x2, y1y2, x1x2, dxy;
  fe_mul(x1y2, p.x, q.y);
  fe_mul(y1x2, p.y, q.x);
  fe_mul(y1y2, p.y, q.y);
  fe_mul(x1x2, p.x, q.x);
  Fe tmp;
  fe_mul(tmp, x1x2, y1y2);  // x1*x2*y1*y2
  fe_mul(dxy, d, tmp);      // d*x1*x2*y1*y2
  Fe one;
  one.w[0] = 1;
  Fe den_x, den_y, num_x, num_y;
  fe_add(den_x, one, dxy);  // 1 + d*x1*x2*y1*y2
  fe_sub(den_y, one, dxy);  // 1 - d*x1*x2*y1*y2
  fe_add(num_x, x1y2, y1x2);
  fe_add(num_y, y1y2, x1x2);  // a=-1: y3 = (y1*y2 + x1*x2)/(1 - d*x1*x2*y1*y2)
  Fe ix, iy;
  fe_inv(ix, den_x);
  fe_inv(iy, den_y);
  fe_mul(r.x, num_x, ix);
  fe_mul(r.y, num_y, iy);
  r.inf = false;
}

void point_double(Point& r, const Point& p) { point_add(r, p, p); }

// scalar as 32 bytes little-endian
void scalar_mul(Point& r, const Point& p, const uint8_t s[32]) {
  Point acc;  // identity
  Point addend = p;
  for (int i = 0; i < 256; ++i) {
    if ((s[i / 8] >> (i % 8)) & 1) {
      Point t;
      point_add(t, acc, addend);
      acc = t;
    }
    Point t2;
    point_double(t2, addend);
    addend = t2;
  }
  r = acc;
}

// Compress: 255 bits of y, top bit = x odd.
void point_compress(uint8_t out[32], const Point& p) {
  fe_to_bytes(out, p.y);
  // x odd iff x.w[0] & 1
  if (p.x.w[0] & 1) out[31] |= 0x80;
}

// Decompress. Returns false if invalid.
bool point_decompress(Point& p, const uint8_t in[32]) {
  Fe y;
  fe_from_bytes(y, in);
  // y < p check: fe_from_bytes masks top bit; also reject y >= p via compare
  {
    bool ge = y.w[3] > kP[3] || (y.w[3] == kP[3] && (y.w[2] > kP[2] || (y.w[2] == kP[2] && (y.w[1] > kP[1] || (y.w[1] == kP[1] && y.w[0] >= kP[0])))));
    if (ge) return false;
  }
  const bool x_odd = (in[31] >> 7) & 1;
  Fe& d = curve_d();
  // x^2 = (y^2 - 1) / (d*y^2 + 1)
  Fe y2, num, den, inv_den, x2;
  fe_sq(y2, y);
  Fe one;
  one.w[0] = 1;
  fe_sub(num, y2, one);
  fe_mul(den, d, y2);
  fe_add(den, den, one);
  if (fe_is_zero(den)) return false;
  fe_inv(inv_den, den);
  fe_mul(x2, num, inv_den);
  // x = x2^((p+3)/8); (p+3)/8 = 2^252 - 2
  uint8_t e[32] = {0};
  e[0] = 0xfe;  // 2^252 - 2: low byte 0xfe, bytes 1..30 = 0xff, byte 31 = 0x0f
  for (int i = 1; i < 31; ++i) e[i] = 0xff;
  e[31] = 0x0f;
  Fe x;
  fe_pow(x, x2, e);
  // check: x^2 == x2, else try x * 2^((p-1)/4)
  Fe check;
  fe_sq(check, x);
  if (!fe_equal(check, x2)) {
    // multiply by sqrt(-1) = 2^((p-1)/4)
    uint8_t e2[32] = {0};
    // (p-1)/4 = 2^253 - 5
    e2[0] = 0xfb;
    for (int i = 1; i < 31; ++i) e2[i] = 0xff;
    e2[31] = 0x1f;
    Fe sqrtm1;
    Fe two;
    two.w[0] = 2;
    fe_pow(sqrtm1, two, e2);
    Fe x2b;
    fe_mul(x2b, x, sqrtm1);
    std::memcpy(x.w, x2b.w, sizeof x.w);
    fe_sq(check, x);
    if (!fe_equal(check, x2)) return false;
  }
  if (((x.w[0] & 1) != 0) != x_odd) {
    // negate x
    Fe p;
    std::memcpy(p.w, kP, sizeof kP);
    Fe nx;
    fe_sub(nx, p, x);
    std::memcpy(x.w, nx.w, sizeof x.w);
  }
  std::memcpy(p.x.w, x.w, sizeof p.x.w);
  std::memcpy(p.y.w, y.w, sizeof p.y.w);
  p.inf = false;
  return true;
}

const Point& base_point() {
  static Point g;
  static bool init = false;
  if (!init) {
    // compressed base point: 0x58 followed by 31 0x66 bytes
    uint8_t comp[32];
    comp[0] = 0x58;
    for (int i = 1; i < 32; ++i) comp[i] = 0x66;
    if (!point_decompress(g, comp)) {
      // unreachable: the base point is valid
      std::memset(&g, 0, sizeof g);
    }
    init = true;
  }
  return g;
}

// ---------- scalars mod L ----------
// L = 2^252 + 27742317777372353535851937790883648493
constexpr uint64_t kL[4] = {0x5812631a5cf5d3edULL, 0x14def9dea2f79cd6ULL, 0x0000000000000000ULL,
                            0x1000000000000000ULL};

struct Scalar {
  uint64_t w[4] = {0, 0, 0, 0};
};

void scalar_from_bytes(Scalar& s, const uint8_t b[32]) {
  for (int i = 0; i < 4; ++i) {
    s.w[i] = 0;
    for (int j = 0; j < 8; ++j) s.w[i] |= (uint64_t)b[i * 8 + j] << (8 * j);
  }
}

void scalar_to_bytes(uint8_t b[32], const Scalar& s) {
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 8; ++j) b[i * 8 + j] = (uint8_t)(s.w[i] >> (8 * j));
}

// Compare 8-limb a with 8-limb b. Returns -1/0/1.
int cmp512(const uint64_t a[8], const uint64_t b[8]) {
  for (int i = 7; i >= 0; --i) {
    if (a[i] < b[i]) return -1;
    if (a[i] > b[i]) return 1;
  }
  return 0;
}

void sub512(uint64_t r[8], const uint64_t a[8], const uint64_t b[8]) {
  uint64_t borrow = 0;
  for (int i = 0; i < 8; ++i) {
    U128 d = sub3(a[i], b[i], borrow);
    r[i] = d.lo;
    borrow = d.hi & 1u;
  }
}

// Reduce a 64-byte (512-bit) little-endian value mod L.
void scalar_reduce(Scalar& s, const uint8_t b[64]) {
  uint64_t v[8];
  for (int i = 0; i < 8; ++i) {
    v[i] = 0;
    for (int j = 0; j < 8; ++j) v[i] |= (uint64_t)b[i * 8 + j] << (8 * j);
  }
  uint64_t l8[8] = {kL[0], kL[1], kL[2], kL[3], 0, 0, 0, 0};
  // shift-subtract from bit 511 down
  for (int shift = 511 - 252; shift >= 0; --shift) {
    // t = L << shift (512-bit)
    uint64_t t[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    const int word = shift / 64, bit = shift % 64;
    for (int i = 0; i < 8; ++i) {
      if (i + word < 8) {
        t[i + word] |= l8[i] << bit;
        if (bit && i + word + 1 < 8) t[i + word + 1] |= l8[i] >> (64 - bit);
      }
    }
    if (cmp512(v, t) >= 0) sub512(v, v, t);
  }
  for (int i = 0; i < 4; ++i) s.w[i] = v[i];
}

void scalar_add(Scalar& r, const Scalar& a, const Scalar& b) {
  uint8_t buf[64] = {0};
  uint64_t carry = 0;
  for (int i = 0; i < 4; ++i) {
    U128 s = add3(a.w[i], b.w[i], carry);
    uint64_t lo = s.lo;
    carry = s.hi;
    for (int j = 0; j < 8; ++j) buf[i * 8 + j] = (uint8_t)(lo >> (8 * j));
  }
  // buf[32..64) = 0; reduce
  scalar_reduce(r, buf);
}

void scalar_mul(Scalar& r, const Scalar& a, const Scalar& b) {
  uint64_t t[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 4; ++i) {
    uint64_t carry = 0;
    for (int j = 0; j < 4; ++j) {
      U128 s = muladd(a.w[i], b.w[j], t[i + j], carry);
      t[i + j] = s.lo;
      carry = s.hi;
    }
    t[i + 4] = carry;
  }
  uint8_t buf[64];
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 8; ++j) buf[i * 8 + j] = (uint8_t)(t[i] >> (8 * j));
  scalar_reduce(r, buf);
}

bool scalar_lt_l(const uint8_t s[32]) {
  // check s < L (for signature validation)
  for (int i = 3; i >= 0; --i) {
    uint64_t w = 0;
    for (int j = 0; j < 8; ++j) w |= (uint64_t)s[i * 8 + j] << (8 * j);
    if (w < kL[i]) return true;
    if (w > kL[i]) return false;
  }
  return false;  // equal -> not less
}

}  // namespace

void keypair_from_seed(const Seed& seed, PublicKey& pub, SecretKey& sec) {
  auto h = sha512(seed.data(), seed.size());
  uint8_t a[32];
  std::memcpy(a, h.data(), 32);
  a[0] &= 248;
  a[31] &= 63;
  a[31] |= 64;
  Point A;
  scalar_mul(A, base_point(), a);
  point_compress(pub.data(), A);
  std::memcpy(sec.data(), seed.data(), 32);
  std::memcpy(sec.data() + 32, pub.data(), 32);
}

Signature sign(const uint8_t* msg, size_t msglen, const SecretKey& sec) {
  auto h = sha512(sec.data(), 32);  // seed = sec[0..32)
  uint8_t a[32];
  std::memcpy(a, h.data(), 32);
  a[0] &= 248;
  a[31] &= 63;
  a[31] |= 64;
  // r = SHA512(h[32..64] || msg) mod L
  Sha512 hs;
  hs.update(h.data() + 32, 32);
  hs.update(msg, msglen);
  auto rhash = hs.final();
  Scalar r;
  scalar_reduce(r, rhash.data());
  uint8_t rbytes[32];
  scalar_to_bytes(rbytes, r);
  Point R;
  scalar_mul(R, base_point(), rbytes);
  uint8_t Rcomp[32];
  point_compress(Rcomp, R);
  // S = (r + H(R || pub || msg) * a) mod L
  const uint8_t* pub = sec.data() + 32;
  Sha512 hh;
  hh.update(Rcomp, 32);
  hh.update(pub, 32);
  hh.update(msg, msglen);
  auto hram = hh.final();
  Scalar k, ka, S;
  scalar_reduce(k, hram.data());
  Scalar sa;
  scalar_from_bytes(sa, a);
  scalar_mul(ka, k, sa);
  scalar_add(S, r, ka);
  Signature sig;
  std::memcpy(sig.data(), Rcomp, 32);
  scalar_to_bytes(sig.data() + 32, S);
  return sig;
}

bool verify(const Signature& sig, const uint8_t* msg, size_t msglen, const PublicKey& pub) {
  // Decode R
  Point R;
  if (!point_decompress(R, sig.data())) return false;
  // S < L and canonical
  if (!scalar_lt_l(sig.data() + 32)) return false;
  // Decode A
  Point A;
  if (!point_decompress(A, pub.data())) return false;
  // h = H(R || pub || msg) mod L
  Sha512 hh;
  hh.update(sig.data(), 32);
  hh.update(pub.data(), 32);
  hh.update(msg, msglen);
  auto hram = hh.final();
  Scalar k;
  scalar_reduce(k, hram.data());
  uint8_t kbytes[32];
  scalar_to_bytes(kbytes, k);
  // Check S*G == R + k*A
  Point SG, kA, rhs;
  scalar_mul(SG, base_point(), sig.data() + 32);
  scalar_mul(kA, A, kbytes);
  point_add(rhs, R, kA);
  if (SG.inf != rhs.inf) return false;
  if (SG.inf) return true;  // both identity (degenerate but consistent)
  return fe_equal(SG.x, rhs.x) && fe_equal(SG.y, rhs.y);
}

}  // namespace litholattice::crypto::ed25519
