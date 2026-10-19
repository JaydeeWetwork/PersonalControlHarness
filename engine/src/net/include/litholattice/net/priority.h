// Bandwidth budgeting: Veloren's PrioManager model.
//
// Every outbound stream (snapshots, chunk data, events, ...) registers with a
// priority and a GUARANTEED byte floor. Each snapshot slot, allocate() runs:
//
//   1. Every stream drains its guaranteed floor first (in stream-id order).
//   2. Leftover bytes are split evenly across the streams at each priority
//      level, highest priority first; a remainder of <groupSize bytes is
//      dealt one byte per stream in stream-id order.
//
// Deterministic: the same stream set and total always produce the same
// grants, in the same order, so server and client simulations of "what got
// sent" agree (matters for deterministic anti-cheat telemetry).
//
// INTEREST-MANAGEMENT HOOK: this is the prioritization half. The selection
// half (which entities/chunks are worth sending) is game policy: the game
// sorts entities by relevance (distance to the client's player,PVS, etc.)
// before handing them to SnapshotClient::build, whose byte budget then
// enforces the grant. Streams:
//
//   kStreamSnapshot = 1  Critical  the entity snapshot (never starve this)
//   kStreamChunk    = 2  High      chunk/block deltas (Luanti-style MapBlock
//                                  full sends + per-node deltas ride here)
//   kStreamEvent    = 3  Normal    gameplay events, chat, etc.
//
// Research basis: survey 6.5 (Veloren network crate: PrioManager, guaranteed
// bandwidth + priority-ordered leftover) and 6.7 (GameNetworkingSockets
// lanes: the industrial version of the same idea).
#pragma once

#include <cstdint>
#include <vector>

namespace litholattice {
namespace net {

enum class StreamPriority : uint8_t {
  Critical = 0,
  High = 1,
  Normal = 2,
  Low = 3,
  Background = 4,
};

namespace streams {
constexpr uint32_t kSnapshot = 1;
constexpr uint32_t kChunk = 2;
constexpr uint32_t kEvent = 3;
}  // namespace streams

struct StreamDef {
  uint32_t streamId = 0;
  StreamPriority priority = StreamPriority::Normal;
  uint32_t guaranteedBytes = 0;  // floor drained before any leftover is split
};

struct StreamGrant {
  uint32_t streamId = 0;
  uint32_t bytes = 0;

  bool operator==(const StreamGrant&) const = default;
};

/// Veloren-style per-slot bandwidth allocator. Not thread-safe by design
/// (owned by the net tick); all math is integer and deterministic.
class PrioManager {
 public:
  void clear() { streams_.clear(); }
  void addStream(StreamDef def) { streams_.push_back(def); }
  size_t streamCount() const { return streams_.size(); }

  /// Grants for one snapshot slot, sorted by streamId ascending.
  /// Sum of grants <= totalBytes. A stream's grant is always >= its
  /// guaranteed floor (floors are scaled down proportionally only if their
  /// sum exceeds totalBytes -- configure sanely instead).
  std::vector<StreamGrant> allocate(uint32_t totalBytes) const;

 private:
  std::vector<StreamDef> streams_;
};

}  // namespace net
}  // namespace litholattice
