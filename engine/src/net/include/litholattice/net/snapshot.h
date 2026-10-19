// Snapshot cadence, delta baselines, and client interpolation.
//
// THE DECOUPLED MODEL (research section 6, cross-cutting note 1): the sim runs
// at 512 TPS; snapshots ship at 20-60 Hz (default 32 Hz = every 16th tick).
// Clients keep 2-3 snapshots and interpolate between them with a ~2-3
// snapshot-interval render delay (Fiedler). Per-tick state NEVER ships: at
// 512 Hz the UDP header overhead alone would be ~14 KB/s per player.
//
// SERVER (SnapshotClient, one per connected client):
//   - isSnapshotTick(tick): true when tick % divisor == 0, divisor =
//     round(simHz / snapshotHz). Default: 512/32 = 16.
//   - build(tick, entities): delta-encodes the entity list against the
//     client's last acked snapshot, frames it with the packet header, stores
//     a copy in the per-client history ring (default depth 64 ~= 2 s at
//     32 Hz), and returns the packet bytes.
//   - onAck(ack, ackBits): advances the baseline to the newest acked snapshot
//     and discards older history. The server never waits for an ACK and never
//     retransmits (Quake III model): after loss the next delta is rebased on
//     the newest ack, so deltas transparently grow to cover the gap.
//   - SPIRAL OF DEATH: build() emits at most one packet per call. If the game
//     misses a snapshot slot, it must NOT emit catch-up packets for the
//     missed slots; it just builds the next snapshot when its slot comes and
//     the delta rebases. This composes with the fixed-timestep clamp, which
//     is slowdown (drop time) rather than catch-up (no extra ticks): the
//     snapshotter therefore never sees a burst of slots to fill.
//
// CLIENT (SnapshotReceiver):
//   - receive(bytes): validates the header, drops duplicates and old packets
//     (seq <= highest), looks up the packet's baselineSeq in local history,
//     and decodes. Returns NeedBaseline when the delta's baseline is gone
//     (never received, or evicted); the client then sets needFull and the
//     server answers with one full snapshot. Self-healing: no retransmits.
//   - ackSeq()/ackBits() feed the client's outgoing ack/heartbeat packets.
//   - sample(tick, out): render state at sim-tick `tick`, interpolated
//     between the two buffered snapshots bracketing it. All fixed-point
//     integer math (Q16 alpha): every client computes the identical value.
//     Ticks outside the buffer clamp (hold), never extrapolate: prediction
//     is game policy, not transport policy.
//   - kInterpDelay: Fiedler's recommended render delay in snapshot intervals
//     (2). The game renders at newestTick - kInterpDelay * intervalTicks.
//
// INTEREST MANAGEMENT: SnapshotClient::build takes an optional byte budget
// (default: config maxPayloadBytes). Entity updates pack until the budget is
// hit; the truncated flag marks a cut snapshot and the dropped updates ride
// the next delta. Wire SnapshotClient to PrioManager (priority.h): give the
// snapshot stream its guaranteed floor, and chunk/event traffic splits the
// leftover by priority.
#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "litholattice/net/protocol.h"

namespace litholattice {
namespace net {

struct SnapshotConfig {
  uint32_t simHz = 512;          ///< authoritative sim rate (ticks/sec)
  uint32_t snapshotHz = 32;      ///< snapshot send rate (20-60 typical)
  uint32_t historyDepth = 64;    ///< per-client sent-snapshot ring depth
  uint32_t maxPayloadBytes = 1200;  ///< payload budget per snapshot (< MTU)
  uint32_t forcedFullMinInterval = 32;  ///< min builds between honored
      ///< requestFull()s (0 = no limit). A client waving kFlagNeedFull on
      ///< every ack gets at most one forced full per this many builds
      ///< (~1/s at 32 Hz); excess requests are dropped and counted
      ///< (see ignoredForcedFulls()). Audit LOW 2026-10-11.
};

/// Server-side per-client snapshot state.
class SnapshotClient {
 public:
  explicit SnapshotClient(SnapshotConfig cfg = SnapshotConfig{});

  /// Snapshot cadence divisor: snapshots ship when tick % divisor() == 0.
  uint32_t divisor() const;
  bool isSnapshotTick(uint64_t tick) const { return tick % divisor() == 0; }

  /// Build one snapshot packet for `tick`. `entities` need not be sorted.
  /// At most one packet per call; a missed slot is skipped, never caught up
  /// (spiral-of-death rule). `byteBudget` caps the payload (0 = config
  /// default); the truncated flag is set when updates were cut.
  /// Returns the full packet (14-byte header + payload), ready for UDP.
  ///
  /// Many clients, one tick: normalize ONCE with normalizeForSnapshot()
  /// and call buildNormalized() per client instead of paying the O(E log E)
  /// sort per client (opt 2026-10-11).
  std::vector<uint8_t> build(uint64_t tick, const std::vector<SnapshotEntity>& entities,
                             size_t byteBudget = 0);

  /// Normalize + sort a raw entity list exactly as build() does, for sharing
  /// across clients: atRest zeroes velocity, sorted by id, tick stamped.
  static Snapshot normalizeForSnapshot(uint64_t tick,
                                       const std::vector<SnapshotEntity>& entities);
  /// Build from an already-normalized snapshot (see normalizeForSnapshot):
  /// entities sorted by id, atRest velocities zeroed, tick stamped. `seq`
  /// is assigned here. Same packet as build() would emit for the same input.
  std::vector<uint8_t> buildNormalized(const Snapshot& normalized, size_t byteBudget = 0);

  /// Process the client's ack. Advances the delta baseline to the newest
  /// acked snapshot (by header ack, refined by ackBits) and evicts older
  /// history. Never regresses: an old ack is ignored.
  void onAck(uint16_t ack, uint16_t ackBits);

  /// Honor a client's needFull request: the next build() is a full snapshot.
  /// Rate-limited (audit LOW 2026-10-11): at most one honored request per
  /// SnapshotConfig::forcedFullMinInterval builds; a second request already
  /// pending is a no-op, and requests inside the interval are dropped and
  /// counted (see ignoredForcedFulls()). Server-internal resyncs (baseline
  /// evicted before the ack advanced; history-depth guard) bypass the
  /// limiter — they are correctness, not client requests.
  void requestFull();
  /// Client needFull requests dropped by the rate limiter.
  uint64_t ignoredForcedFulls() const { return ignoredForcedFulls_; }

  uint32_t baselineSeq() const { return baselineSeq_; }
  uint16_t nextSeq() const { return nextSeq_; }
  size_t historySize() const { return history_.size(); }
  const SnapshotConfig& config() const { return cfg_; }

  /// Version byte stamped on outbound snapshot headers: the negotiated
  /// protocol version once the connect handshake agrees (kProtocolVersion
  /// before that). The game calls this after ConnectHandshake reaches
  /// Agreed.
  void setVersion(uint8_t v) { version_ = v; }
  uint8_t version() const { return version_; }

 private:
  SnapshotConfig cfg_;
  uint16_t nextSeq_ = 1;  // seq 0 is reserved ("no baseline")
  uint32_t baselineSeq_ = 0;
  bool forceFull_ = false;
  uint8_t version_ = kProtocolVersion;  // stamped on send
  // Ring of sent snapshots, oldest front. Capped at historyDepth.
  std::deque<Snapshot> history_;
  // requestFull() rate limiter (audit LOW 2026-10-11).
  uint64_t buildCount_ = 0;            // build()/buildNormalized() calls so far
  uint64_t lastForcedFullBuild_ = 0;   // buildCount_ when requestFull() was last honored
  bool everForcedFull_ = false;
  uint64_t ignoredForcedFulls_ = 0;    // client requests dropped by the limiter

  const Snapshot* findSnapshot(uint32_t seq) const;
  void evictThrough(uint32_t seq);
};

/// Client-side snapshot receiver: sequencing, baseline tracking, interpolation.
class SnapshotReceiver {
 public:
  enum class Result {
    Dropped,       ///< duplicate, old, malformed, or wrong type/version
    Applied,       ///< decoded and buffered (full or delta)
    NeedBaseline,  ///< delta's baseline is unknown: set needFull, await a full
  };

  Result receive(const uint8_t* data, size_t len);

  /// Outgoing ack state for the client's heartbeat packets.
  uint16_t ackSeq() const { return highestSeq_; }
  uint16_t ackBits() const;
  /// True after NeedBaseline until a snapshot applies. Send kFlagNeedFull.
  bool needFull() const { return needFull_; }

  /// Latest applied snapshot (for HUD/debug; render via sample()).
  const Snapshot& latest() const { return interp_.back(); }
  bool hasData() const { return !interp_.empty(); }

  /// Render state at sim-tick `tick`, interpolated between buffered
  /// snapshots. Returns false when fewer than 2 snapshots are buffered.
  /// Clamps (holds) outside the buffer; never extrapolates.
  bool sample(uint64_t tick, Snapshot& out) const;

  /// Fiedler's recommended render delay, in snapshot intervals.
  static constexpr uint32_t kInterpDelay = 2;

  size_t historySize() const { return history_.size(); }

  /// Protocol versions accepted on receive. Before the handshake completes
  /// the game installs knownProtocolVersions(); after agreement it narrows
  /// the set to the single agreed version. A version outside the set is
  /// rejected (see lastReject()) — never silently dropped.
  void setAcceptedVersions(std::vector<uint8_t> v) { accepted_ = std::move(v); }
  const std::vector<uint8_t>& acceptedVersions() const { return accepted_; }
  /// Detail of the most recent header rejection (version mismatch).
  HeaderReject lastReject() const { return lastReject_; }

 private:
  uint16_t highestSeq_ = 0;
  bool haveSeq_ = false;
  bool needFull_ = false;
  std::vector<uint8_t> accepted_ = {kProtocolVersion};  // accepted on receive
  HeaderReject lastReject_;
  // Recently received seqs, for the selective-ack bitfield. Slot i records
  // the seq last seen with (seq & 63) == i.
  uint16_t seenSeq_[64] = {};
  // Decoded snapshots, oldest front, capped at 64 (baseline lookup).
  std::deque<Snapshot> history_;
  // Last 3 applied snapshots, oldest front (interpolation buffer).
  std::deque<Snapshot> interp_;

  const Snapshot* findSnapshot(uint32_t seq) const;
};

/// Pure fixed-point interpolation between two snapshots at `tick`
/// (a.tick <= tick <= b.tick; callers clamp). Entities present in both are
/// lerped with Q16 alpha in int64 (exact, deterministic across clients).
/// Entities only in b hold their spawn state; entities only in a (despawned)
/// are omitted. Orientation picks the newest (no slerp: future work).
void interpolateSnapshots(const Snapshot& a, const Snapshot& b, uint64_t tick, Snapshot& out);

}  // namespace net
}  // namespace litholattice
