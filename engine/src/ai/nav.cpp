#include "litholattice/ai/nav.h"
#include <algorithm>
#include <cmath>
#include <queue>

namespace litholattice::ai {
namespace {
uint64_t colKey(int cx, int cz) { return (uint64_t(uint32_t(cx)) << 32) | uint32_t(cz); }
uint64_t cellKey(Cell c) { return (uint64_t(uint32_t(c.x) & 0x3fffff) << 42) | (uint64_t(uint32_t(c.y) & 0xfffff) << 22) | (uint32_t(c.z) & 0x3fffff); }
/// Admissible A* heuristic in fixed point (B-H2 fix, 2026-10-15): octile
/// horizontal + a bundled vertical term,
///   h = 1024 * max(dx,dz) + 424 * min(dx,dz) + 512 * |dy|.
/// Why these coefficients (deviations from the review's draft, documented):
///  - Diagonal marginal is 424, not 448: kCostDiag - kCostWalk =
///    1448 - 1024 = 424. The draft's 448 would price a (1,1) step at 1472
///    against its true cost 1448 -- still inadmissible, by 24 per diagonal.
///    With 424 the horizontal term is EXACT for flat Walk travel (tight on
///    both orthogonal and diagonal ground moves).
///  - Vertical is 512 per level, not 1024: a horizontal edge BUNDLES
///    vertical travel -- step-up costs 1536 = 1024 (across) + 512 (up), a
///    drop costs 1024/1448 + 512 per level fallen. Pricing |dy| at 1024 on
///    top of the horizontal term double-counts the bundle: the cell one
///    across and one up is priced 2048 against a true cost of 1536. At 512
///    the heuristic is tight on step-up and on drops, and stays under
///    climb (2048), fly vertical (1024 pure, but fly cannot bundle, so no
///    edge beats the bound), swim (1229), wire slope (2048), Leap jump-up-2
///    (2560/3072 for a bundled +2) and Leap long drops (base + 512/level,
///    tight for the fluid-waived diagonal case). I.e. for every edge of
///    every mode except the one below, cost(edge) >= the heuristic drop
///    across that edge (consistency), so first-goal-pop is optimal.
///  - Known residual: Leap DIAGONAL gap jumps price a (d,d) displacement
///    with the orthogonal gap formula (1024*d + 512*(d-1)), undercutting an
///    octile-tight heuristic by up to ~248 (plus 512 if the landing is a
///    level down) -- the diagonal-premium inconsistency already filed as
///    B-L1 in the edge costs themselves. A* over Leap can therefore still
///    be marginally suboptimal across a diagonal gap; Walk-family, HPA and
///    the optimality fixture are exact. Closing it belongs with the B-L1
///    cost-model fix, not with a weaker heuristic for every other mode.
NavCost heurFp(Cell a, Cell b) {
  const uint32_t dx = uint32_t(std::abs(a.x - b.x));
  const uint32_t dz = uint32_t(std::abs(a.z - b.z));
  const uint32_t dy = uint32_t(std::abs(a.y - b.y));
  const uint32_t mx = dx > dz ? dx : dz, mn = dx > dz ? dz : dx;
  return mx * kNavCostUnit + mn * 424u + dy * 512u;
}
constexpr int kH[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
constexpr int kD[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
constexpr NavCost kCostWalk = 1024;    // 1.0
constexpr NavCost kCostStep = 1536;    // 1.5 (flat + 512 step premium)
constexpr NavCost kCostDiag = 1448;    // ~sqrt(2) * 1024
constexpr NavCost kCostClimb = 2048;   // 2.0
constexpr NavCost kCostJumpUp2 = 2560; // 2.5
bool standable(const NavView& n, int x, int y, int z) { return n.bits(x, y, z) & kNavStandable; }
bool passable(const NavView& n, int x, int y, int z) { return n.bits(x, y, z) & kNavPassable; }
bool headroom(const NavView& n, int x, int y, int z) { return passable(n, x, y + 1, z); }

/// Ground locomotion shared by Walk / Climb / Leap: flat moves, step-up 1 and
/// ledge drops up to maxDrop, in all 8 horizontal directions. Diagonals carry
/// a corner-cut guard: both orthogonal neighbour cells at the relevant level
/// must be passable, so a mover never clips through a diagonal wall corner.
/// Costs are the pre-P1 float costs in fixed point (step = flat + 512,
/// drop = flat + 512 per level fallen, diagonal base 1448).
void groundMoves(const NavView& n, Cell c, int maxDrop, std::vector<NavStep>& out) {
  auto tryDir = [&](int dx, int dz, bool diag) {
    const int x = c.x + dx, z = c.z + dz;
    const NavCost base = diag ? kCostDiag : kCostWalk;
    if (diag && (!passable(n, x, c.y, c.z) || !passable(n, c.x, c.y, z))) return;  // corner-cut guard
    if (standable(n, x, c.y, z) && headroom(n, x, c.y, z)) {
      out.push_back({{x, c.y, z}, base, StepTag::kWalk});
      return;
    }
    // Step up one: the mover's own head cell must clear too, and for a
    // diagonal the upper corner cells must be open (no clipping the corner
    // while rising).
    if (standable(n, x, c.y + 1, z) && headroom(n, x, c.y + 1, z) && headroom(n, c.x, c.y + 1, c.z) &&
        (!diag || (passable(n, x, c.y + 1, c.z) && passable(n, c.x, c.y + 1, z)))) {
      out.push_back({{x, c.y + 1, z}, base + 512, StepTag::kStep});
      return;
    }
    if (!passable(n, x, c.y, z) || !headroom(n, x, c.y, z)) return;
    for (int drop = 1; drop <= maxDrop; ++drop) {  // walk off a ledge
      if (standable(n, x, c.y - drop, z)) {
        out.push_back({{x, c.y - drop, z}, base + NavCost(512 * drop), StepTag::kDrop});
        break;
      }
      if (!passable(n, x, c.y - drop, z)) break;
    }
  };
  for (auto& d : kH) tryDir(d[0], d[1], false);
  for (auto& d : kD) tryDir(d[0], d[1], true);
}

/// Vertical moves on climbable cells (ladders / vines), shared by Climb/Leap.
void climbMoves(const NavView& n, Cell c, std::vector<NavStep>& out) {
  for (int dy : {1, -1})
    if ((n.bits(c.x, c.y + dy, c.z) & kNavClimb) || ((n.bits(c.x, c.y, c.z) & kNavClimb) && passable(n, c.x, c.y + dy, c.z)))
      out.push_back({{c.x, c.y + dy, c.z}, kCostClimb, StepTag::kClimb});
}

class Walk : public IMovementMode {
 public:
  explicit Walk(int maxDrop) : maxDrop_(maxDrop) {}
  std::string_view name() const override { return "walk"; }
  void neighbours(const NavView& n, Cell c, std::vector<NavStep>& out) const override {
    groundMoves(n, c, maxDrop_, out);
  }
 protected:
  int maxDrop_;
};
class Climb : public Walk {
 public:
  Climb() : Walk(3) {}
  std::string_view name() const override { return "climb"; }
  void neighbours(const NavView& n, Cell c, std::vector<NavStep>& out) const override {
    Walk::neighbours(n, c, out);
    climbMoves(n, c, out);
  }
};
/// Leap (P1): Walk + Climb, plus jump-up 2, gap jumps of 2-4 cells and long
/// drops of 4-8. Jump feasibility is closed-form from the cell model (per the
/// research report's S3 lesson: no runtime physics rollouts — headroom, arc
/// clearance and landing checks are pure nav-bit queries, hence exact and
/// deterministic). Numeric costs are JD-review-pending.
class Leap final : public Climb {
 public:
  std::string_view name() const override { return "leap"; }
  void neighbours(const NavView& n, Cell c, std::vector<NavStep>& out) const override {
    Climb::neighbours(n, c, out);
    jumpUp2(n, c, out);
    gapJumps(n, c, out);
    longDrops(n, c, out);
  }

 private:
  static void jumpUp2(const NavView& n, Cell c, std::vector<NavStep>& out) {
    auto tryDir = [&](int dx, int dz, bool diag) {
      const int x = c.x + dx, z = c.z + dz;
      // Landing standable at +2 with headroom above it; the mover needs
      // clearance at +2/+3 over its own column to rise into the arc.
      if (!standable(n, x, c.y + 2, z) || !headroom(n, x, c.y + 2, z)) return;
      if (!passable(n, c.x, c.y + 2, c.z) || !passable(n, c.x, c.y + 3, c.z)) return;
      // Diagonal: at apex height the body crosses the corner — both side
      // columns must be open at +2.
      if (diag && (!passable(n, x, c.y + 2, c.z) || !passable(n, c.x, c.y + 2, z))) return;
      out.push_back({{x, c.y + 2, z}, kCostJumpUp2 + (diag ? NavCost(512) : NavCost(0)), StepTag::kJump});
    };
    for (auto& d : kH) tryDir(d[0], d[1], false);
    for (auto& d : kD) tryDir(d[0], d[1], true);
  }
  static void gapJumps(const NavView& n, Cell c, std::vector<NavStep>& out) {
    auto tryDir = [&](int dx, int dz, bool diag) {
      for (int dist = 2; dist <= 4; ++dist) {
        const int lx = c.x + dx * dist, lz = c.z + dz * dist;
        for (int ly : {c.y, c.y - 1}) {  // same level, or landing one lower
          if (!standable(n, lx, ly, lz) || !headroom(n, lx, ly, lz)) continue;
          if (ly == c.y - 1 && !passable(n, lx, c.y, lz)) continue;  // shaft above the lower landing
          bool ok = true;
          for (int i = 1; i < dist && ok; ++i) {
            const int ix = c.x + dx * i, iz = c.z + dz * i;
            // Arc clearance at body and head height; a standable cell at
            // takeoff level in between means it is a walk, not a gap.
            if (!passable(n, ix, c.y, iz) || !passable(n, ix, c.y + 1, iz) || standable(n, ix, c.y, iz)) ok = false;
            if (ok && ly == c.y - 1 && !passable(n, ix, c.y - 1, iz)) ok = false;
            if (ok && diag) {  // corner cells at each diagonal sub-step
              if (!passable(n, ix, c.y, iz - dz) || !passable(n, ix - dx, c.y, iz)) ok = false;
            }
          }
          if (ok && diag) {  // corner cells at the landing step
            if (!passable(n, lx, c.y, lz - dz) || !passable(n, lx - dx, c.y, lz)) ok = false;
          }
          if (ok) out.push_back({{lx, ly, lz}, NavCost(1024 * dist + 512 * (dist - 1)), StepTag::kGap});
        }
      }
    };
    for (auto& d : kH) tryDir(d[0], d[1], false);
    for (auto& d : kD) tryDir(d[0], d[1], true);
  }
  static void longDrops(const NavView& n, Cell c, std::vector<NavStep>& out) {
    auto tryDir = [&](int dx, int dz, bool diag) {
      const int x = c.x + dx, z = c.z + dz;
      const NavCost base = diag ? kCostDiag : kCostWalk;
      if (diag && (!passable(n, x, c.y, c.z) || !passable(n, c.x, c.y, z))) return;  // corner-cut guard
      if (!passable(n, x, c.y, z) || !headroom(n, x, c.y, z)) return;
      // The shaft is scanned from drop 1 (a blockage anywhere ends the fall);
      // standable landings at drops 1..3 are groundMoves' edges, so only
      // drops 4..8 emit here, with a +1024 risk premium that a fluid landing
      // waives. A deep-fluid cell (fluid bit, not standable) is a landing at
      // any depth, premium-free.
      for (int drop = 1; drop <= 8; ++drop) {
        const uint8_t b = n.bits(x, c.y - drop, z);
        if (b & kNavStandable) {
          if (drop >= 4) {
            const NavCost risk = (b & kNavFluid) ? NavCost(0) : NavCost(1024);
            out.push_back({{x, c.y - drop, z}, base + NavCost(512 * drop) + risk, StepTag::kDrop});
          }
          break;
        }
        if (b & kNavFluid) {
          out.push_back({{x, c.y - drop, z}, base + NavCost(512 * drop), StepTag::kDrop});
          break;
        }
        if (!(b & kNavPassable)) break;
      }
    };
    for (auto& d : kH) tryDir(d[0], d[1], false);
    for (auto& d : kD) tryDir(d[0], d[1], true);
  }
};
class Fly final : public IMovementMode {
 public:
  std::string_view name() const override { return "fly"; }
  void neighbours(const NavView& n, Cell c, std::vector<NavStep>& out) const override {
    static constexpr int k6[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (auto& d : k6)
      if (passable(n, c.x + d[0], c.y + d[1], c.z + d[2]))
        out.push_back({{c.x + d[0], c.y + d[1], c.z + d[2]}, kCostWalk, StepTag::kFly});
  }
};
class Swim final : public Walk {
 public:
  Swim() : Walk(3) {}
  std::string_view name() const override { return "swim"; }
  void neighbours(const NavView& n, Cell c, std::vector<NavStep>& out) const override {
    Walk::neighbours(n, c, out);
    static constexpr int k6[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (auto& d : k6)
      if (n.bits(c.x + d[0], c.y + d[1], c.z + d[2]) & kNavFluid)
        out.push_back({{c.x + d[0], c.y + d[1], c.z + d[2]}, NavCost(1229), StepTag::kSwim});  // 1.2 * 1024
  }
};
class WireFollow final : public IMovementMode {
 public:
  std::string_view name() const override { return "wire-follow"; }
  void neighbours(const NavView& n, Cell c, std::vector<NavStep>& out) const override {
    for (auto& d : kH)
      for (int dy : {0, 1, -1}) {
        const int x = c.x + d[0], y = c.y + dy, z = c.z + d[1];
        if (n.bits(x, y, z) & kNavTrack) {
          out.push_back({{x, y, z}, dy != 0 ? NavCost(2048) : kCostWalk, StepTag::kTrack});
          break;
        }
      }
  }
};
}  // namespace

WalkCache::WalkCache(const ChunkMap& world, const BlockRegistry& blocks, NavRules rules,
                     size_t maxColumns)
    : world_(world), blocks_(blocks), rules_(std::move(rules)), maxColumns_(maxColumns) {
  if (!rules_.solid) rules_.solid = [this](BlockId b) { return blocks_.has(b, kSolid); };
}

uint8_t WalkCache::compute(int x, int y, int z) const {
  auto at = [&](int yy) { return yy < 0 || yy >= kWorldHeight ? BlockId(kAir) : world_.get(x, yy, z); };
  const BlockId b = at(y), below = at(y - 1);
  uint8_t r = 0;
  const bool passable = !rules_.solid(b);
  if (passable) r |= kNavPassable;
  if (passable && y > 0 && rules_.solid(below)) r |= kNavStandable;
  if (rules_.climbable && rules_.climbable(b)) r |= kNavClimb | kNavPassable;
  if (rules_.fluid && rules_.fluid(b)) r |= kNavFluid;
  if (rules_.track && passable && rules_.track(b)) r |= kNavTrack;  // the wire cell itself (wires are not solid)
  return r;
}

std::shared_ptr<const WalkCache::Column> WalkCache::column(int cx, int cz) const {
  uint64_t gen;
  {
    std::lock_guard l(m_);
    if (auto it = cols_.find(colKey(cx, cz)); it != cols_.end()) {
      it->second->lastUsed = ++useClock_;
      return it->second;
    }
    gen = generation_;
  }
  if (!world_.column(cx, cz)) return nullptr;
  auto col = std::make_shared<Column>();
  col->words.assign(5 * kWordsPerPlane, 0);
  std::array<std::vector<int16_t>, kChunkSize * kChunkSize> spans;  // ascending heights per (z,x)
  for (int y = 0; y < kWorldHeight; ++y)
    for (int z = 0; z < kChunkSize; ++z)
      for (int x = 0; x < kChunkSize; ++x) {
        const uint8_t b = compute(cx * kChunkSize + x, y, cz * kChunkSize + z);
        if (!b) continue;  // solid airless cell: no plane bit, no span
        const size_t idx = cellIndex(x, y, z);
        const size_t w = idx >> 6;
        const uint64_t bit = uint64_t(1) << (idx & 63);
        if (b & kNavPassable) col->words[w] |= bit;
        if (b & kNavStandable) {
          col->words[kWordsPerPlane + w] |= bit;
          spans[size_t(z) * kChunkSize + size_t(x)].push_back(int16_t(y));
        }
        if (b & kNavClimb) col->words[2 * kWordsPerPlane + w] |= bit;
        if (b & kNavFluid) col->words[3 * kWordsPerPlane + w] |= bit;
        if (b & kNavTrack) col->words[4 * kWordsPerPlane + w] |= bit;
      }
  for (size_t i = 0; i < spans.size(); ++i) {
    col->spanOff[i] = uint32_t(col->spanData.size());
    col->spanCnt[i] = uint16_t(spans[i].size());
    for (auto rit = spans[i].rbegin(); rit != spans[i].rend(); ++rit) col->spanData.push_back(*rit);
  }
  std::lock_guard l(m_);
  // Poison check: invalidate() ran mid-compute -> drop the stale column instead
  // of reinserting it. Also don't clobber a fresher column computed concurrently.
  if (gen != generation_) return nullptr;
  auto [it, inserted] = cols_.emplace(colKey(cx, cz), std::move(col));
  it->second->lastUsed = ++useClock_;
  // P3 LRU cap: evict least-recently-used columns down to the cap. Safe at
  // any moment: NavViews hold shared_ptrs to the columns they use, and a
  // re-requested column recomputes from the live map (never staler than
  // the invalidation contract -- eviction can only make data fresher).
  while (maxColumns_ != 0 && cols_.size() > maxColumns_) {
    auto victim = cols_.begin();
    for (auto jt = cols_.begin(); jt != cols_.end(); ++jt)
      if (jt->second->lastUsed < victim->second->lastUsed) victim = jt;
    if (victim == it) break;  // unreachable at cap >= 1; defensive
    cols_.erase(victim);
  }
  return it->second;
}

uint8_t WalkCache::bits(int x, int y, int z) const {
  if (y < 0 || y >= kWorldHeight) return y >= kWorldHeight ? uint8_t(kNavPassable) : uint8_t(0);
  const auto col = column(x >> 4, z >> 4);
  if (!col) return 0;
  return assemble(*col, cellIndex(x & 15, y, z & 15));
}

size_t WalkCache::cachedBytes() const {
  std::lock_guard l(m_);
  size_t n = 0;
  for (const auto& entry : cols_) {
    const Column& c = *entry.second;
    n += sizeof(Column) + c.words.size() * sizeof(uint64_t) + c.spanData.size() * sizeof(int16_t);
  }
  return n;
}

std::vector<int16_t> WalkCache::standableSpans(int x, int z) const {
  const auto col = column(x >> 4, z >> 4);
  if (!col) return {};
  const size_t i = size_t(z & 15) * kChunkSize + size_t(x & 15);
  const uint32_t off = col->spanOff[i];
  const uint16_t n = col->spanCnt[i];
  return {col->spanData.begin() + off, col->spanData.begin() + off + n};
}

int WalkCache::topStandable(int x, int z) const {
  const auto col = column(x >> 4, z >> 4);
  if (!col) return -1;
  const size_t i = size_t(z & 15) * kChunkSize + size_t(x & 15);
  return col->spanCnt[i] ? int(col->spanData[col->spanOff[i]]) : -1;
}

uint8_t NavView::bits(int x, int y, int z) const {
  if (y < 0 || y >= kWorldHeight) return y >= kWorldHeight ? uint8_t(kNavPassable) : uint8_t(0);
  const uint64_t key = colKey(x >> 4, z >> 4);
  if (key != lastKey_) {
    auto it = held_.find(key);
    if (it == held_.end()) it = held_.emplace(key, cache_.column(x >> 4, z >> 4)).first;
    lastKey_ = key;
    lastCol_ = it->second.get();
  }
  return lastCol_ ? WalkCache::assemble(*lastCol_, WalkCache::cellIndex(x & 15, y, z & 15)) : uint8_t(0);
}

uint64_t WalkCache::generation() const { std::lock_guard l(m_); return generation_; }

uint64_t WalkCache::columnVersion(int cx, int cz) const {
  std::lock_guard l(m_);
  const auto it = colVer_.find(colKey(cx, cz));
  return it == colVer_.end() ? 0 : it->second;
}

void WalkCache::invalidate(int x, int z) {
  std::lock_guard l(m_);
  cols_.erase(colKey(x >> 4, z >> 4));
  ++colVer_[colKey(x >> 4, z >> 4)];
  ++generation_;
}
size_t WalkCache::cachedColumns() const { std::lock_guard l(m_); return cols_.size(); }

std::unique_ptr<IMovementMode> makeWalk(int maxDrop) { return std::make_unique<Walk>(maxDrop); }
std::unique_ptr<IMovementMode> makeClimb() { return std::make_unique<Climb>(); }
std::unique_ptr<IMovementMode> makeLeap() { return std::make_unique<Leap>(); }
std::unique_ptr<IMovementMode> makeFly() { return std::make_unique<Fly>(); }
std::unique_ptr<IMovementMode> makeSwim() { return std::make_unique<Swim>(); }
std::unique_ptr<IMovementMode> makeWireFollow() { return std::make_unique<WireFollow>(); }

Cell hopToward(Cell from, Cell goal, int hop) {
  const int dx = goal.x - from.x, dy = goal.y - from.y, dz = goal.z - from.z;
  const int m = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});
  if (m <= hop) return goal;
  return {from.x + dx * hop / m, from.y + dy * hop / m, from.z + dz * hop / m};
}

Path findPath(const WalkCache& cache, const PathRequest& req) {
  Path out;
  const NavView nav(cache);
  if (!req.mode) return out;
  struct Node { NavCost f, g; uint64_t order; Cell c; };
  auto cmp = [](const Node& a, const Node& b) { return a.f != b.f ? a.f > b.f : a.order > b.order; };  // FIFO ties: deterministic
  std::priority_queue<Node, std::vector<Node>, decltype(cmp)> open(cmp);
  struct Parent { uint64_t from; NavCost g; StepTag tag; };
  std::unordered_map<uint64_t, Parent> parent;  // key -> (parent key, g, arriving edge tag)
  std::unordered_map<uint64_t, Cell> cells;
  parent.reserve(size_t(req.maxNodes) * 4);
  cells.reserve(size_t(req.maxNodes) * 4);
  const uint64_t start = cellKey(req.from);
  parent[start] = {start, 0, StepTag::kStepStart};
  cells[start] = req.from;
  open.push({heurFp(req.from, req.to), 0, 0, req.from});
  uint64_t order = 1, best = start;
  NavCost bestH = heurFp(req.from, req.to);
  std::vector<NavStep> nb;
  bool found = false;
  while (!open.empty() && out.expanded < req.maxNodes) {
    const Node n = open.top();
    open.pop();
    const uint64_t k = cellKey(n.c);
    if (n.g > parent[k].g) continue;  // stale
    ++out.expanded;
    if (n.c == req.to) { best = k; found = true; break; }
    if (const NavCost h = heurFp(n.c, req.to); h < bestH) { bestH = h; best = k; }
    nb.clear();
    req.mode->neighbours(nav, n.c, nb);
    for (const NavStep& s : nb) {
      const Cell c = s.to;
      if (std::abs(c.x - req.from.x) > req.maxRange || std::abs(c.z - req.from.z) > req.maxRange || std::abs(c.y - req.from.y) > req.maxRange) continue;
      const uint64_t ck = cellKey(c);
      const NavCost g = n.g + s.cost;
      if (auto it = parent.find(ck); it != parent.end() && it->second.g <= g) continue;
      parent[ck] = {k, g, s.tag};
      cells[ck] = c;
      open.push({g + heurFp(c, req.to), g, order++, c});
    }
  }
  for (uint64_t k = best;; k = parent[k].from) {
    out.cells.push_back(cells[k]);
    out.steps.push_back(parent[k].tag);
    if (k == start) break;
  }
  std::reverse(out.cells.begin(), out.cells.end());
  std::reverse(out.steps.begin(), out.steps.end());
  out.totalCost = parent[best].g;
  out.complete = found;
  if (req.smooth) out = smoothPath(cache, out);
  return out;
}

Path smoothPath(const WalkCache& cache, const Path& in) {
  if (in.cells.size() < 3 || in.steps.size() != in.cells.size()) return in;
  const NavView nav(cache);
  // Can the mover travel the straight segment a -> b (same y) directly?
  // Exact integer supercover of the segment: every crossed cell must be
  // standable with headroom. Corner crossings count BOTH side cells, so a
  // smoothed leg never cuts a corner the step-by-step path could not.
  auto flatClear = [&](Cell a, Cell b) {
    if (a.y != b.y) return false;
    const int dx = b.x - a.x, dz = b.z - a.z;
    const int sx = dx > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
    const int adx = std::abs(dx), adz = std::abs(dz);
    auto clearCell = [&](int x, int z) { return standable(nav, x, a.y, z) && headroom(nav, x, a.y, z); };
    int x = a.x, z = a.z, i = 0, j = 0;  // i/j: grid lines crossed so far per axis
    while (i < adx || j < adz) {
      const bool hasX = i < adx, hasZ = j < adz;
      // Next crossing parameters compared exactly: (2i+1)*adz vs (2j+1)*adx.
      const long long tx = hasX ? (2LL * i + 1) * adz : 0;
      const long long tz = hasZ ? (2LL * j + 1) * adx : 0;
      if (hasX && (!hasZ || tx < tz)) { x += sx; ++i; if (!clearCell(x, z)) return false; }
      else if (hasZ && (!hasX || tz < tx)) { z += sz; ++j; if (!clearCell(x, z)) return false; }
      else {  // exact corner: both side cells and the diagonal cell are crossed
        if (!clearCell(x + sx, z) || !clearCell(x, z + sz)) return false;
        x += sx; z += sz; ++i; ++j;
        if (!clearCell(x, z)) return false;
      }
    }
    return true;
  };
  // The subpath in.cells[lo..hi] must itself be flat for a shortcut to apply.
  auto flatSpan = [&](size_t lo, size_t hi) {
    for (size_t k = lo + 1; k <= hi; ++k)
      if (in.cells[k].y != in.cells[lo].y) return false;
    return true;
  };
  Path out;
  out.complete = in.complete;
  out.expanded = in.expanded;
  out.totalCost = in.totalCost;  // smoothing never raises the quoted cost
  out.cells.push_back(in.cells.front());
  out.steps.push_back(StepTag::kStepStart);
  size_t anchor = 0;
  while (anchor + 1 < in.cells.size()) {
    size_t far = anchor + 1;
    if (in.cells[anchor].y == in.cells[anchor + 1].y)
      for (size_t j = anchor + 2; j < in.cells.size(); ++j) {
        if (!flatSpan(anchor, j) || !flatClear(in.cells[anchor], in.cells[j])) break;
        far = j;
      }
    out.cells.push_back(in.cells[far]);
    out.steps.push_back(far == anchor + 1 ? in.steps[far] : StepTag::kWalk);
    anchor = far;
  }
  return out;
}

}  // namespace litholattice::ai
