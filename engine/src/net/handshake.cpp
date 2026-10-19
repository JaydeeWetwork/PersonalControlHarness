#include "litholattice/net/handshake.h"

#include <algorithm>

namespace litholattice {
namespace net {

NegotiateResult negotiate(std::span<const uint8_t> clientVersions,
                         std::span<const uint8_t> serverVersions) {
  NegotiateResult r;
  for (uint8_t c : clientVersions)
    for (uint8_t s : serverVersions)
      if (protocolCompatible(c, s) && (!r.ok || c > r.version)) {
        r.ok = true;
        r.version = c;  // compatible => equal; c is the common version
      }
  return r;
}

std::vector<uint8_t> encodeClientHello(const EngineVersion& engine, uint8_t sdkVersion,
                                       std::span<const uint8_t> protocols) {
  if (sdkVersion == kSdkVersionUnset) return {};  // fail-closed: SDK line required
  // Ascending, deduplicated offer list.
  std::vector<uint8_t> sorted(protocols.begin(), protocols.end());
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  if (sorted.empty() || sorted.size() > kHsMaxProtoList) return {};
  std::vector<uint8_t> out;
  out.reserve(6 + sorted.size());
  out.push_back(kHsClientHello);
  out.push_back(engine.major);
  out.push_back(engine.minor);
  out.push_back(engine.patch);
  out.push_back(sdkVersion);
  out.push_back(static_cast<uint8_t>(sorted.size()));
  out.insert(out.end(), sorted.begin(), sorted.end());
  return out;
}

std::vector<uint8_t> encodeServerHello(uint8_t chosenProtocol, const EngineVersion& engine,
                                       uint8_t sdkVersion) {
  return {kHsServerHello, chosenProtocol, engine.major, engine.minor, engine.patch, sdkVersion};
}

std::vector<uint8_t> encodeReject(RejectReason reason, uint8_t serverMaxProtocol) {
  return {kHsReject, static_cast<uint8_t>(reason), serverMaxProtocol};
}

bool parseClientHello(const uint8_t* data, size_t len, ClientHelloMsg& out) {
  if (len < 6 || data[0] != kHsClientHello) return false;
  const uint8_t n = data[5];
  if (n == 0 || n > kHsMaxProtoList || 6u + n != len) return false;
  ClientHelloMsg m;
  m.engine = EngineVersion{data[1], data[2], data[3]};
  m.sdkVersion = data[4];
  m.protocols.assign(data + 6, data + 6 + n);
  std::sort(m.protocols.begin(), m.protocols.end());
  m.protocols.erase(std::unique(m.protocols.begin(), m.protocols.end()), m.protocols.end());
  out = std::move(m);
  return true;
}

bool parseServerHello(const uint8_t* data, size_t len, ServerHelloMsg& out) {
  if (len != 6 || data[0] != kHsServerHello) return false;
  ServerHelloMsg m;
  m.chosenProtocol = data[1];
  m.engine = EngineVersion{data[2], data[3], data[4]};
  m.sdkVersion = data[5];
  out = m;
  return true;
}

bool parseReject(const uint8_t* data, size_t len, RejectMsg& out) {
  if (len != 3 || data[0] != kHsReject) return false;
  RejectMsg m;
  m.reason = static_cast<RejectReason>(data[1]);
  if (m.reason == RejectReason::None) return false;  // a reject must say why
  m.serverMaxProtocol = data[2];
  out = m;
  return true;
}

// --- ConnectHandshake -------------------------------------------------------

ConnectHandshake::ConnectHandshake(HandshakeConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.supportedProtocols.empty()) cfg_.supportedProtocols = {kProtocolVersion};
  std::sort(cfg_.supportedProtocols.begin(), cfg_.supportedProtocols.end());
  cfg_.supportedProtocols.erase(
      std::unique(cfg_.supportedProtocols.begin(), cfg_.supportedProtocols.end()),
      cfg_.supportedProtocols.end());
}

bool ConnectHandshake::start(uint64_t nowMs, std::vector<uint8_t>& helloOut) {
  if (cfg_.role != HandshakeRole::Client || state_ != HandshakeState::Idle) return false;
  if (cfg_.sdkVersion == kSdkVersionUnset) return false;  // fail-closed: SDK line required
  helloOut = encodeClientHello(cfg_.engine, cfg_.sdkVersion, cfg_.supportedProtocols);
  if (helloOut.empty()) return false;  // cannot happen (ctor guarantees a list)
  lastActivityMs_ = nowMs;
  state_ = HandshakeState::HelloSent;
  return true;
}

void ConnectHandshake::onPayload(const uint8_t* data, size_t len, uint64_t nowMs) {
  if (state_ == HandshakeState::Agreed || state_ == HandshakeState::Rejected ||
      state_ == HandshakeState::TimedOut)
    return;  // terminal: late traffic is ignored
  if (len == 0) return;

  if (cfg_.role == HandshakeRole::Client) {
    if (state_ != HandshakeState::HelloSent) return;  // not expecting anything
    // Liveness refreshes only on a RECOGNIZED handshake message (audit LOW
    // 2026-10-11): spoofed junk drips must not hold HelloSent past the
    // deadline, or pollTimeout() never fires.
    const bool recognized =
        len > 0 && (data[0] == kHsServerHello || data[0] == kHsReject);
    if (!recognized) return;  // junk: ignored, and the deadline still fires
    lastActivityMs_ = nowMs;
    if (data[0] == kHsServerHello) {
      ServerHelloMsg m;
      if (!parseServerHello(data, len, m) || !offered(m.chosenProtocol)) {
        fail(RejectReason::MalformedHello);  // server answered with a version
        return;                              // we never offered: not trustworthy
      }
      if (!engineCompatible(m.engine, cfg_.engine)) {
        fail(RejectReason::IncompatibleEngineMajor);
        return;
      }
      if (!sdkCompatible(m.sdkVersion, cfg_.sdkVersion)) {
        fail(RejectReason::IncompatibleSdkMajor);  // no cross-SDK-line downgrade
        return;
      }
      peerEngine_ = m.engine;
      peerSdk_ = m.sdkVersion;
      peerHelloParsed_ = true;
      agreed_ = m.chosenProtocol;
      state_ = HandshakeState::Agreed;
    } else if (data[0] == kHsReject) {
      RejectMsg m;
      if (!parseReject(data, len, m)) {
        fail(RejectReason::MalformedHello);
        return;
      }
      fail(m.reason);
    }
    // Anything else is ignored: stay in HelloSent until the deadline.
    return;
  }

  // Server role: exactly one hello is processed; anything after the first
  // is ignored (the reliable channel already dedups and orders).
  if (state_ != HandshakeState::Idle) return;
  // Liveness refreshes only on a recognized ClientHello (audit LOW
  // 2026-10-11): junk must not extend the deadline. (Every recognized path
  // below is terminal — Agreed or fail() — so the refresh only matters for
  // the malformed-but-recognized case, which is harmless either way.)
  if (len == 0 || data[0] != kHsClientHello) return;
  lastActivityMs_ = nowMs;
  if (cfg_.sdkVersion == kSdkVersionUnset) {
    // Our own config is unset: this is a game bug, not a peer fault, but
    // the fail-closed answer is still a reject (logged by the game).
    queueReject(RejectReason::MalformedHello);
    fail(RejectReason::MalformedHello);
    return;
  }
  ClientHelloMsg hello;
  if (!parseClientHello(data, len, hello)) {
    queueReject(RejectReason::MalformedHello);
    fail(RejectReason::MalformedHello);
    return;
  }
  peerEngine_ = hello.engine;
  peerSdk_ = hello.sdkVersion;
  peerHelloParsed_ = true;
  state_ = HandshakeState::HelloReceived;
  if (!engineCompatible(hello.engine, cfg_.engine)) {
    queueReject(RejectReason::IncompatibleEngineMajor);
    fail(RejectReason::IncompatibleEngineMajor);
    return;
  }
  if (!sdkCompatible(hello.sdkVersion, cfg_.sdkVersion)) {
    queueReject(RejectReason::IncompatibleSdkMajor);  // no cross-SDK-line downgrade
    fail(RejectReason::IncompatibleSdkMajor);
    return;
  }
  const NegotiateResult r = negotiate(hello.protocols, cfg_.supportedProtocols);
  if (!r.ok) {
    queueReject(RejectReason::NoCommonProtocol);
    fail(RejectReason::NoCommonProtocol);
    return;
  }
  agreed_ = r.version;
  pending_.push_back(encodeServerHello(agreed_, cfg_.engine, cfg_.sdkVersion));
  state_ = HandshakeState::Agreed;
}

bool ConnectHandshake::pending(std::vector<uint8_t>& out) {
  if (pending_.empty()) return false;
  out = std::move(pending_.front());
  pending_.pop_front();
  return true;
}

HandshakeState ConnectHandshake::pollTimeout(uint64_t nowMs) {
  if ((state_ == HandshakeState::HelloSent || state_ == HandshakeState::HelloReceived) &&
      cfg_.timeoutMs > 0 && nowMs >= lastActivityMs_ &&
      nowMs - lastActivityMs_ >= cfg_.timeoutMs) {
    state_ = HandshakeState::TimedOut;
    reject_ = RejectReason::None;  // no peer fault: the peer simply never answered
  }
  return state_;
}

bool ConnectHandshake::offered(uint8_t v) const {
  return std::find(cfg_.supportedProtocols.begin(), cfg_.supportedProtocols.end(), v) !=
         cfg_.supportedProtocols.end();
}

void ConnectHandshake::fail(RejectReason r) {
  reject_ = r;
  state_ = HandshakeState::Rejected;
}

void ConnectHandshake::queueReject(RejectReason r) {
  const uint8_t maxProto =
      cfg_.supportedProtocols.empty() ? 0 : cfg_.supportedProtocols.back();
  pending_.push_back(encodeReject(r, maxProto));
}

}  // namespace net
}  // namespace litholattice
