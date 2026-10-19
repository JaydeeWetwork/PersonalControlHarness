// Reliable + unreliable channels over one UDP socket.
//
// CHANNEL MODEL: every datagram carries a channel bit in the packet header
// (kFlagReliable, bit2 -- the 14-byte header does NOT grow). One UDP socket
// serves both channels; ChannelDemux routes inbound datagrams by the bit.
//
//   Reliable    ordered, acked, retransmitted (RTO, default 200 ms). For
//               events, critical state, anything that must arrive exactly
//               once, in order. Reuses the header ack/ackBits fields and
//               RFC-1982 seq arithmetic -- no second ack scheme is invented.
//               Ack meaning is TCP-style cumulative + SACK-above (ack =
//               highest contiguously received, bits = the 16 after ack),
//               which differs from the snapshot stream's use of the same
//               fields (highest received, bits below -- see protocol.h).
//               The field layout on the wire is identical.
//   Unreliable  fire-and-forget, sequenced, latest-wins (old/duplicates
//               dropped, never blocks, never retransmits). For inputs and
//               frequent small updates. SnapshotReceiver already uses this
//               pattern for the snapshot stream; the gate is factored here
//               as NewestOnly so the generic channel shares it.
//
// WHICH TRAFFIC GOES WHERE is config: ChannelMap (game-filled). Defaults
// preserve today's wire behavior exactly: SnapshotClient keeps framing its
// own packets (bit clear, no retransmits, deltas rebase on the newest ack)
// and those datagrams pass through the multiplexer untouched. Events default
// to reliable; chunk batches and acks default to unreliable.
//
// TRANSPORT: none here. Channels produce/consume framed datagrams
// (std::vector<uint8_t>); the game hands them to UDP. Time is a
// game-provided monotonic millisecond clock (fakeable in tests).
//
// THREADING: not thread-safe by design (owned by the net tick), like
// PrioManager and SnapshotClient.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <vector>

#include "litholattice/net/protocol.h"

namespace litholattice {
namespace net {

/// Channel a datagram travels on. The wire encoding is the kFlagReliable
/// header bit (set = Reliable, clear = Unreliable); this enum is the
/// config-level name for it.
enum class ChannelId : uint8_t {
  Unreliable = 0,
  Reliable = 1,
};

/// Latest-wins sequence gate: accepts a seq iff it is newer (RFC-1982) than
/// every previously accepted one. This is the pattern SnapshotReceiver uses
/// for the snapshot stream, factored so UnreliableChannel shares it instead
/// of duplicating the logic.
class NewestOnly {
 public:
  bool accept(uint16_t seq) {
    if (have_ && !seqGreater(seq, highest_)) return false;
    highest_ = seq;
    have_ = true;
    return true;
  }
  bool have() const { return have_; }
  uint16_t highest() const { return highest_; }

 private:
  uint16_t highest_ = 0;
  bool have_ = false;
};

/// Which channel each traffic kind uses. The game fills this once; the
/// multiplexer consults it when framing. Defaults preserve today's wire
/// behavior: snapshots stay on the effectively-unreliable delta stream
/// (SnapshotClient frames its own packets; the channel layer passes those
/// datagrams through untouched -- see ChannelMux), events go reliable.
struct ChannelMap {
  ChannelId snapshot = ChannelId::Unreliable;
  ChannelId ack = ChannelId::Unreliable;
  ChannelId chunkBatch = ChannelId::Unreliable;
  ChannelId event = ChannelId::Reliable;

  ChannelId channelFor(PayloadType type) const {
    switch (type) {
      case PayloadType::Snapshot:
        return snapshot;
      case PayloadType::Ack:
        return ack;
      case PayloadType::ChunkBatch:
        return chunkBatch;
      case PayloadType::Event:
        return event;
      case PayloadType::Handshake:
        return ChannelId::Reliable;  // the connect handshake must not be lost
    }
    return ChannelId::Unreliable;  // game-defined types default to unreliable
  }
};

struct ReliableConfig {
  uint64_t rtoMs = 200;            ///< retransmit timeout (ms, game clock)
  uint32_t maxInFlight = 64;       ///< unacked cap: send() fails past this (backpressure)
  uint32_t maxBuffered = 256;      ///< out-of-order receive buffer cap (datagrams)
  uint32_t maxReady = 256;         ///< in-order delivery queue cap (datagrams).
                                   ///< Past this, inbound payloads are dropped
                                   ///< (counted in droppedReadyCount()); ack
                                   ///< processing already ran, so the sender's
                                   ///< window still slides. Drop-incoming keeps
                                   ///< the delivered stream a truncated prefix,
                                   ///< never reordered (audit MEDIUM 2026-10-11).
  size_t maxPayloadBytes = 1200;   ///< payload cap per datagram (< MTU with header)
};

/// One endpoint of the reliable channel (both directions live here: send
/// state for outbound payloads, receive state for inbound ones, like a
/// mini-TCP). Ordered, acked, retransmitted:
///
///   - send() frames a payload (channel bit set, own seq starting at 1;
///     seq 0 marks ack-only heartbeats, never a payload) and queues it for
///     retransmission. Returns false on oversize payload or a full
///     in-flight window -- the caller applies backpressure (chunks, or
///     retries later); nothing is silently dropped.
///   - poll(nowMs) returns retransmits due at the RTO. Retransmits carry
///     the ORIGINAL seq (the receiver dedups) but FRESH ack/ackBits, so ack
///     state keeps flowing even when only retransmits are going out.
///   - receive() ingests one inbound datagram: it processes the header
///     ack/ackBits (driving retransmission), then buffers the payload for
///     in-order delivery. Out-of-order payloads wait; duplicates and old
///     packets are dropped.
///   - pollDelivery() pops the next in-order payload.
///   - heartbeat() emits an ack-only datagram (seq 0, not sequenced, not
///     retransmitted -- TCP-ACK style): with no reliable payload to send,
///     this is how acks keep flowing (the game sends these on a timer
///     whenever the channel is active).
///
/// ACK SEMANTICS (TCP-style, in the existing header ack/ackBits fields):
/// ack = highest CONTIGUOUSLY received seq (0 when nothing is), bit i =
/// "also received ack+1+i" (selective ack for the 16 packets above ack).
/// This differs from the snapshot stream's use of the same fields
/// (highest received, bits for the 16 BELOW -- see protocol.h): a reliable
/// stream needs cumulative acknowledgment so the sender can safely evict
/// old packets. The field layout on the wire is identical; only the
/// per-channel interpretation differs. RFC-1982 seq arithmetic throughout.
///
/// A channel is a fresh pairing: both sides start their seq at 1 and the
/// receiver expects 1 first. Create a new ReliableChannel per connection.
class ReliableChannel {
 public:
  explicit ReliableChannel(ReliableConfig cfg = ReliableConfig{}) : cfg_(cfg) {}

  /// Queue `payload` for reliable ordered delivery. `nowMs` seeds the RTO
  /// clock for this packet. `datagram` receives the framed bytes for the
  /// socket. False = payload too big, or the in-flight window is full.
  /// Seq 0 is never used for payloads (it marks ack-only heartbeats).
  bool send(const uint8_t* payload, size_t len, PayloadType type, uint64_t nowMs,
            std::vector<uint8_t>& datagram);
  bool send(const std::vector<uint8_t>& payload, PayloadType type, uint64_t nowMs,
            std::vector<uint8_t>& datagram) {
    return send(payload.data(), payload.size(), type, nowMs, datagram);
  }

  /// Ack-only heartbeat: a header-only datagram (channel bit set, seq 0,
  /// type Ack, empty payload) carrying the current ack/ackBits. Like TCP
  /// ACKs it is NOT sequenced, NOT queued, and NOT retransmitted -- it is
  /// pure ack flow. The game sends these on a timer whenever the channel
  /// is active but has no reliable payload (that is how acks keep moving
  /// after the last data packet). Always succeeds (no backpressure).
  bool heartbeat(uint64_t nowMs, std::vector<uint8_t>& datagram);

  /// Retransmits due at `nowMs` (monotonic ms, game clock). Each entry is a
  /// full datagram ready for the socket. Empty when everything is acked.
  std::vector<std::vector<uint8_t>> poll(uint64_t nowMs);

  /// Process a peer ack (also called internally by receive() from the header
  /// ack/ackBits). Cumulative + selective (SACK above ack), RFC-1982; safe
  /// to call with stale/duplicate acks.
  ///
  /// SECURITY LIMITATION (audit MEDIUM 2026-10-11, no cheap in-class fix):
  /// ack fields ride plaintext in the 14-byte header, so a spoofed ack
  /// (seq=0, ack=N, ackBits=0xFFFF) evicts the whole in-flight window and
  /// the sender stops retransmitting genuinely-lost datagrams. The eviction
  /// math is window-bounded and RFC-1982-correct, but eviction *within* the
  /// window is attacker-controlled. The real fix is the builder's
  /// authenticated KEX (encryption.h, kHsKeyExchange) plus the game binding
  /// channels to the peer address/port at the socket layer — FLAGGED for
  /// the builder. Until then the ordered/acked guarantee holds only against
  /// non-adversarial loss.
  void onAck(uint16_t ack, uint16_t ackBits);

  /// Ingest one inbound datagram (the demux routes by channel bit). Returns
  /// false on malformed input or a clear channel bit (not our channel).
  /// Valid duplicates/out-of-order packets return true (ack processing still
  /// ran); their payloads are deduped/buffered, never delivered twice.
  /// Ack-only heartbeats (seq 0) only run ack processing and are never
  /// delivered via pollDelivery. In-order payloads queue in ready_ up to
  /// maxReady; past the cap inbound payloads are dropped and counted
  /// (droppedReadyCount()) — ack processing already ran, so the sender's
  /// window slides and the delivered stream is a truncated prefix.
  bool receive(const uint8_t* data, size_t len);
  /// Ingest with an already-parsed header (see
  /// ChannelDemux::onReliableHeader): skips the channel's second header
  /// decode. `payload` points at the first payload byte (data + kHeaderLen)
  /// with `payloadLen` bytes available. Precondition: the header was
  /// accepted under the same accepted-version set this channel uses (the
  /// game installs one set on the demux and on every channel; see
  /// setAcceptedVersions).
  bool receive(const PacketHeader& h, const uint8_t* payload, size_t payloadLen);

  /// Pop the next in-order payload. False when none is ready.
  bool pollDelivery(std::vector<uint8_t>& payload, PayloadType& type) {
    if (ready_.empty()) return false;
    payload = std::move(ready_.front().payload);
    type = ready_.front().type;
    ready_.pop_front();
    return true;
  }

  /// Outgoing ack state, stamped into every datagram this side frames (fresh
  /// values also re-stamped onto retransmits by poll()). Cumulative-ack +
  /// SACK-above semantics (see class doc): ackSeq() is the highest
  /// contiguously received seq (0 when none), ackBits() bit i marks
  /// "also received ackSeq()+1+i".
  uint16_t ackSeq() const { return cumulativeAck_; }
  uint16_t ackBits() const;

  size_t unackedCount() const { return inflight_.size(); }
  size_t reorderSize() const { return reorder_.size(); }
  size_t readyCount() const { return ready_.size(); }
  /// Inbound payloads dropped because the in-order delivery queue hit
  /// maxReady (audit MEDIUM 2026-10-11). A rising count means the game is
  /// not draining pollDelivery() fast enough — or the peer is flooding.
  uint64_t droppedReadyCount() const { return droppedReady_; }
  const ReliableConfig& config() const { return cfg_; }

  /// Version byte stamped on outbound headers: the negotiated protocol
  /// version once the connect handshake agrees (kProtocolVersion before
  /// that). The game calls this after ConnectHandshake reaches Agreed.
  void setVersion(uint8_t v) { version_ = v; }
  uint8_t version() const { return version_; }

  /// Protocol versions accepted on receive. Before the handshake completes
  /// the game installs knownProtocolVersions() (so a v1-stamped ClientHello
  /// still routes on a v2 server); after agreement it narrows the set to
  /// the single agreed version. A version outside the set is rejected --
  /// see lastReject() -- never silently dropped.
  void setAcceptedVersions(std::vector<uint8_t> v) { accepted_ = std::move(v); }
  const std::vector<uint8_t>& acceptedVersions() const { return accepted_; }
  /// Detail of the most recent header rejection (version mismatch).
  /// Cleared on the next accepted header.
  HeaderReject lastReject() const { return lastReject_; }

 private:
  struct InflightPacket {
    uint16_t seq = 0;
    std::vector<uint8_t> datagram;  // as framed (retransmits re-stamp acks)
    uint64_t lastSentMs = 0;
  };
  struct BufferedPacket {
    PayloadType type = PayloadType::Ack;
    std::vector<uint8_t> payload;
  };

  ReliableConfig cfg_;
  uint16_t nextSeq_ = 1;  // seq 0 marks ack-only heartbeats; never a payload seq
  std::map<uint16_t, InflightPacket> inflight_;  // unacked, by seq

  uint8_t version_ = kProtocolVersion;                  // stamped on send
  std::vector<uint8_t> accepted_ = {kProtocolVersion};  // accepted on receive
  HeaderReject lastReject_;

  uint16_t expected_ = 1;  // next seq to deliver (fresh pairing starts at 1)
  uint16_t cumulativeAck_ = 0;  // highest contiguously received seq (0 = none)
  std::map<uint16_t, BufferedPacket> reorder_;  // out-of-order, by seq
  std::deque<BufferedPacket> ready_;           // in-order, awaiting pollDelivery
  uint64_t droppedReady_ = 0;  // inbound payloads dropped past maxReady

  void advanceExpected();
  void restampAcks(std::vector<uint8_t>& datagram) const;
  /// Decode a header accepting any version in accepted_ (see the setters
  /// above). On success `in` is positioned past the header; on failure
  /// lastReject_ describes a version mismatch (bad magic/truncation fail
  /// without a reject, there is nothing version-specific to report).
  bool decodeAccepted(const uint8_t* data, size_t len, PacketHeader& h, quantize::ByteReader& in);
};

/// Fire-and-forget sequenced channel: latest-seq-wins. send() frames one
/// datagram (channel bit clear, own seq); receive() accepts it iff its seq is
/// newer than every previously accepted one (NewestOnly -- the same gate the
/// snapshot stream uses). Old packets and duplicates are dropped. Never
/// blocks, never retransmits, needs no clock.
class UnreliableChannel {
 public:
  /// Frame one unreliable datagram. False only on oversize payload.
  bool send(const uint8_t* payload, size_t len, PayloadType type,
            std::vector<uint8_t>& datagram);
  bool send(const std::vector<uint8_t>& payload, PayloadType type,
            std::vector<uint8_t>& datagram) {
    return send(payload.data(), payload.size(), type, datagram);
  }

  /// Ingest one inbound datagram. Returns true (filling `payload`/`type`)
  /// iff it is newer than everything seen before. Malformed input, a set
  /// channel bit, old packets and duplicates all return false.
  bool receive(const uint8_t* data, size_t len, std::vector<uint8_t>& payload,
               PayloadType& type);
  /// Ingest with an already-parsed header (see
  /// ChannelDemux::onUnreliableHeader): skips the channel's second header
  /// decode. Same precondition as ReliableChannel::receive(h, payload, len).
  bool receive(const PacketHeader& h, const uint8_t* payload, size_t payloadLen,
               std::vector<uint8_t>& out, PayloadType& type);

  uint16_t nextSeq() const { return nextSeq_; }
  bool haveReceived() const { return gate_.have(); }
  uint16_t highestReceived() const { return gate_.highest(); }

  /// Version byte stamped on outbound headers (see ReliableChannel).
  void setVersion(uint8_t v) { version_ = v; }
  uint8_t version() const { return version_; }
  /// Protocol versions accepted on receive (see ReliableChannel).
  void setAcceptedVersions(std::vector<uint8_t> v) { accepted_ = std::move(v); }
  const std::vector<uint8_t>& acceptedVersions() const { return accepted_; }
  HeaderReject lastReject() const { return lastReject_; }

 private:
  uint16_t nextSeq_ = 1;  // seq 0 is reserved
  NewestOnly gate_;
  uint8_t version_ = kProtocolVersion;                  // stamped on send
  std::vector<uint8_t> accepted_ = {kProtocolVersion};  // accepted on receive
  HeaderReject lastReject_;

  bool decodeAccepted(const uint8_t* data, size_t len, PacketHeader& h, quantize::ByteReader& in);
};

/// Send-side multiplexer: one UDP socket serves both channels. Frames a
/// payload per the ChannelMap and hands the datagram to the socket.
///
/// Snapshot passthrough: PayloadType::Snapshot ALWAYS returns false here.
/// SnapshotClient owns snapshot framing (its own seq/ack state drives the
/// delta baselines), so its datagrams go to the socket directly, untouched --
/// channel bit clear, today's wire behavior bit-for-bit. Mapping
/// ChannelMap::snapshot = Reliable is accepted by the config but needs a
/// SnapshotClient framing change to take effect (open question; the game
/// cannot feed framed snapshot packets through the reliable channel, which
/// needs bare payloads).
class ChannelMux {
 public:
  explicit ChannelMux(ChannelMap map = ChannelMap{}) : map_(map) {}

  const ChannelMap& map() const { return map_; }
  void setMap(ChannelMap m) { map_ = m; }
  ReliableChannel& reliable() { return reliable_; }
  UnreliableChannel& unreliable() { return unreliable_; }

  /// Frame `payload` on its mapped channel. `nowMs` feeds the reliable RTO
  /// clock. Returns false for PayloadType::Snapshot (passthrough -- the
  /// caller already holds the SnapshotClient-framed datagram), for oversize
  /// payloads, or on reliable backpressure.
  bool send(PayloadType type, const uint8_t* payload, size_t len, uint64_t nowMs,
            std::vector<uint8_t>& datagram);

  /// Version byte stamped on outbound headers by both channels (see
  /// ReliableChannel::setVersion). The game calls this after the connect
  /// handshake agrees on a version.
  void setVersion(uint8_t v) {
    reliable_.setVersion(v);
    unreliable_.setVersion(v);
  }
  /// Protocol versions accepted on receive by both channels (see
  /// ReliableChannel::setAcceptedVersions).
  void setAcceptedVersions(std::vector<uint8_t> v) {
    reliable_.setAcceptedVersions(v);
    unreliable_.setAcceptedVersions(v);
  }

 private:
  ChannelMap map_;
  ReliableChannel reliable_;
  UnreliableChannel unreliable_;
};

/// Receive-side demultiplexer: routes one inbound datagram by the channel
/// bit (kFlagReliable). Legacy snapshot datagrams (bit clear, type Snapshot)
/// arrive on the unreliable side, exactly as today -- the game routes those
/// to its SnapshotReceiver and everything else to UnreliableChannel.
class ChannelDemux {
 public:
  using Handler = std::function<void(const uint8_t* data, size_t len)>;
  /// Header-threading handler: like Handler, but also receives the parsed
  /// PacketHeader, so the channel's receive(h, payload, len) overload can
  /// skip its second header decode (one parse per datagram, not two).
  using HeaderHandler =
      std::function<void(const PacketHeader& h, const uint8_t* data, size_t len)>;

  void onReliable(Handler h) { reliable_ = std::move(h); }
  void onUnreliable(Handler h) { unreliable_ = std::move(h); }
  void onReliableHeader(HeaderHandler h) { reliableHeader_ = std::move(h); }
  void onUnreliableHeader(HeaderHandler h) { unreliableHeader_ = std::move(h); }

  /// Route one datagram. False on malformed header or when no handler is
  /// installed for the datagram's channel. When a HeaderHandler is installed
  /// for the datagram's channel it is called with the already-parsed header
  /// (preferred: avoids the double header decode); otherwise the legacy
  /// Handler gets the raw datagram and the channel re-parses the header.
  bool receive(const uint8_t* data, size_t len) const;

  /// Protocol versions accepted when routing (see
  /// ReliableChannel::setAcceptedVersions). Before the connect handshake
  /// completes the game installs knownProtocolVersions() so a v1-stamped
  /// ClientHello still routes on a v2 server; after agreement it narrows to
  /// the agreed version.
  void setAcceptedVersions(std::vector<uint8_t> v) { accepted_ = std::move(v); }

 private:
  Handler reliable_;
  Handler unreliable_;
  HeaderHandler reliableHeader_;
  HeaderHandler unreliableHeader_;
  std::vector<uint8_t> accepted_ = {kProtocolVersion};
};

}  // namespace net
}  // namespace litholattice
