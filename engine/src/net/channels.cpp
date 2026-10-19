#include "litholattice/net/channels.h"

namespace litholattice {
namespace net {

namespace {
// Decode a header accepting any version in `accepted`. On success fills `h`
// and leaves `in` positioned past the header. `reject` (optional) reports
// the mismatch when every candidate failed on the version byte (a clear,
// reportable rejection rather than a silent drop).
bool decodeAcceptedHeader(const uint8_t* data, size_t len,
                          const std::vector<uint8_t>& accepted, PacketHeader& h,
                          quantize::ByteReader& in, HeaderReject* reject) {
  HeaderReject last;
  bool sawVersionMismatch = false;
  for (uint8_t v : accepted) {
    quantize::ByteReader r(data, len);
    HeaderReject rej;
    if (decodeHeader(r, h, v, &rej)) {
      in = r;
      if (reject) *reject = HeaderReject{};
      return true;
    }
    if (!rej.versionMismatch) {
      if (reject) *reject = rej;  // bad magic/truncated: no version fixes that
      return false;
    }
    sawVersionMismatch = true;
    last = rej;
  }
  if (reject && sawVersionMismatch) *reject = last;
  return false;
}
}  // namespace

// --- ReliableChannel ---------------------------------------------------------

bool ReliableChannel::decodeAccepted(const uint8_t* data, size_t len, PacketHeader& h,
                                     quantize::ByteReader& in) {
  return decodeAcceptedHeader(data, len, accepted_, h, in, &lastReject_);
}

bool ReliableChannel::heartbeat(uint64_t /*nowMs*/, std::vector<uint8_t>& datagram) {
  // Ack-only: not sequenced, not queued, not retransmitted. Seq 0 can never
  // be a payload seq (send() skips it), so the receiver treats it as pure
  // ack flow.
  PacketHeader h;
  h.version = version_;  // negotiated version once the handshake agrees
  h.flags = kFlagReliable;
  h.seq = 0;
  h.ack = ackSeq();
  h.ackBits = ackBits();
  h.type = PayloadType::Ack;
  h.payloadLen = 0;
  quantize::ByteWriter w;
  encodeHeader(h, w);
  datagram = w.buf;
  return true;
}

bool ReliableChannel::send(const uint8_t* payload, size_t len, PayloadType type,
                           uint64_t nowMs, std::vector<uint8_t>& datagram) {
  if (len > cfg_.maxPayloadBytes) return false;
  if (inflight_.size() >= cfg_.maxInFlight) return false;  // backpressure
  PacketHeader h;
  h.version = version_;  // negotiated version once the handshake agrees
  h.flags = kFlagReliable;
  h.seq = nextSeq_++;
  if (nextSeq_ == 0) nextSeq_ = 1;  // seq 0 marks ack-only heartbeats
  h.ack = ackSeq();
  h.ackBits = ackBits();
  h.type = type;
  h.payloadLen = static_cast<uint16_t>(len);

  quantize::ByteWriter w;
  encodeHeader(h, w);
  if (len) w.putBytes(payload, len);
  datagram = w.buf;
  inflight_.emplace(h.seq, InflightPacket{h.seq, datagram, nowMs});
  return true;
}

std::vector<std::vector<uint8_t>> ReliableChannel::poll(uint64_t nowMs) {
  std::vector<std::vector<uint8_t>> out;
  for (auto& kv : inflight_) {
    InflightPacket& p = kv.second;
    if (nowMs - p.lastSentMs >= cfg_.rtoMs) {
      p.lastSentMs = nowMs;
      // Same seq (the receiver dedups), but FRESH ack/ackBits: retransmits
      // keep ack state flowing even when no new datagrams are going out.
      std::vector<uint8_t> d = p.datagram;
      restampAcks(d);
      out.push_back(std::move(d));
    }
  }
  return out;
}

void ReliableChannel::restampAcks(std::vector<uint8_t>& datagram) const {
  // Header layout: ack at offset 6, ackBits at offset 8 (little-endian).
  const uint16_t ack = ackSeq(), bits = ackBits();
  datagram[6] = static_cast<uint8_t>(ack & 0xFF);
  datagram[7] = static_cast<uint8_t>(ack >> 8);
  datagram[8] = static_cast<uint8_t>(bits & 0xFF);
  datagram[9] = static_cast<uint8_t>(bits >> 8);
}

void ReliableChannel::onAck(uint16_t ack, uint16_t ackBits) {
  if (ack == 0 && ackBits == 0) return;  // nothing acked yet
  // RFC-1982 "at-or-before", valid inside the 2^15 window (the in-flight
  // window is <= 64 deep, so this is unambiguous in practice).
  auto atOrBefore = [](uint16_t s, uint16_t t) {
    return static_cast<uint16_t>(t - s) < 0x8000;
  };
  auto covered = [&](uint16_t s) {
    if (atOrBefore(s, ack)) return true;  // cumulative: everything through ack
    for (int i = 0; i < 16; ++i)          // selective: the 16 above ack
      if (ackBits & (1u << i)) {
        if (s == static_cast<uint16_t>(ack + 1 + i)) return true;
      }
    return false;
  };
  // Evict every covered packet. A stale ack simply covers nothing new.
  for (auto it = inflight_.begin(); it != inflight_.end();) {
    if (covered(it->first))
      it = inflight_.erase(it);
    else
      ++it;
  }
}

uint16_t ReliableChannel::ackBits() const {
  // Exact SACK: the reorder buffer IS the set of received-but-not-contiguous
  // packets, so bit i = "reorder_ holds ack+1+i". No aliasing ring needed.
  uint16_t bits = 0;
  const uint16_t base = cumulativeAck_;
  for (int i = 0; i < 16; ++i) {
    if (reorder_.count(static_cast<uint16_t>(base + 1 + i)))
      bits |= static_cast<uint16_t>(1u << i);
  }
  return bits;
}

bool ReliableChannel::receive(const uint8_t* data, size_t len) {
  quantize::ByteReader in(data, len);
  PacketHeader h;
  if (!decodeAccepted(data, len, h, in)) return false;
  if (!(h.flags & kFlagReliable)) return false;  // not the reliable channel
  if (in.remaining() != h.payloadLen) return false;  // exact framing
  return receive(h, data + kHeaderLen, in.remaining());
}

bool ReliableChannel::receive(const PacketHeader& h, const uint8_t* payload, size_t payloadLen) {
  onAck(h.ack, h.ackBits);

  if (h.seq == 0) {
    // Ack-only heartbeat (see heartbeat()): pure ack flow. Not sequenced,
    // not buffered, never delivered. Anything else on seq 0 is malformed.
    return h.type == PayloadType::Ack && payloadLen == 0;
  }

  BufferedPacket pkt{h.type, std::vector<uint8_t>(payload, payload + payloadLen)};

  if (h.seq == expected_) {
    // Receive-side flow control (audit MEDIUM 2026-10-11): bound the
    // in-order delivery queue like the reorder buffer. Past maxReady the
    // incoming payload is dropped (counted in droppedReady_) instead of
    // growing without bound. Ack processing already ran above, so the
    // sender's window slides; the delivered stream stays a truncated
    // prefix, never reordered.
    if (ready_.size() < cfg_.maxReady)
      ready_.push_back(std::move(pkt));
    else
      ++droppedReady_;
    cumulativeAck_ = h.seq;
    advanceExpected();
  } else if (reorder_.count(h.seq)) {
    // Duplicate of an already-buffered packet: valid, already counted.
  } else if (seqGreater(h.seq, expected_)) {
    // Out-of-order future packet: hold for in-order delivery. On overflow
    // shed load -- the sender's RTO will retransmit; dropping here never
    // loses data, it only delays it.
    if (reorder_.size() < cfg_.maxBuffered) reorder_.emplace(h.seq, std::move(pkt));
  }
  // Else: old duplicate (seq at-or-before cumulativeAck_): drop. Ack
  // processing above already ran, so the sender still learns it arrived.
  return true;
}

void ReliableChannel::advanceExpected() {
  for (;;) {
    ++expected_;
    if (expected_ == 0) expected_ = 1;  // seq 0 marks ack-only heartbeats
    auto it = reorder_.find(expected_);
    if (it == reorder_.end()) break;
    cumulativeAck_ = it->first;
    // Same receive-side cap as the in-order path above: drain pressure here
    // frees reorder_ even when ready_ is full.
    if (ready_.size() < cfg_.maxReady)
      ready_.push_back(std::move(it->second));
    else
      ++droppedReady_;
    reorder_.erase(it);
  }
}

// --- UnreliableChannel -------------------------------------------------------

bool UnreliableChannel::decodeAccepted(const uint8_t* data, size_t len, PacketHeader& h,
                                       quantize::ByteReader& in) {
  return decodeAcceptedHeader(data, len, accepted_, h, in, &lastReject_);
}

bool UnreliableChannel::send(const uint8_t* payload, size_t len, PayloadType type,
                             std::vector<uint8_t>& datagram) {
  if (len > kMaxUdpPayload - kHeaderLen) return false;
  PacketHeader h;
  h.version = version_;  // negotiated version once the handshake agrees
  h.flags = 0;  // channel bit clear = unreliable
  h.seq = nextSeq_++;
  if (nextSeq_ == 0) nextSeq_ = 1;  // seq 0 is reserved
  h.type = type;
  h.payloadLen = static_cast<uint16_t>(len);

  quantize::ByteWriter w;
  encodeHeader(h, w);
  if (len) w.putBytes(payload, len);
  datagram = w.buf;
  return true;
}

bool UnreliableChannel::receive(const uint8_t* data, size_t len,
                                std::vector<uint8_t>& payload, PayloadType& type) {
  quantize::ByteReader in(data, len);
  PacketHeader h;
  if (!decodeAccepted(data, len, h, in)) return false;
  if (h.flags & kFlagReliable) return false;  // the reliable channel's traffic
  if (in.remaining() != h.payloadLen) return false;  // exact framing
  return receive(h, data + kHeaderLen, in.remaining(), payload, type);
}

bool UnreliableChannel::receive(const PacketHeader& h, const uint8_t* payload, size_t payloadLen,
                                std::vector<uint8_t>& out, PayloadType& type) {
  if (!gate_.accept(h.seq)) return false;            // old or duplicate
  type = h.type;
  out.assign(payload, payload + payloadLen);
  return true;
}

// --- ChannelMux --------------------------------------------------------------

bool ChannelMux::send(PayloadType type, const uint8_t* payload, size_t len, uint64_t nowMs,
                      std::vector<uint8_t>& datagram) {
  if (type == PayloadType::Snapshot) return false;  // passthrough (see header)
  if (map_.channelFor(type) == ChannelId::Reliable)
    return reliable_.send(payload, len, type, nowMs, datagram);
  return unreliable_.send(payload, len, type, datagram);
}

// --- ChannelDemux ------------------------------------------------------------

bool ChannelDemux::receive(const uint8_t* data, size_t len) const {
  quantize::ByteReader in(data, len);
  PacketHeader h;
  if (!decodeAcceptedHeader(data, len, accepted_, h, in, nullptr)) return false;
  if (h.flags & kFlagReliable) {
    // Header-threading first: one header parse per datagram instead of two.
    if (reliableHeader_) {
      reliableHeader_(h, data, len);
      return true;
    }
    if (!reliable_) return false;
    reliable_(data, len);
  } else {
    if (unreliableHeader_) {
      unreliableHeader_(h, data, len);
      return true;
    }
    if (!unreliable_) return false;
    unreliable_(data, len);
  }
  return true;
}

}  // namespace net
}  // namespace litholattice
