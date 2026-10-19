// Connect handshake: protocol version negotiation at connect.
//
// The client and server agree on the highest mutually compatible protocol
// version over the reliable channel BEFORE any game traffic flows.
//
// NEGOTIATION RULE (locked 2026-10-11): highest mutually compatible
// protocol version wins. Three gates, checked in this order, ALL must pass:
//
//   1. Engine semver: major must match exactly (major = breaking changes:
//      save format, network protocol, SDK). Minor/patch differences within
//      a major are fine (backwards compatible by the semver contract in
//      version.h).
//   2. SDK line: the SDK major (LITHOLATTICE_SDK_VERSION, mod_interface.h)
//      must match exactly. Same-major required, NOT negotiable: a peer on
//      a different SDK line may frame/route mod traffic (kModNetTypeBase
//      and up) differently, and the mod.json SDK-range enforcement is a
//      load-time gate, not a wire gate. Cross-SDK-line downgrade is
//      refused, never silently accepted.
//   3. Protocol: highest mutually compatible version wins; same-major
//      required. Protocol versions are single integers and each one IS its
//      own major (v1/v2 have no minor on the wire), so "same major" is
//      equality — see protocolCompatible() in version.h.
//
// DOWNGRADE / SUBSET POLICY (locked): when one side offers a subset, the
// highest version in the INTERSECTION wins — a client offering {1} against
// a server offering {1,2} agrees on 1. That is a legitimate downgrade, not
// a refusal. Refusal happens only when the intersection is empty
// (NoCommonProtocol), a gate above fails, or the agreed version selects a
// codec the game has not implemented (codecForProtocol() -> Unknown, or
// V1Baseline with no legacy v1 codec plugged in): the game must then fail
// closed rather than send undecodable traffic. A server that cannot serve
// v1 should not offer v1 at all (see the deployment guidance below: the
// current offer is {3}).
//
// DEPLOYMENT GUIDANCE (updated F-L4, review 2026-10-13): the CURRENT offer
// is {3} ONLY. HandshakeConfig::supportedProtocols empty (the default)
// fills {kProtocolVersion} = {3}; no code in the tree installs
// knownProtocolVersions() {1,2,3}, and there is NO v1 codec in-tree
// (V1Baseline is a selection with no implementation). A v2-only or v1-only
// peer therefore gets Reject(NoCommonProtocol) — fail-closed. Games should
// keep their offer at the default {3}
// (HandshakeConfig::supportedProtocols = {LITHOLATTICE_PROTOCOL_V3}, or
// leave it empty). Whether to add v1/v2 interop at all is a pending product
// decision (JD, review H2) — do not widen the offer list until that lands:
// the offer list travels as unauthenticated plaintext until KEX lands, so
// an on-path attacker could rewrite a widened ClientHello list ({1,2,3} ->
// {1}) and force agreement on the codec-less v1. Real downgrade protection
// needs handshake integrity: once the builder's KEX lands, the transcript
// (hellos + negotiated version) must be signed — or the hello MACed under
// the session key — before game traffic flows (transcript-signing future,
// FLAGGED for the builder).
//
// WIRE FORMAT (little-endian, tiny on the wire):
//
//   ClientHello (msg 1):
//     u8  1                    message id (kHsClientHello)
//     u8  engineMajor
//     u8  engineMinor
//     u8  enginePatch
//     u8  sdkVersion           SDK major line (LITHOLATTICE_SDK_VERSION)
//     u8  nProto               supported protocol versions (1..kHsMaxProtoList)
//     nProto x u8  protocol version, ascending
//
//   ServerHello (msg 2):
//     u8  2                    message id (kHsServerHello)
//     u8  chosenProtocol       the agreed version (highest mutually compatible)
//     u8  engineMajor
//     u8  engineMinor
//     u8  enginePatch
//     u8  sdkVersion           server's SDK major line (must equal the client's)
//
//   Reject (msg 3):
//     u8  3                    message id (kHsReject)
//     u8  reason               RejectReason code
//     u8  serverMaxProtocol    highest protocol version the server supports
//                              (so the client can log something useful)
//
// A ClientHello is at most 6 + 32 = 38 bytes. Handshake messages travel as
// PayloadType::Handshake datagrams on the reliable channel (see channels.h):
// they must not be lost, and the reliable channel's ordering means exactly
// one hello is ever processed per side.
//
// VERSION BYTE (item: negotiated version on the wire): during the handshake
// each side stamps its own highest supported protocol version into the
// 14-byte header's version byte. The game installs knownProtocolVersions()
// as the channels' accepted set before the handshake (so a v1-stamped
// ClientHello still routes on a v2 server), then narrows the accepted set
// to the single agreed version and calls setVersion(agreed) on every
// sender afterwards. See channels.h / snapshot.h for the setters.
//
// TIMEOUTS: the state machine never hangs. pollTimeout() moves a stalled
// non-terminal state (HelloSent/HelloReceived) to TimedOut; terminal
// states (Agreed/Rejected/TimedOut) never move.
//
// ENCRYPTION SEAM: the handshake authenticates versions, not peers. After
// Agreed, the game may run the key-exchange handshake (kHsKeyExchange, see
// encryption.h) on the reliable channel before game traffic flows; until a
// real cipher is armed, traffic is deliberately plaintext — gated behind
// the NullCipher stub, never silent (see encryption.h).
//
// SPOOFING (audit MEDIUM 2026-10-11): a client in HelloSent accepts any
// well-formed Reject/ServerHello, so a spoofed 3-byte Reject kills the
// handshake (targeted connection-prevention DoS). Partial mitigation:
// ConnectHandshake::bindPeer() ties the state machine to the expected
// endpoint token and the onPayload() overload ignores payloads from any
// other endpoint — the game must derive the token from the socket's remote
// endpoint and actually pass it, or the hook does nothing. The token is an
// endpoint identity, NOT a secret: it stops off-path spoofing only. Full
// fix: the builder's authenticated KEX + socket-layer peer binding —
// FLAGGED for the builder.
#pragma once

#include <cstdint>
#include <deque>
#include <span>
#include <vector>

#include "litholattice/net/protocol.h"

namespace litholattice {
namespace net {

// Handshake sub-message ids (first payload byte of a PayloadType::Handshake
// datagram on the reliable channel).
constexpr uint8_t kHsClientHello = 1;
constexpr uint8_t kHsServerHello = 2;
constexpr uint8_t kHsReject = 3;
// kHsKeyExchange = 4 is reserved in encryption.h (post-Agreed key exchange).

/// Wire cap on the supported-protocol list (bounds memory; larger lists are
/// a malformed hello).
constexpr uint8_t kHsMaxProtoList = 32;
/// Default handshake deadline (ms, game clock). Configurable per connection.
constexpr uint64_t kHandshakeTimeoutMs = 5000;
/// Sentinel for HandshakeConfig::sdkVersion: "the game did not supply its
/// SDK line". The handshake fails closed on it (start() refuses; the server
/// rejects) — it never reaches a comparison.
constexpr uint8_t kSdkVersionUnset = 0xFF;

/// Why a handshake failed. Fits in one wire byte; the game logs it verbatim.
enum class RejectReason : uint8_t {
  None = 0,
  NoCommonProtocol = 1,       ///< no mutually compatible protocol version
  IncompatibleEngineMajor = 2,  ///< engine semver majors differ
  MalformedHello = 3,         ///< the hello did not parse (or the server's
                              ///< reply was not one this side offered, or
                              ///< this endpoint's own config is unset)
  IncompatibleSdkMajor = 4,   ///< SDK major lines differ (no cross-line
                              ///< downgrade, ever)
};

/// Result of negotiate(): the agreed version, or ok=false when the lists
/// share nothing.
struct NegotiateResult {
  bool ok = false;
  uint8_t version = 0;  ///< agreed protocol version when ok
};

/// Highest mutually compatible protocol version. Order-insensitive;
/// duplicates are harmless; either list may be empty (then ok=false).
/// Compatibility is protocolCompatible() (same-major rule) applied
/// pairwise; the maximum of the intersection wins.
NegotiateResult negotiate(std::span<const uint8_t> clientVersions,
                          std::span<const uint8_t> serverVersions);

/// The protocol versions this binary knows how to frame and parse. The game
/// installs this as the channels' accepted set BEFORE the handshake and
/// narrows it to the agreed version after (see the version-byte note above).
inline std::vector<uint8_t> knownProtocolVersions() {
  return {LITHOLATTICE_PROTOCOL_V1, LITHOLATTICE_PROTOCOL_V2, LITHOLATTICE_PROTOCOL_V3};
}

/// Highest version this binary supports (what it stamps on handshake
/// datagrams before agreement).
inline uint8_t maxProtocolVersion() { return LITHOLATTICE_PROTOCOL_V3; }

// --- message builders -------------------------------------------------------

/// Build a ClientHello payload for `protocols` (need not be sorted;
/// duplicates are removed). `sdkVersion` is this endpoint's SDK major line
/// (LITHOLATTICE_SDK_VERSION). Returns empty on programmer error (empty
/// list, or sdkVersion == kSdkVersionUnset).
std::vector<uint8_t> encodeClientHello(const EngineVersion& engine, uint8_t sdkVersion,
                                       std::span<const uint8_t> protocols);
/// Build a ServerHello payload naming the agreed version.
std::vector<uint8_t> encodeServerHello(uint8_t chosenProtocol, const EngineVersion& engine,
                                       uint8_t sdkVersion);
/// Build a Reject payload.
std::vector<uint8_t> encodeReject(RejectReason reason, uint8_t serverMaxProtocol);

// --- message parsers (false on malformed input; `out` untouched) ------------

struct ClientHelloMsg {
  EngineVersion engine;
  uint8_t sdkVersion = kSdkVersionUnset;
  std::vector<uint8_t> protocols;  ///< ascending, deduplicated
};
bool parseClientHello(const uint8_t* data, size_t len, ClientHelloMsg& out);

struct ServerHelloMsg {
  uint8_t chosenProtocol = 0;
  EngineVersion engine;
  uint8_t sdkVersion = kSdkVersionUnset;
};
bool parseServerHello(const uint8_t* data, size_t len, ServerHelloMsg& out);

struct RejectMsg {
  RejectReason reason = RejectReason::None;
  uint8_t serverMaxProtocol = 0;
};
bool parseReject(const uint8_t* data, size_t len, RejectMsg& out);

// ---------------------------------------------------------------------------
// Codec dispatch: the handshake result selects the wire codec.
// ---------------------------------------------------------------------------

/// Which snapshot codec a protocol version selects.
enum class CodecKind : uint8_t {
  Unknown = 0,
  V1Baseline = 1,  ///< v1: full PlayerState, no compression (legacy; see below)
  V2Delta = 2,     ///< v2: the delta-snapshot codec in protocol.h
  V3StableIds = 3,  ///< v3: the v2 delta-snapshot codec PLUS the 64-bit LEB128
                    ///  stable-ID wire primitives (BlockUpdate records, uvarint64).
                    ///  v3 peers negotiate v2 with older binaries (highest common).
};

/// Version -> codec dispatch. The handshake's agreed version feeds this;
/// the game switches its snapshot encode/decode path on the answer.
///
/// What v1 support means: LITHOLATTICE_PROTOCOL_V1 ("baseline: full
/// PlayerState, no compression") is the pre-delta wire format. NO v1
/// encoder/decoder exists in this tree, so V1Baseline is a SELECTION, not
/// an implementation: agreeing on v1 means the game must plug a legacy v1
/// codec itself (or refuse v1 peers / narrow its offer list to {2}).
/// v2 selects the in-tree delta codec. Unknown versions map to
/// CodecKind::Unknown: the game must not send game traffic on them.
constexpr CodecKind codecForProtocol(uint8_t protocolVersion) {
  switch (protocolVersion) {
    case LITHOLATTICE_PROTOCOL_V1:
      return CodecKind::V1Baseline;
    case LITHOLATTICE_PROTOCOL_V2:
      return CodecKind::V2Delta;
    case LITHOLATTICE_PROTOCOL_V3:
      return CodecKind::V3StableIds;
    default:
      return CodecKind::Unknown;
  }
}

// ---------------------------------------------------------------------------
// ConnectHandshake: the state machine.
// ---------------------------------------------------------------------------

enum class HandshakeRole : uint8_t { Client, Server };

enum class HandshakeState : uint8_t {
  Idle,           ///< nothing started (client) / no hello yet (server)
  HelloSent,      ///< client: hello is out, waiting for ServerHello/Reject
  HelloReceived,  ///< server: hello parsed, reply queued (transient)
  Agreed,         ///< versions agreed; agreedProtocol() is valid
  Rejected,       ///< failed; rejectReason() is valid
  TimedOut,       ///< deadline passed with no progress; a clean failure
};

struct HandshakeConfig {
  HandshakeRole role = HandshakeRole::Client;
  std::vector<uint8_t> supportedProtocols;  ///< empty = {kProtocolVersion}
  EngineVersion engine;                     ///< this endpoint's engine semver
  /// This endpoint's SDK major line (LITHOLATTICE_SDK_VERSION from
  /// mod_interface.h). Defaults to kSdkVersionUnset, which fails closed:
  /// the game MUST set this. Same-line required (see the negotiation rule).
  uint8_t sdkVersion = kSdkVersionUnset;
  uint64_t timeoutMs = kHandshakeTimeoutMs;
};

/// One side of the connect handshake, run over the reliable channel. The
/// class is transport-agnostic: it produces and consumes handshake PAYLOADS
/// (std::vector<uint8_t>); the game frames them with
/// ReliableChannel::send(..., PayloadType::Handshake, ...) and feeds
/// pollDelivery() output back in via onPayload().
///
///   Client: start() -> send hello -> onPayload(ServerHello) => Agreed.
///   Server: onPayload(ClientHello) -> pending() yields ServerHello/Reject.
///
/// Retransmission is the reliable channel's job (RTO); this class only
/// needs the deadline. A fresh ConnectHandshake is created per connection
/// attempt (like ReliableChannel, it is a fresh pairing).
class ConnectHandshake {
 public:
  explicit ConnectHandshake(HandshakeConfig cfg);

  HandshakeRole role() const { return cfg_.role; }
  HandshakeState state() const { return state_; }
  const HandshakeConfig& config() const { return cfg_; }

  /// Client role: build the ClientHello payload, enter HelloSent. False in
  /// any state but Idle, for the server role, or when sdkVersion is unset
  /// (fail-closed: the game must supply its SDK line).
  bool start(uint64_t nowMs, std::vector<uint8_t>& helloOut);

  /// Feed one handshake payload from the reliable channel (in order; the
  /// reliable channel guarantees exactly-once in-order delivery). Server
  /// replies are queued for pending(). `nowMs` refreshes the activity
  /// clock, but ONLY for recognized handshake messages (audit LOW
  /// 2026-10-11): junk payloads are ignored and must not hold HelloSent
  /// past the deadline. Ignored in terminal states (Agreed/Rejected/TimedOut).
  void onPayload(const uint8_t* data, size_t len, uint64_t nowMs);
  void onPayload(const std::vector<uint8_t>& payload, uint64_t nowMs) {
    onPayload(payload.data(), payload.size(), nowMs);
  }
  /// onPayload with the sender's endpoint token (see bindPeer). When a peer
  /// is bound and the token does not match, the payload is ignored entirely:
  /// never parsed, never refreshing liveness (partial spoofed-Reject/DoS
  /// mitigation — the game must supply the token for it to have any effect).
  void onPayload(const uint8_t* data, size_t len, uint64_t nowMs, uint64_t fromPeerToken) {
    if (peerBound_ && fromPeerToken != peerToken_) return;  // unexpected endpoint: ignore
    onPayload(data, len, nowMs);
  }
  void onPayload(const std::vector<uint8_t>& payload, uint64_t nowMs, uint64_t fromPeerToken) {
    onPayload(payload.data(), payload.size(), nowMs, fromPeerToken);
  }

  /// Bind this handshake to the expected peer (partial mitigation for the
  /// spoofed-Reject/ServerHello DoS, audit MEDIUM 2026-10-11). The token is
  /// opaque to the net layer: the game derives it from the socket's remote
  /// endpoint (e.g. a hash of ip+port) once the socket is bound to the peer,
  /// and passes the sender's token to the onPayload() overload above.
  /// Unbound = historical behavior (every payload is processed). The token
  /// is an endpoint identity, NOT a secret: it stops off-path spoofing
  /// only. Full fix — authenticated KEX + socket-layer peer binding — is
  /// FLAGGED for the builder (see the SPOOFING note at the top of this file).
  void bindPeer(uint64_t peerToken) {
    peerToken_ = peerToken;
    peerBound_ = true;
  }
  bool peerBound() const { return peerBound_; }

  /// Pop the next queued reply (server role: ServerHello or Reject). False
  /// when the queue is empty.
  bool pending(std::vector<uint8_t>& out);

  /// Deadline check: HelloSent/HelloReceived idle past timeoutMs becomes
  /// TimedOut (a clean failure the game can report and retry). Terminal
  /// states never move. Returns the (possibly new) state.
  HandshakeState pollTimeout(uint64_t nowMs);

  // --- results ---
  uint8_t agreedProtocol() const { return agreed_; }  ///< valid in Agreed
  RejectReason rejectReason() const { return reject_; }  ///< valid in Rejected
  /// The peer's engine semver. Valid once a hello was parsed
  /// (peerHelloParsed()).
  const EngineVersion& peerEngine() const { return peerEngine_; }
  /// The peer's SDK major line. Valid once a hello was parsed.
  uint8_t peerSdkVersion() const { return peerSdk_; }
  bool peerHelloParsed() const { return peerHelloParsed_; }

 private:
  HandshakeConfig cfg_;
  HandshakeState state_ = HandshakeState::Idle;
  uint64_t lastActivityMs_ = 0;
  uint8_t agreed_ = 0;
  RejectReason reject_ = RejectReason::None;
  EngineVersion peerEngine_;
  uint8_t peerSdk_ = kSdkVersionUnset;
  bool peerHelloParsed_ = false;
  std::deque<std::vector<uint8_t>> pending_;
  uint64_t peerToken_ = 0;  // expected endpoint token (see bindPeer)
  bool peerBound_ = false;  // true once bindPeer() tied us to an endpoint

  bool offered(uint8_t v) const;
  void fail(RejectReason r);
  void queueReject(RejectReason r);
};

}  // namespace net
}  // namespace litholattice
