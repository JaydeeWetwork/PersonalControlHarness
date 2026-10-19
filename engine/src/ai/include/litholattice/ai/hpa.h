// Hierarchical pathfinding + flow fields (P4, 2026-10-14), layered over the
// WalkCache without changing it. Same determinism rules as findPath:
// integer fixed-point costs, FIFO tie-breaks, node budgets -- never
// wall-clock. Everything here is derived data, rebuilt lazily and repaired
// from the WalkCache's per-column version counters; nothing is persisted.
//
// Three pieces:
//  - FlowField: a Dijkstra integration field over Walk-semantics steps.
//    Built once per goal, followed by any number of mobs -- the
//    pack/crowd-movement primitive (Deceitor pack aggro is the consumer
//    class this serves).
//  - HpaGraph: HPA*-lite. Clusters are chunk columns; border crossings are
//    grouped into entrance portals; intra-cluster entrance-to-entrance
//    costs are exact bounded-A* costs, cached per cluster and repaired
//    when a column version moves. A query runs A* over the entrance
//    graph and refines the winning route into a mode-exact cell path.
//  - The portal-pair path cache inside HpaGraph: refined entrance-to-
//    entrance legs are cached (bounded LRU) with hit/miss stats, so a
//    pack crossing the same doorway twice pays the search once.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "litholattice/ai/nav.h"

namespace litholattice::ai {

/// Dijkstra integration field toward one goal, over the exact neighbour
/// edges of the Walk movement mode (so field descent is always a legal
/// walk). Costs are the steps' own fixed-point costs.
class FlowField {
 public:
  static constexpr NavCost kUnreached = ~NavCost(0);

  /// Build over `cache` for standable cells within Chebyshev `maxRange`
  /// of `goal`, visiting at most `maxCells` cells (budget, not clock).
  /// Returns false when the goal cell itself is not standable.
  bool build(const WalkCache& cache, Cell goal, int maxRange = 48, size_t maxCells = 65536);
  Cell goal() const { return goal_; }
  size_t size() const { return dist_.size(); }
  /// Exact field distance of a cell (kUnreached when not in the field).
  NavCost dist(Cell c) const;
  /// The neighbouring cell (a legal Walk step from `from`) with the
  /// lowest field distance, strictly below dist(from). Nullopt at the
  /// goal, outside the field, or on a (theoretically impossible) local
  /// minimum. Deterministic: the mode's fixed emission order breaks ties.
  std::optional<Cell> next(Cell from) const;

 private:
  const WalkCache* cache_ = nullptr;
  std::unique_ptr<IMovementMode> walk_;
  Cell goal_{};
  std::unordered_map<Cell, NavCost, CellHash> dist_;
};

/// One border crossing between adjacent clusters: `a` is the entrance
/// cell in the canonically-first cluster (west for X borders, north for
/// Z borders), `b` its pair across the border, `step` the exact Walk
/// step cost from `a` to `b`. Portals are DIRECTED (B-C2 fix,
/// 2026-10-15): the reverse traversal `b` -> `a` exists only when that
/// step is itself legal (a 3-block drop down is not a 3-block climb up),
/// recorded in `revOk` / `revStep` (the reverse step's own cost -- drops
/// and step-ups price differently, so it is never derived from `step`).
struct HpaPortal {
  Cell a, b;
  NavCost step = 0;
  bool revOk = false;
  NavCost revStep = 0;
};

class HpaGraph {
 public:
  explicit HpaGraph(const WalkCache& cache) : cache_(cache) {}

  /// A full, refined, mode-exact path from `from` to `to`, routed over
  /// the entrance graph; nullopt when no route exists (callers fall back
  /// to a bounded findPath). Legs are searched with maxRange 24 and
  /// `maxNodesPerLeg` expansions each (JD-review-pending budget).
  std::optional<Path> findPath(Cell from, Cell to, const IMovementMode* mode,
                               int maxNodesPerLeg = 8000);

  size_t clustersBuilt() const { return clusters_.size(); }
  size_t portalCacheSize() const { return legCache_.size(); }
  size_t portalHits() const { return portalHits_; }
  size_t portalMisses() const { return portalMisses_; }

 private:
  struct Cluster {
    std::array<uint64_t, 5> vers{};  // versions of self + E/W/N/S neighbours at build time
    std::vector<Cell> nodes;         // entrance cells inside this cluster
    std::vector<HpaPortal> portals;  // every crossing touching this cluster
    std::vector<std::vector<std::pair<int, NavCost>>> adj;  // intra-cluster edges over nodes
  };
  const Cluster& ensure(int cx, int cz);
  Path refineLeg(Cell a, Cell b, const IMovementMode* mode, int maxNodes, bool useCache);

  const WalkCache& cache_;
  std::unique_ptr<IMovementMode> walk_;  // graph costs are Walk-costed; refinement is mode-exact
  uint64_t legEpoch_ = 0;  // B-C1: WalkCache generation the leg cache was built under;
                           // a mismatch at findPath entry clears legCache_/legOrder_
  std::unordered_map<uint64_t, Cluster> clusters_;
  struct LegKey {
    Cell a, b;
    const IMovementMode* mode;
    bool operator==(const LegKey& o) const { return a == o.a && b == o.b && mode == o.mode; }
  };
  struct LegKeyHash {
    size_t operator()(const LegKey& k) const {
      uint64_t h = CellHash{}(k.a) * 0x9e3779b97f4a7c15ull;
      h ^= CellHash{}(k.b) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      h ^= std::hash<const void*>{}(k.mode);
      return size_t(h);
    }
  };
  static constexpr size_t kLegCacheCap = 256;  // JD-review-pending
  std::unordered_map<LegKey, Path, LegKeyHash> legCache_;
  std::vector<LegKey> legOrder_;  // FIFO eviction order for the leg cache
  size_t portalHits_ = 0, portalMisses_ = 0;
};

}  // namespace litholattice::ai
