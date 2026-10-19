// Voxel navigation: a per-chunk walkability cache, movement-mode plug-ins and bounded A*.
// Pure functions of the world snapshot, so results are deterministic on any thread.
//
// COSTS (P1, 2026-10-14): all path costs are FIXED-POINT integers (NavCost,
// 1024 == one walk step). The pre-P1 float costs were reproducible only
// incidentally (IEEE-754 + fixed op order); integer sums make determinism
// structural. Edge-cost mapping preserves the old float values exactly where
// they were binary-exact (walk 1024, step-up 1536, drop 1024 + 512/drop,
// climb 2048); swim's 1.2 rounds to 1229.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "litholattice/world/block.h"
#include "litholattice/world/chunk_map.h"

namespace litholattice::ai {

struct Cell {
  int x = 0, y = 0, z = 0;
  bool operator==(const Cell&) const = default;
};

/// Splitmix-style cell hash for unordered maps keyed by Cell (P4).
/// One shared definition so every nav container hashes identically;
/// results never depend on it (no decision iterates these maps).
struct CellHash {
  size_t operator()(const Cell& c) const {
    uint64_t h = uint64_t(uint32_t(c.x)) * 0x9e3779b97f4a7c15ull;
    h ^= uint64_t(uint32_t(c.y)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= uint64_t(uint32_t(c.z)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return size_t(h);
  }
};

/// Game-supplied block classification (engine has no content). Defaults use BlockFlags only.
struct NavRules {
  std::function<bool(BlockId)> solid;      ///< blocks movement (default: kSolid)
  std::function<bool(BlockId)> climbable;  ///< ladders / vines (default: none)
  std::function<bool(BlockId)> fluid;      ///< swimmable (default: none)
  std::function<bool(BlockId)> track;      ///< wire-follow mobs walk on these (default: none)
};

/// Per-cell nav bits, cached per chunk column and invalidated on edit.
enum NavBits : uint8_t { kNavPassable = 1, kNavStandable = 2, kNavClimb = 4, kNavFluid = 8, kNavTrack = 16 };

/// Fixed-point path cost: 1024 == 1.0 (one flat walk step).
using NavCost = uint32_t;
constexpr NavCost kNavCostUnit = 1024;

/// How one path step is travelled, parallel to Path::cells (steps[i] is the
/// edge that ARRIVES at cells[i]; steps[0] is kStepStart). Presentation and
/// execution layers (P2 interpolation) key off these; the sim treats every
/// step as one cell of travel regardless of tag.
enum class StepTag : uint8_t {
  kStepStart, kWalk, kStep, kJump, kGap, kClimb, kDrop, kFly, kSwim, kTrack
};

/// One legal move out of a cell, as emitted by a movement mode.
struct NavStep {
  Cell to;
  NavCost cost = kNavCostUnit;  ///< >= 1 fixed-point units
  StepTag tag = StepTag::kWalk;
};

class WalkCache {
 public:
  /// P3 memory (2026-10-14): columns are 5 bit-planes (120 KiB) instead of
  /// a byte array (192 KiB), a power-of-two-friendly LRU cap bounds how many
  /// columns are kept (eviction is recomputation only -- derived data, never
  /// persisted), and each column carries a standable-height span sidecar.
  /// `maxColumns` default 512 is JD-review-pending (0 = unbounded, the old
  /// behaviour). Memory per cached column ≈ 120 KiB planes + ~1.6 KiB
  /// struct + ~2 B per standable cell of span data.
  WalkCache(const ChunkMap& world, const BlockRegistry& blocks, NavRules rules,
            size_t maxColumns = 512);
  /// Bits of a cell (0 outside loaded columns / the world).
  /// Thread contract (B-H3 discipline, 2026-10-15): the cache structure
  /// itself is thread-safe, but computing an uncached column reads the
  /// ChunkMap, which only the caller's designated reader thread may do
  /// (in production the AiWorld tick thread, at the AI-tick boundary --
  /// the same boundary at which invalidations are drained). A
  /// worker-thread first-compute is a contract VIOLATION, not a benign
  /// stretch: it reads live ChunkMap storage concurrently with the
  /// mutating owner thread, which is a C++ data race (UB). The
  /// discipline that upholds this: whoever submits searches to a pool
  /// must first pre-warm, on the reader thread, every column the search
  /// can touch. AiWorld::tick does exactly that for its plan jobs (the
  /// full planRange box around each pending search, plus a one-column
  /// margin for movement-mode overshoot) before any job is submitted,
  /// so pool workers only ever read already-cached, immutable columns.
  /// Direct pool consumers of findPath must do the same.
  uint8_t bits(int x, int y, int z) const;
  /// Compute and cache the column on the calling thread. Intended for the
  /// reader thread to pre-warm columns before worker-thread pathfinding
  /// (see the thread contract on bits()).
  void prewarm(int cx, int cz) const { column(cx, cz); }
  /// Drop the cached column holding block column (x, z) (call after edits there).
  /// Bumps the generation so in-flight computes can't reinsert stale columns.
  void invalidate(int x, int z);
  size_t cachedColumns() const;
  /// P4: edit epoch. generation() bumps on EVERY invalidate();
  /// columnVersion() is the epoch of one column (0 = never invalidated).
  /// Derived-data bookkeeping for hierarchical caches (HpaGraph) -- the
  /// same contract as invalidate(): every edit path must invalidate.
  uint64_t generation() const;
  uint64_t columnVersion(int cx, int cz) const;
  /// P3: the LRU cap (see ctor); approximate total bytes held by cached
  /// columns (bit-planes + span sidecar + per-column struct).
  size_t maxColumns() const { return maxColumns_; }
  size_t cachedBytes() const;
  /// P3 surface spans: the standable heights at block (x, z), TOP-FIRST.
  /// Returned by value (short copies) so callers never hold the cache.
  std::vector<int16_t> standableSpans(int x, int z) const;
  /// Topmost standable height at (x, z), or -1 when nothing is standable.
  int topStandable(int x, int z) const;

 private:
  friend class NavView;
  struct Column {
    // Bit-planes: one bit per cell per NavBits flag (5 planes, plane p
    // holds bit 1<<p) = 5 * 24 KiB = 120 KiB per column.
    std::vector<uint64_t> words;
    // Standable-height spans: for each (z,x) cell-column, its standable
    // heights top-first, packed into one flat run indexed by spanOff/spanCnt.
    std::vector<int16_t> spanData;
    std::array<uint32_t, kChunkSize * kChunkSize> spanOff{};
    std::array<uint16_t, kChunkSize * kChunkSize> spanCnt{};
    mutable uint64_t lastUsed = 0;  ///< LRU clock; only touched under the cache mutex
  };
  static constexpr size_t kWordsPerPlane = size_t(kChunkSize) * kChunkSize * kWorldHeight / 64;
  static size_t cellIndex(int lx, int y, int lz) {
    return (size_t(y) * kChunkSize + size_t(lz)) * kChunkSize + size_t(lx);
  }
  /// Reassemble one cell's NavBits byte from the planes.
  static uint8_t assemble(const Column& c, size_t cellIdx) {
    const size_t w = cellIdx >> 6;
    const uint64_t b = uint64_t(1) << (cellIdx & 63);
    const uint64_t* W = c.words.data();
    uint8_t out = 0;
    if (W[w] & b) out |= kNavPassable;
    if (W[kWordsPerPlane + w] & b) out |= kNavStandable;
    if (W[2 * kWordsPerPlane + w] & b) out |= kNavClimb;
    if (W[3 * kWordsPerPlane + w] & b) out |= kNavFluid;
    if (W[4 * kWordsPerPlane + w] & b) out |= kNavTrack;
    return out;
  }
  std::shared_ptr<const Column> column(int cx, int cz) const;
  uint8_t compute(int x, int y, int z) const;
  const ChunkMap& world_;
  const BlockRegistry& blocks_;
  NavRules rules_;
  const size_t maxColumns_;
  mutable std::mutex m_;
  mutable std::unordered_map<uint64_t, std::shared_ptr<const Column>> cols_;
  mutable uint64_t generation_ = 0;  ///< bumped by invalidate(); poisons in-flight computes
  mutable uint64_t useClock_ = 0;    ///< monotone LRU clock, bumped under m_
  mutable std::unordered_map<uint64_t, uint64_t> colVer_;  ///< per-column edit epoch (P4)
};

/// One search's view of a WalkCache: keeps the columns it touched alive and skips the cache
/// lock on repeat lookups (single-threaded; make one per search).
class NavView {
 public:
  explicit NavView(const WalkCache& cache) : cache_(cache) {}
  uint8_t bits(int x, int y, int z) const;
 private:
  const WalkCache& cache_;
  mutable uint64_t lastKey_ = ~0ull;
  mutable const WalkCache::Column* lastCol_ = nullptr;
  mutable std::unordered_map<uint64_t, std::shared_ptr<const WalkCache::Column>> held_;
};

/// A movement mode plug-in: which neighbour steps are legal and what they cost.
class IMovementMode {
 public:
  virtual ~IMovementMode() = default;
  virtual std::string_view name() const = 0;
  /// Appends legal steps from `c` (fixed-point costs, each >= 1 unit).
  virtual void neighbours(const NavView& nav, Cell c, std::vector<NavStep>& out) const = 0;
};

/// Built-in modes: walk (step up 1, drop up to 3, diagonals with a corner-cut
/// guard), climb (walk + climbables), leap (walk + climb + jump-up 2 + gap
/// jumps 2-4 + long drops 4-8), fly (any passable cell), swim (fluid cells +
/// walk), wire-follow (only standable cells over track blocks).
std::unique_ptr<IMovementMode> makeWalk(int maxDrop = 3);
std::unique_ptr<IMovementMode> makeClimb();
std::unique_ptr<IMovementMode> makeLeap();
std::unique_ptr<IMovementMode> makeFly();
std::unique_ptr<IMovementMode> makeSwim();
std::unique_ptr<IMovementMode> makeWireFollow();

struct PathRequest {
  Cell from, to;
  const IMovementMode* mode = nullptr;
  int maxRange = 64;      ///< a plan never leaves the box of this half-size around `from`
  int maxNodes = 20000;   ///< expansion budget
  bool smooth = false;    ///< opt-in: string-pull the result (see smoothPath)
};
struct Path {
  std::vector<Cell> cells;      ///< from .. end (end == goal when `complete`)
  std::vector<StepTag> steps;   ///< parallel to cells; steps[0] == kStepStart
  bool complete = false;        ///< false: a partial hop toward the goal (replan from its end)
  int expanded = 0;
  NavCost totalCost = 0;        ///< exact fixed-point cost of the returned steps (P4: HPA edge costs)
};
/// Bounded A* (deterministic tie-breaking). If the goal is outside maxRange or unreachable in
/// budget, returns the explored cell closest to the goal as a hop.
Path findPath(const WalkCache& nav, const PathRequest& req);
/// Greedy string-pull over a found path: on flat (single-y) legs, drop every
/// intermediate cell the mover can walk past in a straight line (each crossed
/// cell standable with headroom). Conservative: legs that change level are
/// kept verbatim, so a smoothed path is always a subsequence of legal travel.
/// Tags on kept cells are preserved; merged legs are tagged kWalk.
Path smoothPath(const WalkCache& nav, const Path& in);
/// The intermediate target for a long trip: the goal clamped to `hop` blocks from `from`.
Cell hopToward(Cell from, Cell goal, int hop = 48);

}  // namespace litholattice::ai
