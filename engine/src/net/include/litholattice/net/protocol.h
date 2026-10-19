// Packet framing and the snapshot wire protocol (v2), plus the v3 stable-ID
// wire records (BlockUpdate; see the v3 section at the bottom).
//
// PACKET HEADER (14 bytes, little-endian):
//
//   offset  size  field
//   0       2     magic = 0x4C4C ('LL')
//   2       1     protocol version (LITHOLATTICE_PROTOCOL_V3; v2 still negotiated
//                   with older peers via the handshake)
//   3       1     flags: bit0 = truncated (byte budget cut this snapshot,
//                           more entity updates pending for the next slot)
//                    bit1 = needFull (client -> server: "my baseline is gone,
//                           send the next snapshot as a full snapshot")
//                    bit2 = reliable (CHANNEL BIT: 1 = reliable channel,
//                           0 = unreliable -- includes the legacy snapshot
//                           stream, whose wire format is unchanged.
//                           The 14-byte header does NOT grow: the channel
//                           rides in the existing flags byte. See channels.h.)
//   4       2     seq: packet sequence number, uint16, wraps.
//                   Ordering uses RFC-1982 serial arithmetic: seqGreater(a, b)
//                   is ((int16_t)(a - b)) > 0. At 60 Hz a 16-bit space wraps
//                   every ~18 minutes; the >2^15 comparison window is plenty.
//   6       2     ack: highest sequence number received from the peer
//   8       2     ackBits: selective ack; bit i = "I also received ack-1-i"
//                   (covers the 16 packets before ack; absorbs single loss)
//   10      2     payload type (PayloadType)
//   12      2     payload length in bytes (excludes this header)
//
// SNAPSHOT PAYLOAD (type 1):
//
//   u32  serverTick    the sim tick this snapshot represents (512 TPS grid)
//   u32  baselineSeq   seq this delta was encoded against; 0 = full snapshot
//                      (encoded against the empty state), i.e. Fiedler's
//                      "encoded relative to initial state" flag as a value.
//   uvarint nUpdated   number of entity update/spawn records
//   nUpdated x records, entities sorted by id ascending:
//     uvarint idGap   (id - prevId - 1); first record: id itself (prevId = -1)
//     u8      mask    bit0 pos changed      bit1 vel changed
//                     bit2 orient changed   bit3 flags changed
//                     bit4 spawned (absolute record, not a delta)
//                     bit5 atRest (Fiedler's 1-bit flag: velocity is implied
//                          zero and takes no bytes on the wire)
//     if spawned:  s32 x,y,z (Q10 fixed point, absolute)
//                  s32 vx,vy,vz (absolute) unless atRest
//                  u32 orient (smallest-three) if hasOrient... (see below)
//                  u32 flags (absolute)
//     else:        for each changed field, the DELTA vs the baseline value:
//                  pos:  3 x varint(dx,dy,dz)   (zigzag via signed varint)
//                  vel:  3 x varint(dvx,dvy,dz) (skipped when atRest: the
//                        baseline's effective velocity is 0 while atRest,
//                        so the delta formula still holds)
//                  orient: u32 absolute (deltas of rotations are meaningless)
//                  flags: uvarint absolute new value
//   uvarint nRemoved  number of despawned entities
//   nRemoved x uvarint idGap (same gap coding)
//
// The hasOrient/hasFlags presence is itself part of `flags` (game-defined
// bits): orient bytes appear iff the entity's flags say it has an
// orientation. The codec does not interpret game flag bits; the game must
// keep them consistent between baseline and current (they are compared
// like any other field).
//
// DELTA MODEL (Quake III style): the server keeps a per-client ring of sent
// snapshots back to the client's last ack. Each snapshot is delta-encoded
// against the client's last acked snapshot. The server never waits for an
// ACK and never retransmits: after loss, the next delta is simply rebased on
// the newest ack, so deltas grow to cover the gap. A client that loses its
// baseline sets needFull and gets one full snapshot to resync.
//
// BYTE BUDGET: encode takes maxBytes; entity records are packed until the
// budget would be exceeded, then the truncated flag is set. Nothing is lost:
// the dropped updates are still newer than the baseline, so the NEXT delta
// vs the (possibly advanced) baseline re-includes them. Removals are packed
// first (a lingering ghost entity is worse than a stale position).
//
// SPIRAL OF DEATH (research cross-cutting note): the snapshotter must never
// emit catch-up bursts. If a snapshot slot is missed, it is skipped; the next
// delta rebases on the newest ack. Server-side this composes with the fixed-
// timestep clamp: the clamp is slowdown (drop time), not catch-up (no extra
// ticks), so the snapshotter never sees a burst of slots to fill either.
//
// TRANSPORT: none here. encode produces a std::vector<uint8_t>; the game
// hands it to UDP later. The codec is MTU-aware via maxBytes (default keeps
// packets under ~1400 bytes).
#pragma once

#include <cstdint>
#include <vector>

#include "litholattice/net/quantize.h"
#include "litholattice/net/version.h"

namespace litholattice {
namespace net {

// This wire format (delta snapshots, quantization, varint packing) is new
// vs v1 ("full PlayerState, no compression"): new encodings are new protocol
// versions, per version.h. Negotiation semantics (highest common version,
// per-version codec selection) are defined by the connect handshake in
// handshake.h: highest mutually compatible protocol version wins, the
// result feeds codecForProtocol() which selects the wire codec.
#define LITHOLATTICE_PROTOCOL_V2 2
/// v3 (2026-10-12, Infinite Registry phase 1): adds the 64-bit LEB128 varint
/// primitives (quantize::putUVarint64/getUVarint64) and the BlockUpdate wire
/// record below. All v2 encodings are unchanged; v3 peers still negotiate v2
/// with older binaries via the handshake's highest-common-version rule.
#define LITHOLATTICE_PROTOCOL_V3 3

constexpr uint16_t kMagic = 0x4C4C;  // 'LL', little-endian
constexpr uint8_t kProtocolVersion = LITHOLATTICE_PROTOCOL_V3;
constexpr size_t kHeaderLen = 14;
constexpr size_t kMaxUdpPayload = 1400;  // datagram cap incl. header

/// Runtime copy of kProtocolVersion for the connect handshake: stamp this
/// (via knownProtocolVersions()/setVersion()) rather than re-reading the
/// macro at each call site. See version.h's runtimeEngineVersion().
inline uint8_t runtimeProtocolVersion() { return kProtocolVersion; }

enum class PayloadType : uint16_t {
  Snapshot = 1,
  Ack = 2,        // header-only heartbeat; ack/ackBits/needFull live in the header
  ChunkBatch = 3,  // reserved for the chunk-streaming layer
  Event = 4,      // reserved for reliable-ish event traffic
  Handshake = 5,  // connect handshake (ClientHello/ServerHello/Reject); rides
                  // the reliable channel so it cannot be lost. See handshake.h.
};

constexpr uint8_t kFlagTruncated = 1 << 0;
constexpr uint8_t kFlagNeedFull = 1 << 1;
/// Channel bit (bit2): set = reliable channel (ordered, acked, retransmitted;
/// see channels.h), clear = unreliable (fire-and-forget, latest-wins -- also
/// the legacy snapshot stream, whose wire format is unchanged). The 14-byte
/// header does NOT grow: the channel rides in the existing flags byte.
constexpr uint8_t kFlagReliable = 1 << 2;

struct PacketHeader {
  uint8_t version = kProtocolVersion;
  uint8_t flags = 0;
  uint16_t seq = 0;
  uint16_t ack = 0;
  uint16_t ackBits = 0;
  PayloadType type = PayloadType::Snapshot;
  uint16_t payloadLen = 0;

  bool operator==(const PacketHeader&) const = default;
};

/// RFC-1982 serial-number arithmetic for the 16-bit sequence space.
inline bool seqGreater(uint16_t a, uint16_t b) { return static_cast<int16_t>(a - b) > 0; }
inline bool seqGreaterEq(uint16_t a, uint16_t b) { return static_cast<int16_t>(a - b) >= 0; }

bool encodeHeader(const PacketHeader& h, quantize::ByteWriter& out);

/// Why decodeHeader rejected a packet. A version mismatch is an explicit,
/// reportable rejection (the game can log it or answer with a handshake
/// Reject) — never a silent drop.
struct HeaderReject {
  bool versionMismatch = false;  ///< false = bad magic or truncated input
  uint8_t got = 0;               ///< the version byte that arrived (when versionMismatch)
};

/// Validates magic; returns false (header untouched) on bad magic, a version
/// outside `acceptedVersion`, or truncated input. On a version mismatch
/// `reject` is filled so the caller can report exactly what arrived.
///
/// The default accepted version is the compiled one (kProtocolVersion);
/// after the connect handshake agrees on a version the game narrows the
/// accepted set to it (see channels.h / snapshot.h setters). During the
/// handshake the accepted set is the known protocol versions, so a
/// v1-stamped ClientHello still routes on a v2 server.
bool decodeHeader(quantize::ByteReader& in, PacketHeader& out, uint8_t acceptedVersion,
                  HeaderReject* reject = nullptr);
/// Accepts kProtocolVersion only (pre-handshake default). Kept for call
/// sites that do not track a negotiated version.
inline bool decodeHeader(quantize::ByteReader& in, PacketHeader& out) {
  return decodeHeader(in, out, kProtocolVersion, nullptr);
}
/// Same, but reports a version mismatch through `reject`.
inline bool decodeHeader(quantize::ByteReader& in, PacketHeader& out, HeaderReject* reject) {
  return decodeHeader(in, out, kProtocolVersion, reject);
}

// ---------------------------------------------------------------------------
// Snapshot message.
// ---------------------------------------------------------------------------

/// One replicated entity. Positions/velocities are Q10 fixed point (see
/// quantize.h). `flags` bits are game-defined; bit meanings the codec cares
/// about: kEntHasOrient (orient field valid on the wire).
struct SnapshotEntity {
  uint32_t id = 0;
  int32_t x = 0, y = 0, z = 0;      // Q10 fixed point, blocks
  int32_t vx = 0, vy = 0, vz = 0;   // Q10 fixed point, blocks/tick
  uint32_t orient = 0;              // smallest-three packed quaternion
  uint32_t flags = 0;               // game-defined bits
  bool atRest = false;              // Fiedler 1-bit: velocity implied zero
  bool hasOrient = false;

  bool operator==(const SnapshotEntity&) const = default;
};

constexpr uint32_t kEntHasOrient = 1u << 0;

/// Field-change mask bits used inside entity records.
enum SnapshotField : uint8_t {
  kFieldPos = 1 << 0,
  kFieldVel = 1 << 1,
  kFieldOrient = 1 << 2,
  kFieldFlags = 1 << 3,
  kFieldSpawned = 1 << 4,
  kFieldAtRest = 1 << 5,  // current atRest state (baseline's may differ)
};

struct Snapshot {
  uint32_t tick = 0;  // sim tick this snapshot represents
  uint32_t seq = 0;   // transport seq, filled by the snapshotter
  std::vector<SnapshotEntity> entities;  // sorted by id ascending

  bool operator==(const Snapshot&) const = default;
};

/// Optional encode side-channel: when non-null, encodeSnapshot fills it with
/// the ids of the entity records it packed (updates/spawns, id ascending)
/// and the ids it removed (ascending). The caller can then splice the exact
/// post-image baseline — what the client decodes — without re-parsing the
/// payload: packed records decode to the caller's current values bit-exactly
/// (for encoder-feasible inputs), updates cut by the byte budget keep the
/// baseline value, and cut spawns never shipped. See SnapshotClient::build().
struct EncodePackedInfo {
  std::vector<uint32_t> packedIds;   ///< records packed, id ascending
  std::vector<uint32_t> removedIds;  ///< removals packed, id ascending
};

/// Encode `current` against `baseline` (nullptr = full snapshot vs the empty
/// state). `baselineSeq` is written into the payload. Stops packing entity
/// updates before exceeding maxBytes; sets `truncated` when updates were cut.
/// Removals are always packed first. Returns false only on programmer error
/// (unsorted input is sorted internally, so this cannot fail otherwise).
/// `packedOut` optionally receives the packed record/removal ids (see
/// EncodePackedInfo).
bool encodeSnapshot(const Snapshot& current, const Snapshot* baseline, uint32_t baselineSeq,
                    size_t maxBytes, quantize::ByteWriter& out, bool& truncated,
                    EncodePackedInfo* packedOut = nullptr);

/// Decode a snapshot payload. `baseline` must be the snapshot with
/// baselineSeq (nullptr only when baselineSeq == 0, i.e. a full snapshot).
/// Returns false on malformed input or a missing baseline; `out` is untouched
/// on failure. On success, out.entities is sorted by id.
bool decodeSnapshot(quantize::ByteReader& in, const Snapshot* baseline, Snapshot& out);

/// Compute the changed-field mask between two records of the same entity.
uint8_t entityDiffMask(const SnapshotEntity& base, const SnapshotEntity& cur);

/// Reads the 8-byte snapshot prefix (tick + baselineSeq) without consuming the
/// rest. The receiver uses this to find the baseline BEFORE decoding the body.
bool decodeSnapshotPrefix(quantize::ByteReader& in, uint32_t& tick, uint32_t& baselineSeq);

/// Decodes the body after the prefix: entity records then removals.
/// `baseline` is the snapshot named by baselineSeq (nullptr only for a full
/// snapshot, baselineSeq == 0). On success out.tick = tick, out.entities is
/// sorted by id; out.seq is left for the caller (it comes from the packet
/// header, not the payload). `out` is untouched on failure.
bool decodeSnapshotBody(quantize::ByteReader& in, const Snapshot* baseline, uint32_t tick,
                        Snapshot& out);

// ---------------------------------------------------------------------------
// v3: stable-ID wire records (Infinite Registry).
// ---------------------------------------------------------------------------

/// A block/item change on the wire (protocol v3). The 64-bit StableId travels
/// as a LEB128 uvarint64 ("pack the zeros": small ids cost 1 byte); coordinates
/// are zigzag varints. This is the designated wire encoding for block/item
/// stable IDs; the game wires it into chunk/event traffic. Pure codec here.
struct BlockUpdate {
  int32_t x = 0, y = 0, z = 0;  ///< world coords
  uint64_t stableId = 0;        ///< block/item StableId (uvarint64 on the wire)
  bool operator==(const BlockUpdate&) const = default;
};

void encodeBlockUpdate(const BlockUpdate& u, quantize::ByteWriter& out);
/// False on truncated/malformed input; `out` is untouched on failure.
bool decodeBlockUpdate(quantize::ByteReader& in, BlockUpdate& out);

}  // namespace net
}  // namespace litholattice
