#include "litholattice/net/encryption.h"

namespace litholattice {
namespace net {

// --- NullCipher: the deliberate-plaintext stub ---------------------------------

CryptoResult NullCipher::seal(const uint8_t* /*plain*/, size_t /*plainLen*/,
                              const uint8_t /*nonce*/[kCryptoNonceBytes], uint8_t* /*out*/,
                              size_t /*outCap*/) {
  // Never copies plaintext through: the game sends the raw payload itself
  // while isPlaintext() is true, so "encrypted" output can never be
  // confused with sealed output.
  return {CryptoError::NotImplemented, 0};
}

CryptoResult NullCipher::open(const uint8_t* /*sealed*/, size_t /*sealedLen*/,
                              const uint8_t /*nonce*/[kCryptoNonceBytes], uint8_t* /*out*/,
                              size_t /*outCap*/) {
  return {CryptoError::NotImplemented, 0};
}

// --- ReplayWindow ------------------------------------------------------------------

bool ReplayWindow::peek(uint64_t counter) const {
  if (!started_) return true;
  if (counter > highest_) return true;
  const uint64_t diff = highest_ - counter;
  if (diff >= 64) return false;  // too old: outside the window
  return (bitmap_ & (1ull << diff)) == 0;  // false when already seen (replay)
}

void ReplayWindow::commit(uint64_t counter) {
  if (!peek(counter)) return;  // defensive: a stale commit changes nothing
  if (!started_) {
    started_ = true;
    highest_ = counter;
    bitmap_ = 1;  // the first counter is seen
    return;
  }
  if (counter > highest_) {
    const uint64_t shift = counter - highest_;
    bitmap_ = (shift >= 64) ? 0 : (bitmap_ << shift);
    bitmap_ |= 1u;  // the new highest is seen
    highest_ = counter;
    return;
  }
  bitmap_ |= 1ull << (highest_ - counter);
}

bool ReplayWindow::accept(uint64_t counter) {
  if (!peek(counter)) return false;
  commit(counter);
  return true;
}

// --- KeyExchange message -------------------------------------------------------------

std::vector<uint8_t> encodeKeyExchange(const KeyExchangeMsg& m) {
  if (m.ephemeralKey.size() > kKexMaxKeyBytes) return {};
  if (m.algorithm != KexAlgorithm::X25519) return {};  // only X25519 is specified
  std::vector<uint8_t> out;
  out.reserve(5 + m.ephemeralKey.size());
  out.push_back(kHsKeyExchange);
  out.push_back(m.initiator ? 1 : 0);
  out.push_back(static_cast<uint8_t>(m.algorithm));
  const uint16_t n = static_cast<uint16_t>(m.ephemeralKey.size());
  out.push_back(static_cast<uint8_t>(n));
  out.push_back(static_cast<uint8_t>(n >> 8));
  out.insert(out.end(), m.ephemeralKey.begin(), m.ephemeralKey.end());
  return out;
}

bool parseKeyExchange(const uint8_t* data, size_t len, KeyExchangeMsg& out) {
  if (len < 5 || data[0] != kHsKeyExchange) return false;
  if (data[1] > 1) return false;  // initiator is a boolean byte
  if (data[2] != static_cast<uint8_t>(KexAlgorithm::X25519)) return false;
  const uint16_t n = static_cast<uint16_t>(data[3] | (data[4] << 8));
  if (n > kKexMaxKeyBytes || 5u + n != len) return false;
  KeyExchangeMsg m;
  m.initiator = data[1] == 1;
  m.algorithm = KexAlgorithm::X25519;
  m.ephemeralKey.assign(data + 5, data + 5 + n);
  out = std::move(m);
  return true;
}

// --- CryptoSession ---------------------------------------------------------------------

CryptoSession::CryptoSession(CryptoConfig cfg) : cfg_(cfg) {}

CryptoResult CryptoSession::armCipher(std::unique_ptr<ICipher> cipher) {
  if (!cipher) return {CryptoError::BadKey, 0};
  if (cfg_.requireEncryption && cipher->isPlaintext()) {
    // Fail-closed: a plaintext backend is refused, the old cipher stays.
    return {CryptoError::NotImplemented, 0};
  }
  const bool encrypted = !cipher->isPlaintext();
  cipher_ = std::move(cipher);
  state_ = encrypted ? CryptoSessionState::Encrypted : CryptoSessionState::Plaintext;
  return {CryptoError::None, 0};
}

CryptoResult CryptoSession::deriveKeys(const KeyExchangeMsg& /*local*/,
                                       const KeyExchangeMsg& /*remote*/) {
  // STUB: the builder implements ephemeral-X25519 here (see the builder
  // notes in encryption.h) and arms the resulting cipher via armCipher().
  return {CryptoError::NotImplemented, 0};
}

bool CryptoSession::isPlaintext() const {
  return !cipher_ || cipher_->isPlaintext();
}

CryptoResult CryptoSession::sealPacket(const uint8_t* plain, size_t plainLen, uint8_t* out,
                                       size_t outCap) {
  if (isPlaintext()) return {CryptoError::NotImplemented, 0};  // never silently plaintext
  uint8_t nonce[kCryptoNonceBytes];
  nextNonce(nonce);
  return cipher_->seal(plain, plainLen, nonce, out, outCap);
}

CryptoResult CryptoSession::openPacket(const uint8_t* sealed, size_t sealedLen, uint64_t counter,
                                       uint8_t* out, size_t outCap) {
  if (isPlaintext()) return {CryptoError::NotImplemented, 0};
  // Verify-then-commit (review F-L1): the replay window is only PEEKED
  // before authentication and COMMITTED after the cipher successfully
  // opens the packet. The old accept-before-open order let a single
  // unauthenticated packet with counter = highest + 1,000,000 shift the
  // window past every legitimate packet and kill the session. (With the
  // v1 NullCipher this path returns NotImplemented above and never reaches
  // here, so today's behavior is unchanged; the ordering is pinned by a
  // test with a fake cipher for the day a real backend lands.)
  if (!replay_.peek(counter)) return {CryptoError::AuthFailed, 0};  // replay gate first
  uint8_t nonce[kCryptoNonceBytes];
  makeNonce(cfg_.sessionId, counter, nonce);
  const CryptoResult r = cipher_->open(sealed, sealedLen, nonce, out, outCap);
  if (r.error == CryptoError::None) replay_.commit(counter);
  return r;
}

}  // namespace net
}  // namespace litholattice
