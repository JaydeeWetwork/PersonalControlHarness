// Hierarchical pathfinding + flow fields (P4). See hpa.h for the design.
// Determinism: every search below uses integer fixed-point costs and
// insertion-order (FIFO) tie-breaking, exactly like findPath; lazy cache
// states change WHEN values are computed, never WHAT they are (intra-
// cluster costs are exact bounded-A* totals, refined legs are exact
// findPath results), so routes are history-independent.
#include "litholattice/ai/hpa.h"

#include <algorithm>
#include <cstdlib>
#include <queue>
#include <tuple>

namespace litholattice::ai {
namespace {

uint64_t colKey(int cx, int cz) { return (uint64_t(uint32_t(cx)) << 32) | uint32_t(cz); }
bool standable(const NavView& n, Cell c) { return (n.bits(c.x, c.y, c.z) & kNavStandable) != 0; }
bool passable(const NavView& n, Cell c) { return (n.bits(c.x, c.y, c.z) & kNavPassable) != 0; }

// Exact Walk legality for one axis-aligned step a -> b (mirrors
// groundMoves in nav.cpp: flat needs standable + headroom, step-up needs
// the mover's head cell too, drops need a passable shaft).
bool stepLegal(const NavView& n, Cell a, Cell b) {
  if (!standable(n, b)) return false;
  const int dy = b.y - a.y;
  if (dy == 0) return passable(n, {b.x, b.y + 1, b.z});
  if (dy == 1) return passable(n, {b.x, b.y + 1, b.z}) && passable(n, {a.x, a.y + 2, a.z});
  if (dy < 0 && dy >= -3) {
    if (!passable(n, {b.x, a.y, b.z}) || !passable(n, {b.x, a.y + 1, b.z})) return false;
    for (int y = a.y - 1; y > b.y; --y)
      if (!passable(n, {b.x, y, b.z})) return false;
    return true;
  }
  return false;
}

NavCost stepCostFor(int dy) {  // axis steps only (portals never diagonal)
  if (dy == 1) return 1536;
  if (dy < 0) return NavCost(1024 + 512 * (-dy));
  return 1024;
}

// All crossings of one border, grouped into entrance runs. `lx, lz` is
// the CANONICAL cluster (west for X borders, north for Z borders); the
// neighbour across the border is +1 in the axis. Crossings are collected
// lane-major, height-minor; consecutive crossings whose lanes are
// adjacent (or equal) and whose canonical-side heights differ by <= 1
// form a run; each run emits one portal at its middle crossing. Both
// adjacent clusters compute this identically (grouping keys on the
// canonical side only), so their portal sets always agree.
void scanBorder(const NavView& nav, bool axisX, int lx, int lz, std::vector<HpaPortal>& out) {
  struct Cross { Cell l, r; NavCost step; bool revOk; NavCost revStep; int lane; };
  std::vector<Cross> all;
  for (int lane = 0; lane < kChunkSize; ++lane) {
    for (int y = 0; y < kWorldHeight; ++y) {
      const Cell l = axisX ? Cell{lx * kChunkSize + kChunkSize - 1, y, lz * kChunkSize + lane}
                           : Cell{lx * kChunkSize + lane, y, lz * kChunkSize + kChunkSize - 1};
      if (!standable(nav, l)) continue;
      int bestDy = 99, bestY = -1;
      for (int dy = -3; dy <= 1; ++dy) {
        const int ry = y + dy;
        if (ry < 0 || ry >= kWorldHeight) continue;
        const Cell r = axisX ? Cell{(lx + 1) * kChunkSize, ry, l.z} : Cell{l.x, ry, (lz + 1) * kChunkSize};
        if (stepLegal(nav, l, r) && std::abs(dy) < std::abs(bestDy)) { bestDy = dy; bestY = ry; }
      }
      if (bestY >= 0) {
        const Cell r = axisX ? Cell{(lx + 1) * kChunkSize, bestY, l.z} : Cell{l.x, bestY, (lz + 1) * kChunkSize};
        // B-C2: the reverse step is validated INDEPENDENTLY, in its own
        // direction, and carries its own cost. A legal drop l -> r does
        // not imply a legal climb r -> l.
        const bool revOk = stepLegal(nav, r, l);
        all.push_back({l, r, stepCostFor(bestDy), revOk, revOk ? stepCostFor(-bestDy) : NavCost(0), lane});
      }
    }
  }
  size_t i = 0;
  while (i < all.size()) {
    size_t j = i + 1;
    while (j < all.size() && all[j].lane - all[j - 1].lane <= 1 &&
           std::abs(all[j].l.y - all[j - 1].l.y) <= 1)
      ++j;
    const Cross& mid = all[i + (j - i) / 2];
    out.push_back({mid.l, mid.r, mid.step, mid.revOk, mid.revStep});
    i = j;
  }
}

}  // namespace

// ---------------------------------------------------------------- FlowField

bool FlowField::build(const WalkCache& cache, Cell goal, int maxRange, size_t maxCells) {
  cache_ = &cache;
  goal_ = goal;
  dist_.clear();
  if (!walk_) walk_ = makeWalk();
  NavView nav(cache);
  if (!standable(nav, goal)) return false;
  struct Item { NavCost d; uint64_t seq; Cell c; };
  struct Cmp {
    bool operator()(const Item& a, const Item& b) const {
      return a.d != b.d ? a.d > b.d : a.seq > b.seq;
    }
  };
  std::priority_queue<Item, std::vector<Item>, Cmp> pq;
  uint64_t seq = 0;
  dist_.emplace(goal, 0);
  pq.push({0, seq++, goal});
  std::vector<NavStep> nb;
  while (!pq.empty()) {
    const Item cur = pq.top();
    pq.pop();
    const auto it = dist_.find(cur.c);
    if (it == dist_.end() || it->second < cur.d) continue;
    if (dist_.size() >= maxCells) break;  // budget spent: keep the reached prefix
    // Predecessor expansion: walk edges are not symmetric (a drop down is
    // not a climb up), so the field grows over REVERSED edges -- a cell u
    // joins with dist(u) = stepCost(u -> cur) + dist(cur) exactly when the
    // Walk mode can step from u into cur. Candidates: the 3x3x5 shell
    // around cur (same level, 1 below for step-ups, up to 3 above for
    // drops, plus climb... no: Walk semantics only).
    for (int dy = -1; dy <= 3; ++dy)
      for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -1; dz <= 1; ++dz) {
          if (dx == 0 && dy == 0 && dz == 0) continue;
          const Cell u{cur.c.x + dx, cur.c.y + dy, cur.c.z + dz};
          if (std::abs(u.x - goal.x) > maxRange || std::abs(u.z - goal.z) > maxRange ||
              std::abs(u.y - goal.y) > maxRange)
            continue;
          if (!standable(nav, u)) continue;
          nb.clear();
          walk_->neighbours(nav, u, nb);
          for (const NavStep& s : nb) {
            if (!(s.to == cur.c)) continue;
            const NavCost nd = cur.d + s.cost;
            const auto jt = dist_.find(u);
            if (jt == dist_.end() || nd < jt->second) {
              dist_[u] = nd;
              pq.push({nd, seq++, u});
            }
          }
        }
  }
  return true;
}

NavCost FlowField::dist(Cell c) const {
  const auto it = dist_.find(c);
  return it == dist_.end() ? kUnreached : it->second;
}

std::optional<Cell> FlowField::next(Cell from) const {
  if (!cache_ || !walk_) return std::nullopt;
  const NavCost d0 = dist(from);
  if (d0 == kUnreached || d0 == 0) return std::nullopt;
  NavView nav(*cache_);
  std::vector<NavStep> nb;
  walk_->neighbours(nav, from, nb);
  std::optional<Cell> best;
  NavCost bestD = d0;
  for (const NavStep& s : nb) {
    const NavCost d = dist(s.to);
    if (d < bestD) { bestD = d; best = s.to; }  // strict <: first in emission order wins ties
  }
  return best;
}

// ---------------------------------------------------------------- HpaGraph

const HpaGraph::Cluster& HpaGraph::ensure(int cx, int cz) {
  const uint64_t key = colKey(cx, cz);
  const std::array<uint64_t, 5> vers = {cache_.columnVersion(cx, cz), cache_.columnVersion(cx + 1, cz),
                                        cache_.columnVersion(cx - 1, cz), cache_.columnVersion(cx, cz + 1),
                                        cache_.columnVersion(cx, cz - 1)};
  if (auto it = clusters_.find(key); it != clusters_.end() && it->second.vers == vers) return it->second;
  Cluster cl;
  cl.vers = vers;
  {
    NavView nav(cache_);
    scanBorder(nav, true, cx, cz, cl.portals);        // east (this cluster is canonical/west)
    scanBorder(nav, true, cx - 1, cz, cl.portals);    // west (neighbour canonical)
    scanBorder(nav, false, cx, cz, cl.portals);       // south (this cluster canonical/north)
    scanBorder(nav, false, cx, cz - 1, cl.portals);   // north (neighbour canonical)
  }
  for (const HpaPortal& p : cl.portals) {
    const Cell local = (p.a.x >> 4 == cx && p.a.z >> 4 == cz) ? p.a : p.b;
    if (std::find(cl.nodes.begin(), cl.nodes.end(), local) == cl.nodes.end()) cl.nodes.push_back(local);
  }
  if (!walk_) walk_ = makeWalk();
  const int n = int(cl.nodes.size());
  cl.adj.assign(size_t(n), {});
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j) {
      if (i == j) continue;
      const Path leg = ::litholattice::ai::findPath(cache_, {cl.nodes[size_t(i)], cl.nodes[size_t(j)], walk_.get(), 20, 8000});
      if (leg.complete) cl.adj[size_t(i)].push_back({j, leg.totalCost});
    }
  auto [it, ins] = clusters_.insert_or_assign(key, std::move(cl));
  return it->second;
}

Path HpaGraph::refineLeg(Cell a, Cell b, const IMovementMode* mode, int maxNodes, bool useCache) {
  if (a == b) {
    Path p;
    p.complete = true;
    p.cells = {a};
    p.steps = {StepTag::kStepStart};
    return p;
  }
  if (!useCache) return ::litholattice::ai::findPath(cache_, {a, b, mode, 24, maxNodes});
  const LegKey key{a, b, mode};
  if (auto it = legCache_.find(key); it != legCache_.end()) {
    ++portalHits_;
    return it->second;
  }
  ++portalMisses_;
  Path p = ::litholattice::ai::findPath(cache_, {a, b, mode, 24, maxNodes});
  if (p.complete) {
    if (legCache_.size() >= kLegCacheCap && !legOrder_.empty()) {
      legCache_.erase(legOrder_.front());
      legOrder_.erase(legOrder_.begin());
    }
    legCache_.emplace(key, p);
    legOrder_.push_back(key);
  }
  return p;
}

std::optional<Path> HpaGraph::findPath(Cell from, Cell to, const IMovementMode* mode, int maxNodesPerLeg) {
  if (!mode) return std::nullopt;
  // B-C1: the portal-pair leg cache is keyed by cells + mode only, so any
  // world edit since it was filled can make a cached leg stale (a wall
  // built across its corridor). The WalkCache generation bumps on EVERY
  // invalidate(); when it has moved, drop the whole cache before serving
  // anything from it. Epoch-clear chosen over key-augmentation: simpler,
  // obviously correct, and the cache is a pure speed device -- refilling
  // it costs searches, never semantics.
  if (const uint64_t gen = cache_.generation(); gen != legEpoch_) {
    legCache_.clear();
    legOrder_.clear();
    legEpoch_ = gen;
  }
  const int fx = from.x >> 4, fz = from.z >> 4, tx = to.x >> 4, tz = to.z >> 4;
  ensure(fx, fz);
  ensure(tx, tz);
  // A* over states (cluster, node) where node -1 = the query point in its
  // own cluster (from; the terminal `to` state uses node -2 in its own).
  struct State {
    int cx, cz, idx;
    bool operator==(const State& o) const { return cx == o.cx && cz == o.cz && idx == o.idx; }
  };
  struct StateHash {
    size_t operator()(const State& s) const { return CellHash{}(Cell{s.cx, s.idx, s.cz}); }
  };
  const State start{fx, fz, -1}, goal{tx, tz, -2};
  struct QItem { NavCost f, g; uint64_t seq; State state; };
  struct Cmp {
    bool operator()(const QItem& a, const QItem& b) const {
      return a.f != b.f ? a.f > b.f : a.seq > b.seq;
    }
  };
  // B-H2: same admissible form as nav.cpp's heurFp (octile-exact
  // horizontal + bundled vertical at 512/level; see the long note there).
  // Abstract edges are Walk legs and directed portal steps, whose costs
  // this stays under step-for-step.
  auto heur = [&](Cell c) {
    const uint32_t dx = uint32_t(std::abs(c.x - to.x)), dz = uint32_t(std::abs(c.z - to.z)),
                   dy = uint32_t(std::abs(c.y - to.y));
    const uint32_t mx = dx > dz ? dx : dz, mn = dx > dz ? dz : dx;
    return NavCost(mx * 1024u + mn * 424u + dy * 512u);
  };
  std::unordered_map<State, NavCost, StateHash> gScore;
  std::unordered_map<State, State, StateHash> parent;
  std::unordered_map<State, Cell, StateHash> cellOf;
  std::priority_queue<QItem, std::vector<QItem>, Cmp> open;
  uint64_t seq = 0;
  gScore[start] = 0;
  cellOf[start] = from;
  cellOf[goal] = to;
  open.push({heur(from), 0, seq++, start});
  bool found = false;
  while (!open.empty()) {
    const QItem cur = open.top();
    open.pop();
    if (cur.state == goal) { found = true; break; }
    if (auto it = gScore.find(cur.state); it == gScore.end() || it->second < cur.g) continue;
    const int ccx = cur.state.cx, ccz = cur.state.cz, idx = cur.state.idx;
    const Cell cell = cellOf[cur.state];
    const Cluster& cl = ensure(ccx, ccz);
    auto relax = [&](State ns, Cell nc, NavCost w) {
      const NavCost ng = cur.g + w;
      if (auto it = gScore.find(ns); it == gScore.end() || ng < it->second) {
        gScore[ns] = ng;
        parent[ns] = cur.state;
        cellOf[ns] = nc;
        open.push({ng + heur(nc), ng, seq++, ns});
      }
    };
    if (idx == -1) {
      // From the query point: legs to every entrance of its own cluster,
      // plus the direct leg when the goal shares the cluster.
      for (int j = 0; j < int(cl.nodes.size()); ++j) {
        const Path leg = refineLeg(from, cl.nodes[size_t(j)], mode, maxNodesPerLeg, false);
        if (leg.complete) relax({ccx, ccz, j}, cl.nodes[size_t(j)], leg.totalCost);
      }
      if (ccx == tx && ccz == tz) {
        const Path leg = refineLeg(from, to, mode, maxNodesPerLeg, false);
        if (leg.complete) relax(goal, to, leg.totalCost);
      }
    } else {
      for (const auto& [j, w] : cl.adj[size_t(idx)])
        relax({ccx, ccz, j}, cl.nodes[size_t(j)], w);
      for (const HpaPortal& p : cl.portals) {
        if (p.a == cell) {  // forward: always legal (validated at scan time)
          const Cluster& oc = ensure(p.b.x >> 4, p.b.z >> 4);
          const auto jt = std::find(oc.nodes.begin(), oc.nodes.end(), p.b);
          if (jt != oc.nodes.end())
            relax({p.b.x >> 4, p.b.z >> 4, int(jt - oc.nodes.begin())}, p.b, p.step);
        } else if (p.b == cell && p.revOk) {  // B-C2: reverse only if independently legal, at its own cost
          const Cluster& oc = ensure(p.a.x >> 4, p.a.z >> 4);
          const auto jt = std::find(oc.nodes.begin(), oc.nodes.end(), p.a);
          if (jt != oc.nodes.end())
            relax({p.a.x >> 4, p.a.z >> 4, int(jt - oc.nodes.begin())}, p.a, p.revStep);
        }
      }
      if (ccx == tx && ccz == tz) {
        const Path leg = refineLeg(cell, to, mode, maxNodesPerLeg, false);
        if (leg.complete) relax(goal, to, leg.totalCost);
      }
    }
  }
  if (!found) return std::nullopt;
  // Waypoints: the state chain's cells, from..to.
  std::vector<Cell> wps;
  for (State s = goal;; s = parent[s]) {
    wps.push_back(cellOf[s]);
    if (s == start) break;
  }
  std::reverse(wps.begin(), wps.end());
  // Portal lookup for single-step stitches. B-C2: direction is part of
  // the match -- a u -> v stitch is served only by a portal whose stored
  // direction (forward, or validated reverse) is exactly u -> v, priced
  // at that direction's own cost.
  auto portalStep = [&](Cell u, Cell v, NavCost& w, StepTag& tag) {
    for (int cx : {u.x >> 4, v.x >> 4})
      for (int cz : {u.z >> 4, v.z >> 4}) {
        const Cluster& c = ensure(cx, cz);
        for (const HpaPortal& p : c.portals) {
          NavCost pw;
          if (p.a == u && p.b == v) pw = p.step;
          else if (p.b == u && p.a == v && p.revOk) pw = p.revStep;
          else continue;
          w = pw;
          const int dy = v.y - u.y;
          tag = dy > 0 ? StepTag::kStep : dy < 0 ? StepTag::kDrop : StepTag::kWalk;
          return true;
        }
      }
    return false;
  };
  Path out;
  out.complete = true;
  out.cells.push_back(wps.front());
  out.steps.push_back(StepTag::kStepStart);
  NavCost total = 0;
  for (size_t i = 1; i < wps.size(); ++i) {
    const Cell u = wps[i - 1], v = wps[i];
    NavCost w;
    StepTag tag;
    if (portalStep(u, v, w, tag)) {
      out.cells.push_back(v);
      out.steps.push_back(tag);
      total += w;
      continue;
    }
    const bool cacheLeg = !(u == from) && !(v == to);
    const Path leg = refineLeg(u, v, mode, maxNodesPerLeg, cacheLeg);
    if (!leg.complete || leg.cells.empty() || !(leg.cells.front() == u)) return std::nullopt;
    for (size_t k = 1; k < leg.cells.size(); ++k) {
      out.cells.push_back(leg.cells[k]);
      out.steps.push_back(leg.steps[k]);
    }
    total += leg.totalCost;
  }
  out.totalCost = total;
  return out;
}

}  // namespace litholattice::ai
