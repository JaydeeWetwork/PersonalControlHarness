// Packet encryption: stubs + builder notes (v1 = deliberate plaintext).
//
// STATUS: no real cryptography is implemented in this tree. What exists:
//   - the seam (ICipher, CryptoSession, key-exchange message shape),
//   - the replay-protection window (real logic: it is bookkeeping, not crypto),
//   - the nonce construction (pure byte packing),
//   - a NullCipher that REFUSES to seal/open and reports NotImplemented —
//     traffic stays plaintext, but never silently: isPlaintext() is true
//     and the game must surface it (log once per session; requireEncryption
//     turns it into a hard failure).
//
// OVERHEAD ANALYSIS (locked inputs: small frequent packets, 512 TPS sim,
// 32 Hz snapshots; see version.h):
//
//   The AEAD shortlist is ChaCha20-Poly1305 vs AES-256-GCM. Both cost a
//   16-byte auth tag per packet. The nonce is 12 bytes but is IMPLICIT here
//   (derived from the session id + per-direction packet counter, never
//   transmitted), so the wire overhead is 16 B/pkt, inside the
//   ~28 B/pkt budget (kCryptoMaxOverheadBytes). At 32 Hz snapshots that is
//   16*32 = 512 B/s per peer of crypto overhead — noise next to the
//   ~14 KB/s of UDP/IP headers version.h already budgets for.
//
//   ChaCha20-Poly1305 wins for this engine:
//     - No hardware dependency. Constant-time in portable software; fast on
//       ARM without crypto extensions (the primary dev machine is a
//       mid-range phone) and on x86 without AES-NI. A ~200-byte snapshot at
//       ~3-4 cycles/byte is under a microsecond — nothing at 32 Hz.
//     - AES-GCM is faster ONLY with AES-NI / ARMv8-CE (~0.6 c/B); without
//       it, software AES is slow and notoriously hard to keep constant-time.
//       GCM also has the fragile-nonce problem (a repeated nonce destroys
//       confidentiality); ChaCha20-Poly1305 degrades more gracefully and
//       our implicit counter-nonce makes reuse structurally unlikely.
//     - The sim is deterministic and the threat model is cheaters and
//       snoopers on LAN/public servers, not nation-states: ChaCha20's
//       simplicity is a feature (smaller audit surface).
//
// BUILDER NOTES (what to vendor, where it plugs in):
//
//   1. LIBRARY: vendor libsodium (first choice) or monocypher (lean
//      alternative). libsodium gives X25519 (key exchange) +
//      ChaCha20-Poly1305 (AEAD) + Ed25519 (reserved for the future
//      cryptographically-signed-mods plan) behind one stable, misuse-
//      resistant API; it is the boring, audited default. monocypher is
//      public-domain, ~3k lines, trivially auditable, and enough if binary
//      size matters — but then the builder owns the KEX/Ed25519 glue.
//      BoringSSL is overkill (TLS stack weight for a datagram protocol).
//      Whichever is chosen, it lives behind ICipher: only NullCipher is
//      replaced, nothing else in net/ changes.
//   2. PLUG-IN POINT: after ConnectHandshake reaches Agreed and BEFORE the
//      game narrows the accepted version set / starts game traffic. The
//      initiator sends KeyExchange (kHsKeyExchange) on the reliable channel
//      as a PayloadType::Handshake datagram; the responder answers with its
//      own KeyExchange; both sides run deriveKeys() (ephemeral X25519 —
//      forward secrecy per session), armCipher(), and flip to
//      SessionState::Encrypted. Reuse kHandshakeTimeoutMs for the KEX
//      round-trip; a KEX timeout is a clean handshake failure, not a hang.
//   3. SEAL/OPEN PLACEMENT: ChannelMux seals the payload AFTER framing the
//      14-byte header (header stays plaintext: seq/ack drive the reliability
//      machinery), ChannelDemux opens BEFORE dispatching to the payload
//      handlers. Auth failure (CryptoError::AuthFailed) drops the datagram
//      and logs — fail-closed, never deliver tampered bytes.
//   4. REPLAY PROTECTION (sketch, implemented below): nonce = sessionId ||
//      counter, 12 bytes LE (makeNonce). Each direction keeps its own
//      u64 counter, starting at 0 per session; the 12-byte nonce is never
//      transmitted. The receiver rebuilds the expected nonce from the
//      claimed counter and PEEKS at the ReplayWindow BEFORE opening:
//      duplicates and >64-behind packets are dropped without touching the
//      cipher, and the window is COMMITTED only after the cipher verifies
//      the packet (review F-L1: an accept-before-auth window lets one
//      forged far-future counter poison the session). Counter space is
//      u64: at 512 packets/s it lasts ~10^12 years.
//   5. KEY LIFETIME: one ephemeral keypair per session, zeroed on close
//      (the backend owns the zeroing; NullCipher holds no key material).
//      Rekeying is a future item — the session id in the nonce already
//      partitions the nonce space per session, so rekey = new session.
//   6. PLAINTEXT v1 IS DELIBERATE: NullCipher::isPlaintext() is the gate
//      the game checks. Ship v1 plaintext ONLY with the session log line
//      and (for server browsers) an "unencrypted" badge; flip
//      CryptoConfig::requireEncryption the moment a backend lands.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace litholattice {
namespace net {

// --- wire constants ----------------------------------------------------------

/// 256-bit symmetric key (ChaCha20-Poly1305 / AES-256-GCM).
constexpr size_t kCryptoKeyBytes = 32;
/// 96-bit nonce. Layout: [0..4) u32 sessionId, [4..12) u64 packet counter,
/// little-endian (see makeNonce). IMPLICIT on the wire: derived, never sent.
constexpr size_t kCryptoNonceBytes = 12;
/// AEAD authentication tag (Poly1305 / GHASH). The only crypto bytes that
/// travel on the wire per packet.
constexpr size_t kCryptoTagBytes = 16;
/// Wire overhead per encrypted packet with the implicit nonce: tag only.
constexpr size_t kCryptoWireOverheadBytes = kCryptoTagBytes;
/// Never-exceed overhead budget per packet (nonce 12 + tag 16), for MTU
/// math (kMaxUdpPayload) and the builder's backend choice.
constexpr size_t kCryptoMaxOverheadBytes = 28;

/// Handshake sub-message id for the post-Agreed key exchange. Extends the
/// kHsClientHello/kHsServerHello/kHsReject space in handshake.h (1..3);
/// travels the same way (PayloadType::Handshake on the reliable channel).
constexpr uint8_t kHsKeyExchange = 4;
/// Wire cap on the ephemeral public key (X25519 pubkeys are 32 bytes; the
/// cap bounds memory against a malicious peer).
constexpr uint16_t kKexMaxKeyBytes = 128;

// --- errors -------------------------------------------------------------------

enum class CryptoError : uint8_t {
  None = 0,
  NotImplemented = 1,  ///< stub: no crypto backend linked yet
  BadKey = 2,          ///< key wrong length / not armed
  BadNonce = 3,        ///< nonce wrong length
  AuthFailed = 4,      ///< tag mismatch: drop the datagram, log, fail closed
  BadLength = 5,       ///< output buffer too small / input too short
};

/// Result of a seal/open/derive call. `bytes` is the output length when
/// error == None. Buffers are untouched unless error == None.
struct CryptoResult {
  CryptoError error = CryptoError::None;
  size_t bytes = 0;
};

// --- the seam ------------------------------------------------------------------

/// AEAD cipher backend. The builder implements this against the vendored
/// library (libsodium/monocypher); net/ never sees the library directly.
///
/// Contract: seal() appends a kCryptoTagBytes tag (out must hold
/// plainLen + kCryptoTagBytes); open() verifies the tag and strips it
/// (sealedLen must be >= kCryptoTagBytes). Failures return an error and
/// leave buffers untouched — NEVER emit unauthenticated plaintext.
class ICipher {
 public:
  virtual ~ICipher() = default;
  virtual CryptoResult seal(const uint8_t* plain, size_t plainLen,
                            const uint8_t nonce[kCryptoNonceBytes], uint8_t* out,
                            size_t outCap) = 0;
  virtual CryptoResult open(const uint8_t* sealed, size_t sealedLen,
                            const uint8_t nonce[kCryptoNonceBytes], uint8_t* out,
                            size_t outCap) = 0;
  /// True while no real cipher is armed. The v1 deliberate-plaintext gate.
  virtual bool isPlaintext() const = 0;
  virtual const char* name() const = 0;
};

/// The v1 stub: seal()/open() return NotImplemented and touch nothing.
/// isPlaintext() is true so the game can (must) surface the state.
/// This is the deliberate-plaintext gate — plaintext is never silent.
class NullCipher final : public ICipher {
 public:
  CryptoResult seal(const uint8_t* plain, size_t plainLen,
                    const uint8_t nonce[kCryptoNonceBytes], uint8_t* out,
                    size_t outCap) override;
  CryptoResult open(const uint8_t* sealed, size_t sealedLen,
                    const uint8_t nonce[kCryptoNonceBytes], uint8_t* out,
                    size_t outCap) override;
  bool isPlaintext() const override { return true; }
  const char* name() const override { return "null (deliberate plaintext v1)"; }
};

// --- nonce ----------------------------------------------------------------------

/// Build the 12-byte implicit nonce: [0..4) u32 sessionId, [4..12) u64
/// counter, little-endian. Pure byte packing (no crypto); both sides derive
/// it identically, so it is never transmitted.
inline void makeNonce(uint32_t sessionId, uint64_t counter,
                      uint8_t nonce[kCryptoNonceBytes]) {
  nonce[0] = static_cast<uint8_t>(sessionId);
  nonce[1] = static_cast<uint8_t>(sessionId >> 8);
  nonce[2] = static_cast<uint8_t>(sessionId >> 16);
  nonce[3] = static_cast<uint8_t>(sessionId >> 24);
  for (int i = 0; i < 8; ++i) nonce[4 + i] = static_cast<uint8_t>(counter >> (8 * i));
}

// --- replay protection ------------------------------------------------------------

/// Sliding-window replay filter over the u64 per-direction packet counter.
/// Real logic (bookkeeping, not crypto): accept() is the receive-side gate
/// and runs BEFORE open(), so replays never reach the cipher.
///
/// Verify-then-commit split (review F-L1): peek() answers "would accept()
/// take this counter?" WITHOUT mutating the window, and commit() advances
/// the window for a counter peek() approved. CryptoSession::openPacket
/// peeks, runs the cipher, and commits ONLY on successful authentication —
/// the old accept-before-open order let one forged packet with a far-future
/// counter shift the window and drop every legitimate packet behind it.
/// accept() (= peek + commit) remains for callers with no auth step.
class ReplayWindow {
 public:
  /// True if `counter` is fresh. Advances the window on accept; duplicates
  /// and counters more than 63 behind the highest are rejected.
  bool accept(uint64_t counter);
  /// True if accept(counter) would succeed right now. No mutation.
  bool peek(uint64_t counter) const;
  /// Advance the window for `counter`. Precondition: peek(counter) was
  /// true and nothing else mutated the window since (single-threaded
  /// receive path); re-checks defensively and ignores a stale commit.
  void commit(uint64_t counter);
  uint64_t highest() const { return highest_; }

 private:
  bool started_ = false;
  uint64_t highest_ = 0;
  uint64_t bitmap_ = 0;  ///< bit i = seen(highest - i); bit 0 = highest itself
};

// --- key exchange (message shape only) --------------------------------------------

/// KEX algorithm selector for the KeyExchange message. Only X25519 is
/// specified; the enum leaves room for the builder.
enum class KexAlgorithm : uint8_t {
  Unknown = 0,
  X25519 = 1,  ///< recommended: ephemeral X25519, forward-secret per session
};

/// KeyExchange (msg 4) wire format (little-endian):
///   u8  4                    kHsKeyExchange
///   u8  initiator            1 = initiator (client), 0 = responder (server)
///   u8  algorithm            KexAlgorithm
///   u16 ephKeyLen            ephemeral public key length (size-first, per the
///                            versioned-struct rule; <= kKexMaxKeyBytes)
///   ephKeyLen x u8           ephemeral public key bytes
///
/// The message SHAPE is implemented (encode/parse); the key math is not —
/// CryptoSession::deriveKeys() returns NotImplemented until the backend
/// lands. Runs on the reliable channel after ConnectHandshake::Agreed.
struct KeyExchangeMsg {
  bool initiator = false;
  KexAlgorithm algorithm = KexAlgorithm::Unknown;
  std::vector<uint8_t> ephemeralKey;
};
std::vector<uint8_t> encodeKeyExchange(const KeyExchangeMsg& m);
/// False on malformed input (bad id, bad initiator byte, unknown algorithm,
/// ephKeyLen over the cap or not matching the length); `out` untouched.
bool parseKeyExchange(const uint8_t* data, size_t len, KeyExchangeMsg& out);

// --- session -----------------------------------------------------------------------

enum class CryptoSessionState : uint8_t {
  Plaintext,    ///< no cipher armed (v1 state: NullCipher or nothing)
  KeyExchange,  ///< KEX in flight (post-handshake; builder-driven)
  Encrypted,    ///< cipher armed; seal/open active
};

struct CryptoConfig {
  /// Fail-closed: when true, arming a plaintext cipher (or sending while
  /// plaintext) fails instead of proceeding. Flip this the day a backend
  /// lands. Default false = the deliberate v1 plaintext state.
  bool requireEncryption = false;
  /// Random per session (game supplies, e.g. from the OS CSPRNG). Feeds
  /// the implicit nonce (makeNonce) and partitions the nonce space.
  uint32_t sessionId = 0;
};

/// Owns the cipher backend and the receive-side replay window for one
/// connection. The game drives it: armCipher() after the KEX (or with a
/// NullCipher for v1), then seal()/open() around ChannelMux/ChannelDemux.
class CryptoSession {
 public:
  explicit CryptoSession(CryptoConfig cfg);

  CryptoSessionState state() const { return state_; }
  const CryptoConfig& config() const { return cfg_; }

  /// Install the cipher backend (takes ownership). With requireEncryption,
  /// arming a plaintext cipher fails (NotImplemented) and the old cipher
  /// (if any) is kept. Success moves the session to Encrypted.
  CryptoResult armCipher(std::unique_ptr<ICipher> cipher);

  /// Derive session keys from a completed key exchange. STUB: returns
  /// NotImplemented until the backend lands. The builder implements
  /// ephemeral-X25519 here and arms the resulting cipher via armCipher().
  CryptoResult deriveKeys(const KeyExchangeMsg& local, const KeyExchangeMsg& remote);

  /// True while traffic is (deliberately) plaintext: no cipher armed, or
  /// the armed cipher reports isPlaintext(). The game gates on this.
  bool isPlaintext() const;

  // --- packet path ---
  /// Seal one outbound payload. Plaintext state + requireEncryption =>
  /// NotImplemented (fail-closed). Plaintext state without the flag =>
  /// NotImplemented too: NullCipher never copies plaintext through; the
  /// game sends the raw payload itself while isPlaintext() is true.
  CryptoResult sealPacket(const uint8_t* plain, size_t plainLen, uint8_t* out,
                          size_t outCap);
  /// Open one inbound packet carrying the claimed `counter`. Peeks at the
  /// replay gate BEFORE the cipher (replays/too-old counters return
  /// AuthFailed without touching the backend) and commits the window only
  /// after successful authentication (review F-L1).
  CryptoResult openPacket(const uint8_t* sealed, size_t sealedLen, uint64_t counter,
                          uint8_t* out, size_t outCap);

  /// Next outbound nonce (sessionId || sendCounter++). Only meaningful once
  /// encrypted; harmless otherwise.
  void nextNonce(uint8_t nonce[kCryptoNonceBytes]) { makeNonce(cfg_.sessionId, sendCounter_++, nonce); }
  uint64_t sendCounter() const { return sendCounter_; }
  const ReplayWindow& replayWindow() const { return replay_; }

 private:
  CryptoConfig cfg_;
  CryptoSessionState state_ = CryptoSessionState::Plaintext;
  std::unique_ptr<ICipher> cipher_;
  uint64_t sendCounter_ = 0;
  ReplayWindow replay_;
};

}  // namespace net
}  // namespace litholattice
