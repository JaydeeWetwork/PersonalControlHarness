#include <chrono>
#include <vector>
#include "lithokraft/content/signal.h"
#include <algorithm>
#include "litholattice/core/log.h"
#include "litholattice/storage/storage.h"
#include "litholattice/sim/det_sched.h"
#include "lithokraft/content/component.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace lithokraft {

namespace {
constexpr int kH[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
constexpr int k6[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
}  // namespace

int sixbitCode(char32_t c) {
  if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
  return (c >= 32 && c <= 95) ? int(c) - 32 : -1;
}
char sixbitChar(int code) { return (code >= 0 && code < 64) ? char(code + 32) : '?'; }

std::map<uint64_t, std::vector<uint8_t>> SignalSim::exportState() const {
  std::map<int64_t, const Comp*> sorted;  // deterministic order
  for (const auto& [k, c] : comps_) sorted.emplace(k, &c);
  std::map<uint64_t, storage::ByteWriter> cols;
  for (const auto& [k, c] : sorted) {
    int x, y, z; unpackPos(k, x, y, z);
    storage::ByteWriter& w = cols[ChunkKey::make(x >> 4, z >> 4)];
    const ComponentMemory& m = c->mem;
    w.u64(uint64_t(k));
    w.u8(m.out); w.u8(m.out2); w.u8(m.pulse ? 1 : 0); w.u8(m.prevIn); w.u8(m.prevSide); w.u8(m.prevClk);
    w.u8(m.history); w.u8(m.timer); w.u8(m.channel); w.u8(m.beacon ? 1 : 0);
    w.u16(uint16_t(std::min<size_t>(m.keys.size(), 65535)));
    for (size_t i = 0; i < m.keys.size() && i < 65535; ++i) { w.u8(m.keys[i].first); w.u8(m.keys[i].second); }
  }
  std::map<uint64_t, std::vector<uint8_t>> out;
  for (auto& [key, w] : cols) out[key] = std::move(w.out);
  return out;
}

bool SignalSim::importState(const std::vector<uint8_t>& b) {
  struct Entry { int64_t key; ComponentMemory mem; };
  std::vector<Entry> entries;
  storage::ByteReader r(b.data(), b.size());
  while (r.ok() && !r.done()) {
    Entry e;
    e.key = int64_t(r.u64());
    ComponentMemory& m = e.mem;
    // SW-H2 trust boundary: a migrated / older-version / hand-edited (CRC-valid)
    // SGNL payload can carry outputs up to 255, but strengths are 1..kMaxSignal
    // and the BFS bucket arrays are sized kMaxSignal + 1. Clamp TO kMaxSignal
    // (never zero) so an imported output can never index a bucket out of bounds.
    m.out = uint8_t(std::min<int>(r.u8(), kMaxSignal)); m.out2 = uint8_t(std::min<int>(r.u8(), kMaxSignal));
    m.pulse = r.u8() != 0; m.prevIn = r.u8(); m.prevSide = r.u8(); m.prevClk = r.u8();
    m.history = r.u8(); m.timer = r.u8(); m.channel = r.u8(); m.beacon = r.u8() != 0;
    const uint16_t n = r.u16();
    for (uint16_t i = 0; i < n && r.ok(); ++i) { const uint8_t f = r.u8(); m.keys.emplace_back(f, r.u8()); }
    entries.push_back(std::move(e));
  }
  if (!r.ok()) return false;
  fullPass_ = true;
  for (Entry& e : entries)
    if (auto it = comps_.find(e.key); it != comps_.end()) it->second.mem = std::move(e.mem);  // component still there
  return true;
}

namespace {
// Index every signal cell of ONE column (shared by scan() and scanColumn()).
void scanOneColumn(SignalSim& sim, const ChunkColumn& col, const BlockRegistry& reg) {
  std::vector<BlockId> lut;  // StableId palette -> BlockId, resolved once per sub-chunk
  for (int sy = 0; sy < kSubChunks; ++sy) {
    const SubChunk* s = col.subs[sy].get();
    if (!s) continue;
    const bool w64 = s->blocks.width() == VoxelPalette::Width::W64;
    if (!w64) s->blocks.resolveBlockIds(reg, lut);
    for (int i = 0; i < kSubChunkVolume; ++i) {
      BlockId b;
      if (w64) {  // degenerate width: index IS the StableId (unreachable in practice)
        const auto id = reg.findByStableId(s->blocks.at(size_t(i)));
        b = id ? *id : kAir;
      } else {
        b = lut[s->blocks.rawIndex(size_t(i))];
      }
      if (!isComponent(b)) continue;
      const int x = col.cx * 16 + (i & 15), z = col.cz * 16 + ((i >> 4) & 15), y = sy * 16 + (i >> 8);
      sim.onBlockChanged(x, y, z, b);
    }
  }
}
}  // namespace

void SignalSim::scan(const ChunkMap& w) {
  torches_.clear(); wires_.clear(); lamps_.clear(); comps_.clear();
  const BlockRegistry& reg = blockRegistry();
  for (const auto& [k, col] : w.columns()) scanOneColumn(*this, *col, reg);
}

void SignalSim::scanColumn(const ChunkMap& w, int cx, int cz) {
  const ChunkColumn* col = w.column(cx, cz);
  if (!col) return;
  scanOneColumn(*this, *col, blockRegistry());
}

std::vector<uint8_t> SignalSim::exportColumnState(int cx, int cz) const {
  const auto all = exportState();
  const auto it = all.find(ChunkKey::make(cx, cz));
  return it == all.end() ? std::vector<uint8_t>{} : it->second;
}

namespace {
bool keyInColumn(int64_t k, int cx, int cz) {
  int x, y, z;
  unpackPos(k, x, y, z);
  return (x >> 4) == cx && (z >> 4) == cz;
}
template <typename Set>
void eraseColumnKeys(Set& s, int cx, int cz) {
  for (auto it = s.begin(); it != s.end();) {
    if (keyInColumn(*it, cx, cz)) it = s.erase(it);
    else ++it;
  }
}
}  // namespace

bool SignalSim::hasIndexedCells(int cx, int cz) const { return indexedCellCount(cx, cz) != 0; }

size_t SignalSim::indexedCellCount(int cx, int cz) const {
  size_t n = 0;
  for (int64_t k : torches_) n += keyInColumn(k, cx, cz) ? 1 : 0;
  for (int64_t k : wires_) n += keyInColumn(k, cx, cz) ? 1 : 0;
  for (int64_t k : lamps_) n += keyInColumn(k, cx, cz) ? 1 : 0;
  for (const auto& [k, c] : comps_) n += keyInColumn(k, cx, cz) ? 1 : 0;
  return n;
}

void SignalSim::unscanColumn(int cx, int cz) {
  // Removed wires keep a trace-strength slot in the buffer (slots are
  // never recycled); zero them so a stale strength can never be read for
  // a cell that no longer exists. A re-streamed column's wires get their
  // slots rewritten by the next flood's final-state replay.
  for (int64_t k : wires_)
    if (keyInColumn(k, cx, cz)) {
      const uint32_t slot = strengthBuf_.slotOf(uint64_t(k));
      if (slot != litholattice::TraceStrengthBuffer::kNoSlot) strengthBuf_.set(slot, 0);
    }
  eraseColumnKeys(torches_, cx, cz);
  eraseColumnKeys(wires_, cx, cz);
  eraseColumnKeys(lamps_, cx, cz);
  for (auto it = lampHold_.begin(); it != lampHold_.end();) {
    if (keyInColumn(it->first, cx, cz)) it = lampHold_.erase(it);
    else ++it;
  }
  eraseColumnKeys(awake_, cx, cz);
  for (auto it = comps_.begin(); it != comps_.end();) {
    if (keyInColumn(it->first, cx, cz)) it = comps_.erase(it);
    else ++it;
  }
  eraseColumnKeys(mountPowered_, cx, cz);  // rebuilt per tick anyway; drop now
  // Per-tick scratch naming this column must not survive into the next
  // tick (the sim is stopped for eviction, so clearing is safe): pending
  // lamp recomputes and changed-source marks for cells that are gone.
  eraseColumnKeys(dirtyLamps_, cx, cz);
  std::erase_if(extraLamps_, [&](int64_t k) { return keyInColumn(k, cx, cz); });
  std::erase_if(changedSrc_, [&](int64_t k) { return keyInColumn(k, cx, cz); });
  srcReach_.clear();  // cached reach sets reference wire nets; rebuilt with the nets
  fullPass_ = true;   // layout changed: nets and cached pointers are stale
  netsValid_ = false;
}

void SignalSim::onBlockChanged(int x, int y, int z, BlockId now) {
  const int64_t k = packPos(x, y, z);
  fullPass_ = true; netsValid_ = false;
  torches_.erase(k); wires_.erase(k); lamps_.erase(k); comps_.erase(k); lampHold_.erase(k);
  const SimRole r = roleOf(now);
  if (r == SimRole::Torch) torches_.insert(k);
  else if (r == SimRole::Wire) wires_.insert(k);
  else if (r == SimRole::Lamp || r == SimRole::ColorLamp) lamps_.insert(k);
  else if (const ComponentBehavior* b = BehaviorCatalog::get().component(now)) comps_[k] = Comp{b, {}};
}

// Minecraft-style strong power: a lit torch (either colour) powers the solid opaque block above it.
// An attached (wall / floor / ceiling) switch that is on strongly powers its support block.
// (Attached switches / buttons are found once per pass by collectMountPowered, not per call.)
bool SignalSim::strongPowered(const ChunkMap& w, int x, int y, int z) const {
  const auto& reg = blockRegistry();
  if (!reg.hasAll(w.get(x, y, z), kSolid | kOpaque | kPowerable)) return false;
  if (reg.def(w.get(x, y - 1, z)).role == SimRole::Torch && stValue(w.getState(x, y - 1, z)) > 0) return true;
  return !mountPowered_.empty() && mountPowered_.contains(packPos(x, y, z));
}

void SignalSim::collectMountPowered(const ChunkMap& w) {
  // B-L1 acceptance (P3-2): unordered comps_ iteration — the result is a
  // membership SET (mountPowered_); iteration order cannot leak anywhere.
  mountPowered_.clear();
  for (const auto& [k, c] : comps_) {
    int x, y, z; unpackPos(k, x, y, z);
    const CellState s = w.getState(x, y, z);
    if (!stAttach(s) || !stOn(s)) continue;
    if (const auto r = blockRegistry().def(w.get(x, y, z)).role; r != uint16_t(SimRole::Switch) && r != uint16_t(SimRole::Button)) continue;
    int ox, oy, oz;
    attachOffset(stAttach(s), ox, oy, oz);
    mountPowered_.insert(packPos(x + ox, y + oy, z + oz));
  }
  // A lit Repeater hard-powers the solid conductive block it faces (not glass / ceramic: they are
  // not powerable), so wire and components on that block's other faces pick the signal up.
  for (const auto& [k, c] : comps_) {
    if (!c.mem.out || c.mem.pulse) continue;
    int x, y, z; unpackPos(k, x, y, z);
    if (blockRegistry().def(w.get(x, y, z)).role != uint16_t(SimRole::Repeater)) continue;
    const CellState s = w.getState(x, y, z);
    for (int d = 0; d < 4; ++d) {
      if (c.behavior->emitToward(d, s, c.mem) <= 0) continue;
      const int nx = x + kDirX[d], nz = z + kDirZ[d];
      if (blockRegistry().hasAll(w.get(nx, y, nz), kSolid | kOpaque | kPowerable)) mountPowered_.insert(packPos(nx, y, nz));
    }
  }
}

// A torch's support: below for free-standing (legacy) torches, else its attach face.
static void torchSupport(CellState s, int x, int y, int z, int& sx, int& sy, int& sz) {
  int dx, dy, dz;
  attachOffset(stAttach(s) ? stAttach(s) : 1, dx, dy, dz);
  sx = x + dx; sy = y + dy; sz = z + dz;
}

bool SignalSim::blockPowered(const ChunkMap& w, int x, int y, int z) const {
  const auto& reg = blockRegistry();
  if (!reg.has(w.get(x, y, z), kPowerable)) return false;
  if (strongPowered(w, x, y, z)) return true;
  if (isFlatWire(w.get(x, y + 1, z)) && stValue(w.getState(x, y + 1, z)) > 0) return true;  // wire on top
  for (auto& d : kH)
    if (isFlatWire(w.get(x + d[0], y, z + d[1])) && stValue(w.getState(x + d[0], y, z + d[1])) > 0) return true;
  return false;
}

int SignalSim::emitToward(const ChunkMap& w, int x, int y, int z, int dir) const {
  const auto it = comps_.find(packPos(x, y, z));
  return it == comps_.end() ? 0 : it->second.behavior->emitToward(dir, w.getState(x, y, z), it->second.mem);
}

int SignalSim::inputFrom(const ChunkMap& w, int x, int y, int z, int d) const {
  const int nx = x + kDirX[d], nz = z + kDirZ[d];
  const BlockDef nd = blockRegistry().def(w.get(nx, y, nz));
  const CellState s = w.getState(nx, y, nz);
  // SW-H2: a disk-loaded wire cell state carries a 7-bit value (65..127 are
  // representable); clamp at this sampling point so no component input — and
  // therefore no pass-through output — can ever exceed kMaxSignal.
  if (nd.role == SimRole::Wire) return isVia(w.get(nx, y, nz)) ? 0 : std::min(stValue(s), kMaxSignal);
  if (nd.role == SimRole::Torch) return stValue(s) ? kMaxSignal : 0;
  if (comps_.contains(packPos(nx, y, nz))) return emitToward(w, nx, y, nz, dirBack(d));
  return strongPowered(w, nx, y, nz) ? kMaxSignal : 0;
}

// Catchor output side from its state (mirrors Catchor::outDir in logic_components.cpp).
static int emitDirOfCatchor(CellState s) {
  const int f = stFacing(s), m = stAux(s) & 3;
  return m == 1 ? (f + 3) & 3 : m == 2 ? (f + 1) & 3 : f;
}

bool SignalSim::use(ChunkMap& w, int x, int y, int z, const DirtyFn& dirty, bool alt) {
  const auto it = comps_.find(packPos(x, y, z));
  if (it == comps_.end()) {  // using a wire: the first neighbouring component that reacts to its colour
    const BlockDef wd = blockRegistry().def(w.get(x, y, z));
    if (wd.role != SimRole::Wire) return false;
    for (int d = 0; d < 4; ++d) {
      const int nx = x + kDirX[d], nz = z + kDirZ[d];
      const auto c = comps_.find(packPos(nx, y, nz));
      if (c == comps_.end()) continue;
      if (const auto ns = c->second.behavior->useChannel(w.getState(nx, y, nz), wd.channel)) {
        fullPass_ = true;
        w.setState(nx, y, nz, *ns);
        dirty(nx, y, nz);
        return true;
      }
    }
    return false;
  }
  std::optional<CellState> next;
  const CellState cur = w.getState(x, y, z);
  if (alt) next = it->second.behavior->useAlt(cur, it->second.mem);
  else if (roleOf(w.get(x, y, z)) == SimRole::Redirector && !(stAux(cur) & 4)) {
    // Fed by a Catchor: the Catchor is its input, so the output only cycles centre -> left ->
    // right relative to it (never back into the Catchor, never up or down).
    for (int d = 0; d < 4 && !next; ++d) {
      const int nx = x + kDirX[d], nz = z + kDirZ[d];
      if (roleOf(w.get(nx, y, nz)) != SimRole::Catchor || !comps_.contains(packPos(nx, y, nz))) continue;
      if (emitDirOfCatchor(w.getState(nx, y, nz)) != dirBack(d)) continue;  // its output must point here
      const int t = dirBack(d), opts[3] = {t, (t + 3) & 3, (t + 1) & 3};
      int k = 0;
      while (k < 3 && opts[k] != stFacing(cur)) ++k;
      next = makeState(stValue(cur), opts[(k + 1) % 3], stAux(cur), stOn(cur));
    }
  }
  if (!next && !alt) next = it->second.behavior->use(cur, it->second.mem);
  if (!next) return false;
  fullPass_ = true;
  w.setState(x, y, z, *next);
  dirty(x, y, z);
  return true;
}

std::string SignalSim::inspect(const ChunkMap& w, int x, int y, int z) const {
  static constexpr const char* kDir = "NESW";
  const BlockId b = w.get(x, y, z);
  const CellState s = w.getState(x, y, z);
  const BlockDef d = blockRegistry().def(b);
  std::string r = std::format("#{} @{},{},{} state 0x{:04x} [value {} facing {} aux {} on {}]", b, x, y, z, s, stValue(s), kDir[stFacing(s)],
                              stAux(s), stOn(s) ? 1 : 0);
  const int64_t k = packPos(x, y, z);
  if (d.role == SimRole::Wire) {
    r += std::format(" | {} Trace{}", d.channel == 2 ? "blue" : "red", isVia(b) ? " (via)" : "");
    if (const auto it = wireNet_.find(k); it != wireNet_.end()) r += std::format(" net {}", it->second);
    else r += " (no net yet)";
  }
  if (const auto it = comps_.find(k); it != comps_.end()) {
    const ComponentMemory& m = it->second.mem;
    r += std::format(" | out {}{} out2 {} | edges in {} side {} clk {} | hist 0x{:02x} timer {}", m.out, m.pulse ? " (pulse)" : "", m.out2,
                     m.prevIn, m.prevSide, m.prevClk, m.history, m.timer);
  }
  return r;
}

void SignalSim::keyPulse(int x, int y, int z, int front, int control) {
  if (const auto it = comps_.find(packPos(x, y, z)); it != comps_.end())
  { it->second.mem.keys.push_back({uint8_t(front), uint8_t(control)}); fullPass_ = true; }
}

void SignalSim::tick(ChunkMap& w, int pcx, int pcz, int radius, const DirtyFn& dirty) {
  // Reassembled from the hs* pieces (hs-design §3.2); the handshake path in
  // SignalWorldSimulation drives the identical pieces through the authority.
  hsBeginCycle(w, pcx, pcz, radius);
  hsFlipAndTorches(w, dirty);
  hsComponents(w, dirty);
  hsMark(w);
  if (!hsSkip_) {
    if (fullPass_) hsPropagateFull(w, dirty);
    else {
      hsPrepIncremental(w);
      for (int32_t id : hsMarkedNets_) hsPropagateNet(w, id, dirty);
      hsApplyStrengthPending();
    }
    hsLampsBatch(hsLampKeys_, w, dirty);
    hsApplyLampHolds(hsTlsLampHolds());
    hsTlsLampHolds().clear();
    hsFinishCycle(w);
  }
  hsCommit();
}

void SignalSim::hsBeginCycle(ChunkMap& w, int pcx, int pcz, int radius) {
  ++tickNo_;
  // NOTE: radio_/jammers_ flips are NOT here any more — they are hsCommit()
  // at cycle end, in both modes (D12; see signal.h).
  if (area_.pcx != pcx || area_.pcz != pcz || area_.radius != radius) { fullPass_ = true; netsValid_ = false; }
  if (w.columns().size() != netColumns_) netsValid_ = false;  // columns loaded / dropped: cached pointers stale
  area_ = {pcx, pcz, radius};
  changedSrc_.clear(); extraLamps_.clear();
  if (tickNo_ >= nextHoldExpiry_) {  // afterglows ending this tick need a recompute; find the next end
    nextHoldExpiry_ = UINT64_MAX;
    for (const auto& [k, end] : lampHold_) {
      if (end <= tickNo_) extraLamps_.push_back(k);
      else nextHoldExpiry_ = std::min(nextHoldExpiry_, end);
    }
  }
  const bool wakeAll = allAwake_ || fullPass_ || !compSleep_;
  allAwake_ = wakeAll;  // read by tickComponents
  hsSkip_ = false;
}

void SignalSim::hsFlipAndTorches(ChunkMap& w, const DirtyFn& dirty) {
  collectMountPowered(w);
  tickTorches(w, dirty);
}

void SignalSim::hsComponents(ChunkMap& w, const DirtyFn& dirty) {
  const auto c0 = std::chrono::steady_clock::now();
  tickComponents(w, dirty);
  phase_.comps += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
}

void SignalSim::hsMark(ChunkMap& w) {
  const auto m0 = std::chrono::steady_clock::now();
  // Next tick's wake set: around every changed source; the readers of every re-flooded network.
  allAwake_ = fullPass_ || !compSleep_ || !netsValid_;
  awake_.clear();
  auto populateWake = [this] {
    // The legacy WakeNets RAII ran at tick() exit over the marked bitset as
    // it stood then; on the skip path that bitset still holds the PREVIOUS
    // cycle's marks (they are only cleared inside the marking block below),
    // so this reads the bitset directly, not hsMarkedNets_.
    if (allAwake_) return;
    for (int64_t k : changedSrc_) wakeAround(k);
    forEachMarkedNet([&](int32_t id) {
      if (size_t(id) < netComps_.size()) awake_.insert(netComps_[size_t(id)].begin(), netComps_[size_t(id)].end());
    });
    for (int64_t k : extraLamps_) wakeAround(k);
  };
  const std::unordered_set<int64_t> mountsBefore = mountPowered_.empty() ? std::unordered_set<int64_t>{} : mountPowered_;
  collectMountPowered(w);
  for (int64_t k : mountsBefore) if (!mountPowered_.contains(k)) changedSrc_.push_back(k);
  for (int64_t k : mountPowered_) if (!mountsBefore.contains(k)) changedSrc_.push_back(k);
  if (!fullPass_ && changedSrc_.empty() && extraLamps_.empty()) {  // same result as last pass
    hsSkip_ = true;
    populateWake();
    return;
  }
  if (!fullPass_) {
    if (!netsValid_) buildNets(w);
    clearNetMarks(); dirtyLamps_.clear();
    for (int64_t k : changedSrc_) {
      auto [rit, fresh] = srcReach_.try_emplace(k);
      SrcReach& r = rit->second;
      if (fresh) {  // the 5 x 5 x 5 box once per source, cached until the networks change
        int x, y, z; unpackPos(k, x, y, z);
        for (int dy = -2; dy <= 2; ++dy)
          for (int dz = -2; dz <= 2; ++dz)
            for (int dx = -2; dx <= 2; ++dx) {
              const int64_t n = packPos(x + dx, y + dy, z + dz);
              if (const auto it = wireNet_.find(n); it != wireNet_.end()) {
                if (std::find(r.nets.begin(), r.nets.end(), it->second) == r.nets.end()) r.nets.push_back(it->second);
              } else if (lamps_.contains(n)) r.lamps.push_back(n);
            }
      }
      for (int32_t id : r.nets) markNet(id);
      dirtyLamps_.insert(r.lamps.begin(), r.lamps.end());
    }
    forEachMarkedNet([&](int32_t id) { dirtyLamps_.insert(netLamps_[size_t(id)].begin(), netLamps_[size_t(id)].end()); });
    dirtyLamps_.insert(extraLamps_.begin(), extraLamps_.end());
  }
  // Canonical work lists for this cycle (hs-design §3.2/§9.6): marked nets
  // ascending by id; lamp keys sorted ascending (legacy iterated the
  // unordered sets — per-lamp writes are disjoint and hold merges are
  // commutative, so sorting changes no outcome, only the visit order).
  hsMarkedNets_.clear();
  if (!fullPass_) forEachMarkedNet([&](int32_t id) { hsMarkedNets_.push_back(id); });
  hsLampKeys_.clear();
  if (fullPass_) hsLampKeys_.assign(lamps_.begin(), lamps_.end());
  else hsLampKeys_.assign(dirtyLamps_.begin(), dirtyLamps_.end());
  std::sort(hsLampKeys_.begin(), hsLampKeys_.end());
  phase_.mark += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m0).count();
  populateWake();
}

void SignalSim::hsFinishCycle(ChunkMap& w) {
  if (fullPass_) allAwake_ = true;  // a full pass may change any wire: everyone ticks next
  if (fullPass_ && !netsValid_) buildNets(w);
  fullPass_ = false;
}

void SignalSim::hsCommit() {
  radio_.flip();    // this cycle's broadcasts become readable next cycle (1-tick wireless latency)
  jammers_.flip();  // this cycle's jammer zones become active
}

void SignalSim::wakeAround(int64_t k) {
  int x, y, z; unpackPos(k, x, y, z);
  for (int dy = -1; dy <= 2; ++dy)  // +2: a torch powers the block above it, read beside that block
    for (int dz = -1; dz <= 1; ++dz)
      for (int dx = -1; dx <= 1; ++dx) {
        const int64_t n = packPos(x + dx, y + dy, z + dz);
        if (comps_.contains(n)) awake_.insert(n);
      }
}

void SignalSim::wireLinks(const ChunkMap& w, int x, int y, int z, std::vector<int64_t>& out) const {
  const auto& reg = blockRegistry();
  const uint8_t channel = reg.def(w.get(x, y, z)).channel;
  const bool selfVia = isVia(w.get(x, y, z));
  for (int dy = -1; dy <= 1; dy += 2) {  // via links: straight up/down, same colour, lossless
    const BlockId nb = w.get(x, y + dy, z);
    if (isWire(nb) && reg.def(nb).channel == channel && (selfVia || isVia(nb))) out.push_back(packPos(x, y + dy, z));
  }
  if (selfVia) return;  // a via never links sideways (red and blue tubes can touch)
  const bool upBlocked = reg.has(w.get(x, y + 1, z), kCutsWire);
  for (auto& d : kH) {
    const int nx = x + d[0], nz = z + d[1];
    for (int dy = -1; dy <= 1; ++dy) {
      if (dy == 1 && upBlocked) continue;
      if (dy == -1 && reg.has(w.get(nx, y, nz), kCutsWire)) continue;
      const BlockDef nd = reg.def(w.get(nx, y + dy, nz));
      if (nd.role != SimRole::Wire || nd.channel != channel || isVia(w.get(nx, y + dy, nz))) continue;
      out.push_back(packPos(nx, y + dy, nz));
    }
  }
}

static CellState* statePtr(ChunkMap& w, int x, int y, int z) {
  if (y < 0 || y >= kWorldHeight) return nullptr;
  ChunkColumn* c = w.column(x >> 4, z >> 4);
  if (!c) return nullptr;
  SubChunk* s = c->subs[size_t(y >> 4)].get();
  return s ? &s->state[size_t(SubChunk::index(x & 15, y & 15, z & 15))] : nullptr;
}

void SignalSim::buildNets(ChunkMap& w) {
  wireNet_.clear(); nets_.clear(); netLamps_.clear(); srcReach_.clear();
  wireNet_.reserve(wires_.size());
  std::vector<int64_t> stack, links;
  // Canonical iteration order (packPos): network ids must not depend on
  // unordered_set bucket order (insertion/edit history).
  std::vector<int64_t> ordered(wires_.begin(), wires_.end());
  std::sort(ordered.begin(), ordered.end());
  for (int64_t start : ordered) {
    if (wireNet_.contains(start)) continue;
    const int32_t id = int32_t(nets_.size());
    nets_.emplace_back(); netLamps_.emplace_back();
    std::unordered_set<int64_t> lampSet;
    wireNet_[start] = id; stack.push_back(start);
    while (!stack.empty()) {
      const int64_t k = stack.back(); stack.pop_back();
      nets_[size_t(id)].push_back(k);
      int x, y, z; unpackPos(k, x, y, z);
      for (auto& d : k6) { const int64_t n = packPos(x + d[0], y + d[1], z + d[2]); if (lamps_.contains(n)) lampSet.insert(n); }
      links.clear(); wireLinks(w, x, y, z, links);
      for (int64_t n : links) if (wires_.contains(n) && wireNet_.emplace(n, id).second) stack.push_back(n);
    }
    netLamps_[size_t(id)].assign(lampSet.begin(), lampSet.end());
  }
  // Component input fast path: neighbour kinds and state pointers.
  for (const auto& reg = blockRegistry(); auto& [k, c] : comps_) {
    int x, y, z; unpackPos(k, x, y, z);
    c.fast = false;
    c.self = statePtr(w, x, y, z);
    if (!c.self) continue;
    bool ok = true;
    for (int d = 0; d < 4; ++d) {
      const int nx = x + kDirX[d], nz = z + kDirZ[d];
      const BlockId nb = w.get(nx, y, nz);
      const BlockDef nd = reg.def(nb);
      c.nb[d] = statePtr(w, nx, y, nz); c.ch[d] = 0;
      if (nd.role == SimRole::Wire) { c.kind[d] = isVia(nb) ? 3 : 1; c.ch[d] = nd.channel; }
      else if (nd.role == SimRole::Torch) { c.kind[d] = 2; c.ch[d] = nd.channel; }
      else if (comps_.contains(packPos(nx, y, nz)) || reg.hasAll(nb, kSolid | kOpaque | kPowerable)) c.kind[d] = 4;
      else c.kind[d] = 0;
      if ((c.kind[d] == 1 || c.kind[d] == 2) && !c.nb[d]) ok = false;
    }
    c.top = isFlatWire(w.get(x, y + 1, z)) ? statePtr(w, x, y + 1, z) : nullptr;
    c.fast = ok;
  }
  // Readers of each network: components beside one of its wires, or under one (top input).
  netComps_.assign(nets_.size(), {});
  for (size_t id = 0; id < nets_.size(); ++id) {
    std::vector<int64_t>& v = netComps_[id];
    for (int64_t k : nets_[id]) {
      int x, y, z; unpackPos(k, x, y, z);
      for (auto& d : kH) if (const int64_t n = packPos(x + d[0], y, z + d[1]); comps_.contains(n)) v.push_back(n);
      if (const int64_t n = packPos(x, y - 1, z); comps_.contains(n)) v.push_back(n);
    }
    std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end());
  }
  // Flat arrays: in-area wires only (out-of-area wires are frozen, exactly like the flood).
  const auto& reg = blockRegistry();
  netArr_.assign(nets_.size(), {});
  wireLoc_.clear();
  for (size_t id = 0; id < nets_.size(); ++id) {
    NetArr& a = netArr_[id];
    for (int64_t k : nets_[id]) {
      int x, y, z; unpackPos(k, x, y, z);
      if (!area_.contains(x, z)) continue;
      wireLoc_[k] = {int32_t(id), int32_t(a.keys.size())};
      a.keys.push_back(k); a.st.push_back(statePtr(w, x, y, z)); a.via.push_back(uint8_t(isVia(w.get(x, y, z))));
    }
    a.old.resize(a.keys.size()); a.stamp.assign(a.keys.size(), 0);
  }
  for (size_t id = 0; id < nets_.size(); ++id) {  // CSR links (all indices known now)
    NetArr& a = netArr_[id];
    a.adjStart.reserve(a.keys.size() + 1);
    for (int64_t k : a.keys) {
      a.adjStart.push_back(int32_t(a.adj.size()));
      int x, y, z; unpackPos(k, x, y, z);
      links.clear(); wireLinks(w, x, y, z, links);
      for (int64_t n : links)
        if (const auto it = wireLoc_.find(n); it != wireLoc_.end() && it->second.first == int32_t(id)) a.adj.push_back(it->second.second);
    }
    a.adjStart.push_back(int32_t(a.adj.size()));
  }
  // Lamps fed only by wires get a pointer fast path; anything that can be strongly powered or sits
  // next to a component keeps the generic path.
  lampFast_.clear();
  for (int64_t k : lamps_) {
    int x, y, z; unpackPos(k, x, y, z);
    const BlockId lb = w.get(x, y, z);
    if (reg.hasAll(lb, kSolid | kOpaque | kPowerable)) continue;
    LampFast f{statePtr(w, x, y, z), {}, reg.def(lb).role == SimRole::ColorLamp};
    bool ok = f.self != nullptr;
    for (auto& d : k6) {
      const int nx = x + d[0], ny = y + d[1], nz = z + d[2];
      const BlockId nb = w.get(nx, ny, nz);
      if (isFlatWire(nb)) { if (CellState* p = statePtr(w, nx, ny, nz)) f.wires.push_back(p); continue; }
      if (reg.has(nb, kPowerable) || comps_.contains(packPos(nx, ny, nz))) { ok = false; break; }
    }
    if (ok) lampFast_.emplace(k, std::move(f));
  }
  netColumns_ = w.columns().size();
  netMarkedBits_.assign((nets_.size() + 31) / 32, 0);
  netsValid_ = true;
}
void SignalSim::tickTorches(ChunkMap& w, const DirtyFn& dirty) {
  // 1) Torches: decide from the previous tick's state, then apply together.
  torchNext_.clear();
  torchNext_.reserve(torches_.size());
  for (int64_t k : torches_) {
    int x, y, z; unpackPos(k, x, y, z);
    if (!area_.contains(x, z)) continue;
    int sx, sy, sz;
    torchSupport(w.getState(x, y, z), x, y, z, sx, sy, sz);
    torchNext_.push_back({k, uint8_t(blockPowered(w, sx, sy, sz) ? 0 : 1)});  // a powered support turns it off
  }
  for (auto& [k, lit] : torchNext_) {
    int x, y, z; unpackPos(k, x, y, z);
    const CellState now = w.getState(x, y, z), next = withAttach(CellState(lit), stAttach(now));
    if (now != next) { w.setState(x, y, z, next); dirty(x, y, z); changedSrc_.push_back(k); }
  }
}

static CellState stateOf(const ChunkMap& w, int64_t k) { int x, y, z; unpackPos(k, x, y, z); return w.getState(x, y, z); }

namespace {
// Deterministic per-cell phase inside a divide period (rate scaling, JD
// 2026-10-14): divided components spread across the period instead of
// all firing on one tick — load spreading, the anti-hitch half of
// dividing. Pure function of the cell key: same world -> same phase on
// every run and platform. (Authority-registered components carry an
// explicit TickRate::phase instead; placed cells derive theirs here.)
uint32_t ratePhaseFor(int64_t key, uint32_t divide) {
  return uint32_t(litholattice::detMix64(uint64_t(key) + 0x9E3779B97F4A7C15ULL) % divide);
}
}  // namespace

litholattice::TickRate SignalSim::classTickRate(BlockId block, const ComponentBehavior* behavior) const {
  // An installed override was admitted only when valid and non-neutral
  // (setClassTickRate), so it needs no re-validation here.
  if (const auto it = classRates_.find(block); it != classRates_.end()) return it->second;
  // A-H2: the class-declared rate is validated once per BlockId and the
  // validated result cached (key lookup only — the cache is never
  // iterated, so no unordered order can reach execution).
  if (const auto it = declaredRates_.find(block); it != declaredRates_.end()) return it->second;
  const litholattice::TickRate declared = behavior ? behavior->tickRate() : litholattice::TickRate{};
  litholattice::TickRate resolved = declared;
  if (!declared.valid()) {
    // Logged exactly once per class: after this, the cached neutral
    // result is what every later resolution returns.
    litholattice::logError(
        "signal: invalid class tick rate for block %u (multiply %u, divide %u) — substituting the neutral rate; "
        "a class rate must be powers of two with multiply <= %u and divide <= %u (review A-H2)",
        unsigned(block), declared.multiply, declared.divide, litholattice::kMaxRateMultiply,
        litholattice::kMaxRateDivide);
    resolved = litholattice::TickRate{};
  }
  declaredRates_[block] = resolved;
  return resolved;
}

void SignalSim::tickComponents(ChunkMap& w, const DirtyFn& dirty) {
  // 2) Components: new outputs from the previous tick's inputs (computed first, applied together).
  struct Next { int64_t k; Comp* c; ComponentStep step; };
  std::vector<Next> compNext;
  compNext.reserve(comps_.size());
  const bool all = allAwake_, fastInputs = netsValid_ && !fullPass_;
  lastTicked_ = 0;
  // B-L1 (P3-2): iterate a SORTED key snapshot, not the unordered_map.
  // comps_ iteration order is hash order — it leaks into externally
  // visible ordering: compNext's apply order below is the order sounds_
  // entries are appended and radio_ broadcasts are published. Sorted
  // (packPos) order makes both deterministic and identical across runs
  // and paths. Component INPUTS are all read before any apply, so the
  // iteration order cannot change any component's result — only the
  // publish/sound order, which is exactly what this fixes.
  std::vector<int64_t> compOrder;
  compOrder.reserve(comps_.size());
  for (const auto& [k, c] : comps_) compOrder.push_back(k);
  std::sort(compOrder.begin(), compOrder.end());
  for (int64_t k : compOrder) {
    Comp& c = comps_.find(k)->second;
    if (!all && !c.rateHeld && c.behavior->quiescent(stateOf(w, k), c.mem) && !awake_.contains(k)) continue;
    int x, y, z; unpackPos(k, x, y, z);
    if (!area_.contains(x, z)) continue;
    // Rate scaling (JD 2026-10-14): the class's effective tick rate.
    // divide > 1: this cell is pulled only on its phase tick of the
    // period; on other ticks it HOLDS — its published outputs (mem.out)
    // are read by the propagate phase exactly as before, so readers
    // always sample published state, never a half-advanced one.
    // multiply > 1 is applied at the behaviour calls below. Every
    // shipped class is neutral, so this gate never skips shipped content.
    const litholattice::TickRate rate = classTickRate(w.get(x, y, z), c.behavior);
    if (rate.divide > 1 && (tickNo_ % rate.divide) != uint64_t(ratePhaseFor(k, rate.divide))) {
      c.rateHeld = true;  // component sleep must not swallow the due tick
      continue;
    }
    c.rateHeld = false;
    ++lastTicked_;
    ComponentInputs in;
    const SimRole role = roleOf(blockRegistry().def(w.get(x, y, z)));
    if (role == SimRole::SignalReceivor || role == SimRole::Dish) {
      int rs = 0, rc = 0;
      const int sens = role == SimRole::Dish ? kDishSensitivity : 0;
      if (!jammers_.jammed(x, y, z, c.mem.channel) &&
          radio_.query(c.mem.channel, x, y, z, dim_, sens, rs, rc)) { in.radioIn = rs; in.radioCol = uint8_t(rc); }
    }
    if (role == SimRole::Overclockor && c.mem.jammer)
      jammers_.publish(JammerZone{x, y, z, kJammerRadius, c.mem.channel});
    if (fastInputs && c.fast) {
      for (int d = 0; d < 4; ++d) {
        switch (c.kind[d]) {
          case 1: in.side[d] = std::min(stValue(*c.nb[d]), kMaxSignal); in.ch[d] = c.ch[d]; break;  // SW-H2 clamp (see inputFrom)
          case 2: in.side[d] = stValue(*c.nb[d]) ? kMaxSignal : 0; in.ch[d] = c.ch[d]; break;
          case 3: in.ch[d] = c.ch[d]; break;
          case 4: in.side[d] = inputFrom(w, x, y, z, d); break;
          default: break;
        }
      }
      if (c.top) in.top = std::min(stValue(*c.top), kMaxSignal);  // SW-H2 clamp
      ComponentStep step{};
      // A-H3: chain CellState across sub-pulls — each sub-pull receives
      // the previous sub-pull's step.state, so sub-pulls form a true
      // recurrence in (mem, state). Without this, a state-carrying
      // behaviour (Memor: stored value lives in the state argument)
      // re-read the pre-tick state on every sub-pull and its final step
      // discarded every store made by earlier sub-pulls. Mem-only
      // behaviours are unaffected: their recurrence is in c.mem, which
      // already chained. Only the final step is published (apply loop).
      CellState cur = *c.self;
      for (uint32_t s = 0; s < rate.multiply; ++s) { step = c.behavior->tick(in, cur, c.mem); cur = step.state; }
      compNext.push_back({k, &c, step});
      continue;
    }
    for (int d = 0; d < 4; ++d) {
      in.side[d] = inputFrom(w, x, y, z, d);
      const BlockDef nd = blockRegistry().def(w.get(x + kDirX[d], y, z + kDirZ[d]));
      if (nd.role == SimRole::Wire || nd.role == SimRole::Torch) in.ch[d] = nd.channel;
    }
    if (isFlatWire(w.get(x, y + 1, z))) in.top = std::min(stValue(w.getState(x, y + 1, z)), kMaxSignal);  // SW-H2 clamp
    ComponentStep step{};
    // A-H3: same CellState chaining as the fast path above.
    CellState cur = w.getState(x, y, z);
    for (uint32_t s = 0; s < rate.multiply; ++s) { step = c.behavior->tick(in, cur, c.mem); cur = step.state; }
    compNext.push_back({k, &c, step});
  }
  for (const auto& [k, cp, step] : compNext) {
    ComponentMemory& m = cp->mem;
    // SW-H2 defense-in-depth at the store point: ComponentStep.out is documented
    // 0..64 but nothing enforced it; clamp TO kMaxSignal so a behaviour fed an
    // out-of-range value (or a future behaviour bug) can never write a strength
    // that later indexes buckets_[s] out of bounds. In-range values are untouched.
    const int out = std::clamp(step.out, 0, kMaxSignal), out2 = std::clamp(step.out2, 0, kMaxSignal);
    if (m.out != out || m.out2 != out2 || m.pulse != step.pulse) changedSrc_.push_back(k);
    m.out = uint8_t(out); m.out2 = uint8_t(out2); m.pulse = step.pulse;
    int x, y, z; unpackPos(k, x, y, z);
    if (w.getState(x, y, z) != step.state) { w.setState(x, y, z, step.state); dirty(x, y, z); }
    if (step.sound) sounds_.push_back({*step.sound, stValue(step.state), x, y, z});
    // Wireless: transmittors broadcast (strength = mem.out, colour = mem.out2), unless jammed.
    if (blockRegistry().def(w.get(x, y, z)).role == SimRole::SignalTransmittor && m.out > 0 &&
        m.channel >= 1 && m.channel <= radioChannelsUnlocked() &&
        !jammers_.jammed(x, y, z, m.channel))
      radio_.publish(RadioBroadcast{x, y, z, dim_, m.channel, m.out, m.out2, kTransmittorPower});
  }
}

namespace {
// RAII ns timer into an atomic accumulator (hs* methods time themselves so
// takePhaseMs splits are real on the pull-worker path too).
struct HsTimer {
  std::atomic<uint64_t>& acc;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  ~HsTimer() {
    acc.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - t0).count()),
                  std::memory_order_relaxed);
  }
};
}  // namespace

// ---- propagate: full pass (affine; the legacy propagateWires fullPass_ branch) ----

void SignalSim::hsPropagateFull(ChunkMap& w, const DirtyFn& dirty) {
  HsTimer timer{hsWiresNs_};
  const auto& reg = blockRegistry();
  // 3) Wires: reset, then bucket BFS from all sources (strongest first => max strength wins).
  std::vector<std::pair<int64_t, CellState>> old;
  old.reserve(wires_.size());
  auto resetWire = [&](int64_t k) {
    int x, y, z; unpackPos(k, x, y, z);
    if (!area_.contains(x, z)) return;
    old.push_back({k, w.getState(x, y, z)});
    w.setState(x, y, z, 0);
  };
  for (int64_t k : wires_) resetWire(k);
  for (auto& b : buckets_) b.clear();
  for (auto& b : nbuckets_) b.clear();
  auto seed = [&](int x, int y, int z, int s, uint8_t channel) {  // channel 0 = either colour
    const BlockDef d = reg.def(w.get(x, y, z));
    // SW-H2: guard the high end too — buckets_ has kMaxSignal + 1 entries, so
    // s in 65..255 (reachable via a disk-loaded value before the input clamps)
    // was an out-of-bounds array access.
    if (d.role != SimRole::Wire || isVia(w.get(x, y, z)) || (channel && d.channel != channel) || s <= 0 || s > kMaxSignal) return;
    if (stValue(w.getState(x, y, z)) >= s || !area_.contains(x, z)) return;
    w.setState(x, y, z, CellState(s));
    buckets_[s].push_back(packPos(x, y, z));
  };
  for (auto& [k, lit] : torchNext_) {
    if (!lit) continue;
    int x, y, z; unpackPos(k, x, y, z);
    const uint8_t ch = reg.def(w.get(x, y, z)).channel;  // red torch -> red trace only
    for (auto& d : kH) seed(x + d[0], y, z + d[1], kMaxSignal, ch);
    if (strongPowered(w, x, y + 1, z))  // torch-powered block: colourless power to all 6 sides
      for (auto& d : k6) seed(x + d[0], y + 1 + d[1], z + d[2], kMaxSignal, 0);
  }
  std::vector<std::pair<int64_t, int>> pulses;
  // B-L1 acceptance (P3-2): this comps_ iteration stays unordered BY
  // DESIGN — it only SEEDS buckets/pulses. Bucket BFS resolves by max
  // strength and pulses flood a fixed strength, so the final wire states
  // are independent of seed order; nothing externally ordered (sounds,
  // radio) is emitted here. (tickComponents, which does emit those, uses
  // a sorted snapshot — see there.)
  for (const auto& [k, c] : comps_) {  // components drive either colour; pulses are flooded below
    if (!c.mem.out && !c.mem.out2) continue;
    int x, y, z; unpackPos(k, x, y, z);
    if (!area_.contains(x, z)) continue;
    const CellState st = w.getState(x, y, z);
    if (const int face = stAttach(st); face && (reg.def(w.get(x, y, z)).role == SimRole::Switch || reg.def(w.get(x, y, z)).role == SimRole::Button)) {
      // Attached switch: powers its support (colourless, all 6 sides, like a torch-powered
      // block) and, mounted on top, the dust directly below the support (two-block reach).
      int ox, oy, oz;
      attachOffset(face, ox, oy, oz);
      const int sx = x + ox, sy = y + oy, sz = z + oz;
      for (auto& d : k6) seed(sx + d[0], sy + d[1], sz + d[2], kMaxSignal, 0);
      if (face == 1) seed(x, y - 2, z, kMaxSignal, 0);
      continue;
    }
    for (int d = 0; d < 4; ++d) {
      const int s = c.behavior->emitToward(d, st, c.mem);
      if (s <= 0) continue;
      if (c.mem.pulse) pulses.push_back({packPos(x + kDirX[d], y, z + kDirZ[d]), s});
      else seed(x + kDirX[d], y, z + kDirZ[d], s, uint8_t(c.behavior->emitChannel(d, st, c.mem)));
      // Hard-powered block in front of a Repeater: colourless power to its other five faces.
      if (!c.mem.pulse && mountPowered_.contains(packPos(x + kDirX[d], y, z + kDirZ[d])) && reg.def(w.get(x, y, z)).role == SimRole::Repeater)
        for (auto& e : k6) seed(x + kDirX[d] + e[0], y + e[1], z + kDirZ[d] + e[2], s, 0);
    }
  }
  // Neighbours of a wire cell along the run (same channel; one-block stairs).
  auto forEachLink = [&](int x, int y, int z, auto&& fn) {
    const uint8_t channel = reg.def(w.get(x, y, z)).channel;
    const bool selfVia = isVia(w.get(x, y, z));
    for (int dy = -1; dy <= 1; dy += 2) {  // via links (vertical, lossless)
      const BlockId nb = w.get(x, y + dy, z);
      if (isWire(nb) && reg.def(nb).channel == channel && (selfVia || isVia(nb))) fn(x, y + dy, z);
    }
    if (selfVia) return;
    const bool upBlocked = reg.has(w.get(x, y + 1, z), kCutsWire);
    for (auto& d : kH) {
      const int nx = x + d[0], nz = z + d[1];
      for (int dy = -1; dy <= 1; ++dy) {
        if (dy == 1 && upBlocked) continue;        // climbing needs headroom above this wire
        if (dy == -1 && reg.has(w.get(nx, y, nz), kCutsWire)) continue;  // descending past a block edge
        const BlockDef nd = reg.def(w.get(nx, y + dy, nz));
        if (nd.role != SimRole::Wire || nd.channel != channel || isVia(w.get(nx, y + dy, nz))) continue;  // red never meets blue
        if (!area_.contains(nx, nz)) continue;
        fn(nx, y + dy, nz);
      }
    }
  };
  for (int s = kMaxSignal; s >= 1; --s)
    for (size_t i = 0; i < buckets_[s].size(); ++i) {
      int x, y, z; unpackPos(buckets_[s][i], x, y, z);
      if (stValue(w.getState(x, y, z)) != s) continue;  // superseded by a stronger source
      const bool selfVia = isVia(w.get(x, y, z));
      forEachLink(x, y, z, [&](int nx, int ny, int nz) {
        const int ns = (selfVia || isVia(w.get(nx, ny, nz))) ? s : s - 1;  // vias are lossless
        if (ns <= 0 || stValue(w.getState(nx, ny, nz)) >= ns) return;
        w.setState(nx, ny, nz, CellState(ns));
        // B-M4 (P3-2): NO direct strengthBuf_ write here any more. The
        // incremental path records only FINAL changed cells (per-net
        // pending lists, replayed by hsApplyStrengthPending); this loop
        // wrote every transient BFS improvement, so the full pass's
        // buffer writes diverged from the incremental path's. The final
        // loop below now records the same changed-cell set instead.
        buckets_[ns].push_back(packPos(nx, ny, nz));
      });
    }
  // Pulsor pulses: the whole connected run (up to 64 blocks away) carries the data strength, no decay.
  for (auto& [k0, s] : pulses) {
    if (s <= 0 || s > kMaxSignal) continue;  // SW-H2: out-of-range pulse strengths never enter a cell
    int x, y, z; unpackPos(k0, x, y, z);
    if (!isFlatWire(w.get(x, y, z)) || !inScope(k0)) continue;
    std::vector<int64_t> frontier{k0};
    std::unordered_set<int64_t> seen{k0};
    for (int dist = 0; dist < kMaxSignal && !frontier.empty(); ++dist) {
      std::vector<int64_t> next;
      for (size_t fi = 0; fi < frontier.size(); ++fi) {  // via hops cost no reach: same frontier
        int a, b, c; unpackPos(frontier[fi], a, b, c);
        if (stValue(w.getState(a, b, c)) < s) w.setState(a, b, c, CellState(s));
        const bool selfVia = isVia(w.get(a, b, c));
        forEachLink(a, b, c, [&](int nx, int ny, int nz) {
          const int64_t nk = packPos(nx, ny, nz);
          if (seen.insert(nk).second) (selfVia || isVia(w.get(nx, ny, nz)) ? frontier : next).push_back(nk);
        });
      }
      frontier.swap(next);
    }
  }
  // B-M4 (P3-2): record the FINAL changed-cell set through the same
  // pending-list mechanism the incremental path uses (NetWork::strength +
  // hsApplyStrengthPending): every wire whose end-of-pass state differs
  // from its pre-pass state, in `old` (wire visit) order, with its final
  // strength. Seeds, BFS improvements and pulse floods all terminate in
  // these cells, so this set is exactly the incremental path's set.
  std::vector<std::pair<int64_t, uint8_t>> strengthPending;
  for (auto& [k, prev] : old) {
    int x, y, z; unpackPos(k, x, y, z);
    if (w.getState(x, y, z) != prev) {
      dirty(x, y, z);
      strengthPending.push_back({k, uint8_t(stValue(w.getState(x, y, z)))});
    }
  }
  for (const auto& [key, s] : strengthPending) strengthBuf_.set(strengthBuf_.slotFor(uint64_t(key)), s);
}

// ---- propagate: incremental prep (affine; reset + record per-net work) ----
// The seed/pulse loops below are the legacy propagateWires loops, except the
// effects are RECORDED per net (in the exact legacy visit order) instead of
// applied into the shared bucket arrays: the shared nbuckets_ and the
// TraceStrengthBuffer are the two thread hazards of the legacy code
// (TraceStrengthBuffer::set is NOT a plain slot store — it mutates a shared
// dirty-word bitset, and slotFor() mutates the slot map — see
// hs-integration-notes). hsPropagateNet then applies a net's recorded seeds
// to net-local buckets, so the per-net result is the legacy result (cross-net
// interleaving in the legacy global bucket loop cannot change a net's final
// state: nets are disjoint, and a net's entries keep their relative order).

void SignalSim::hsPrepIncremental(ChunkMap& w) {
  HsTimer timer{hsWiresNs_};
  const auto& reg = blockRegistry();
  if (hsNetWork_.size() != netArr_.size()) hsNetWork_.assign(netArr_.size(), NetWork{});
  for (int32_t id : hsMarkedNets_) {
    NetWork& nw = hsNetWork_[size_t(id)];
    nw.seeds.clear(); nw.pulses.clear(); nw.strength.clear();
    NetArr& a = netArr_[size_t(id)];
    for (size_t i = 0; i < a.st.size(); ++i) { a.old[i] = *a.st[i]; *a.st[i] = 0; }
  }
  for (auto& b : nbuckets_) b.clear();  // legacy hygiene (the global buckets are unused by the per-net flood)
  auto seed = [&](int x, int y, int z, int s, uint8_t channel) {  // channel 0 = either colour
    const BlockDef d = reg.def(w.get(x, y, z));
    // SW-H2: same high-end guard as the full-pass seed (the recorded strength
    // is later applied via lb[size_t(s)] in hsPropagateNet, sized kMaxSignal + 1).
    if (d.role != SimRole::Wire || isVia(w.get(x, y, z)) || (channel && d.channel != channel) || s <= 0 || s > kMaxSignal) return;
    const auto it = wireLoc_.find(packPos(x, y, z));
    if (it == wireLoc_.end() || !isNetMarked(it->second.first)) return;
    hsNetWork_[size_t(it->second.first)].seeds.push_back({it->second.second, s});
  };
  for (auto& [k, lit] : torchNext_) {
    if (!lit) continue;
    int x, y, z; unpackPos(k, x, y, z);
    const uint8_t ch = reg.def(w.get(x, y, z)).channel;  // red torch -> red trace only
    for (auto& d : kH) seed(x + d[0], y, z + d[1], kMaxSignal, ch);
    if (strongPowered(w, x, y + 1, z))  // torch-powered block: colourless power to all 6 sides
      for (auto& d : k6) seed(x + d[0], y + 1 + d[1], z + d[2], kMaxSignal, 0);
  }
  // B-L1 acceptance (P3-2): unordered comps_ iteration is safe here for
  // the same reason as in hsPropagateFull — this loop only RECORDS seeds
  // and pulses into per-net work lists; each net's flood resolves by max
  // strength independent of record order across nets, and nets are
  // disjoint. No externally visible ordering originates here.
  for (const auto& [k, c] : comps_) {  // components drive either colour; pulses are flooded per net
    if (!c.mem.out && !c.mem.out2) continue;
    int x, y, z; unpackPos(k, x, y, z);
    if (!area_.contains(x, z)) continue;
    const CellState st = w.getState(x, y, z);
    if (const int face = stAttach(st); face && (reg.def(w.get(x, y, z)).role == SimRole::Switch || reg.def(w.get(x, y, z)).role == SimRole::Button)) {
      int ox, oy, oz;
      attachOffset(face, ox, oy, oz);
      const int sx = x + ox, sy = y + oy, sz = z + oz;
      for (auto& d : k6) seed(sx + d[0], sy + d[1], sz + d[2], kMaxSignal, 0);
      if (face == 1) seed(x, y - 2, z, kMaxSignal, 0);
      continue;
    }
    for (int d = 0; d < 4; ++d) {
      const int s = c.behavior->emitToward(d, st, c.mem);
      if (s <= 0) continue;
      if (c.mem.pulse) {
        const int64_t target = packPos(x + kDirX[d], y, z + kDirZ[d]);
        const auto it = wireLoc_.find(target);
        if (it != wireLoc_.end() && isNetMarked(it->second.first) &&
            !netArr_[size_t(it->second.first)].via[size_t(it->second.second)])
          hsNetWork_[size_t(it->second.first)].pulses.push_back({it->second.second, s});
      } else seed(x + kDirX[d], y, z + kDirZ[d], s, uint8_t(c.behavior->emitChannel(d, st, c.mem)));
      // Hard-powered block in front of a Repeater: colourless power to its other five faces.
      if (!c.mem.pulse && mountPowered_.contains(packPos(x + kDirX[d], y, z + kDirZ[d])) && reg.def(w.get(x, y, z)).role == SimRole::Repeater)
        for (auto& e : k6) seed(x + kDirX[d] + e[0], y + e[1], z + kDirZ[d] + e[2], s, 0);
    }
  }
}

// ---- propagate: one net's flood (pull-safe; touches only this net's NetArr
// span and its own NetWork record). Never touches ChunkMap structure, the
// shared buckets, slotFor/TraceStrengthBuffer, or the real DirtyFn (the
// incremental flood reports through the strength buffer only, as legacy).

void SignalSim::hsPropagateNet(ChunkMap& /*w*/, int32_t netId, const DirtyFn& /*dirty*/) {
  HsTimer timer{hsWiresNs_};
  NetArr& a = netArr_[size_t(netId)];
  NetWork& nw = hsNetWork_[size_t(netId)];
  thread_local std::array<std::vector<int32_t>, kMaxSignal + 1> lb;
  for (auto& b : lb) b.clear();
  // Apply this net's recorded seeds, in legacy seed order, to net-local buckets.
  for (const auto& [li, s] : nw.seeds) {
    if (s <= 0 || s > kMaxSignal) continue;  // SW-H2 defense-in-depth: lb is sized kMaxSignal + 1
    CellState* p = a.st[size_t(li)];
    if (stValue(*p) >= s) continue;
    *p = CellState(s);
    lb[size_t(s)].push_back(li);
  }
  for (int s = kMaxSignal; s >= 1; --s)
    for (size_t i = 0; i < lb[size_t(s)].size(); ++i) {  // size re-read: lossless via links append to s
      const int32_t li = lb[size_t(s)][i];
      if (stValue(*a.st[size_t(li)]) != s) continue;
      for (int32_t e = a.adjStart[size_t(li)]; e < a.adjStart[size_t(li) + 1]; ++e) {
        const int32_t nj = a.adj[size_t(e)];
        const int ns = (a.via[size_t(li)] || a.via[size_t(nj)]) ? s : s - 1;
        CellState* p = a.st[size_t(nj)];
        if (ns <= 0 || stValue(*p) >= ns) continue;
        *p = CellState(ns);
        lb[size_t(ns)].push_back(nj);
      }
    }
  thread_local std::vector<int32_t> frontier, next;
  for (const auto& [li0, s] : nw.pulses) {
    if (s <= 0 || s > kMaxSignal) continue;  // SW-H2: never write an out-of-range strength into a cell
    const uint32_t gen = ++a.pulseGen;
    frontier.assign(1, li0); a.stamp[size_t(li0)] = gen;
    for (int dist = 0; dist < kMaxSignal && !frontier.empty(); ++dist) {
      next.clear();
      for (size_t fi = 0; fi < frontier.size(); ++fi) {  // via hops cost no reach: same frontier
        const int32_t li = frontier[fi];
        if (stValue(*a.st[size_t(li)]) < s) *a.st[size_t(li)] = CellState(s);
        for (int32_t e = a.adjStart[size_t(li)]; e < a.adjStart[size_t(li) + 1]; ++e) {
          const int32_t nj = a.adj[size_t(e)];
          if (a.stamp[size_t(nj)] == gen) continue;
          a.stamp[size_t(nj)] = gen;
          (a.via[size_t(li)] || a.via[size_t(nj)] ? frontier : next).push_back(nj);
        }
      }
      frontier.swap(next);
    }
  }
  nw.strength.clear();
  for (size_t i = 0; i < a.st.size(); ++i)
    if (*a.st[i] != a.old[i]) {
      // Step 1: strength -> buffer slot, no remesh. Deferred: slotFor/set are
      // not thread-safe; hsApplyStrengthPending replays these on the sim
      // thread in canonical (net, cell) order — the legacy final-loop order.
      nw.strength.push_back({a.keys[i], uint8_t(stValue(*a.st[i]))});
    }
}

void SignalSim::hsApplyStrengthPending() {
  HsTimer timer{hsWiresNs_};
  for (int32_t id : hsMarkedNets_) {
    NetWork& nw = hsNetWork_[size_t(id)];
    for (const auto& [key, s] : nw.strength) strengthBuf_.set(strengthBuf_.slotFor(uint64_t(key)), s);
    nw.strength.clear();
  }
}

// ---- lamps ----

std::vector<SignalSim::LampHoldUpdate>& SignalSim::hsTlsLampHolds() {
  thread_local std::vector<LampHoldUpdate> v;
  return v;
}

void SignalSim::hsApplyLampHolds(const std::vector<LampHoldUpdate>& updates) {
  for (const LampHoldUpdate& u : updates) {
    if (u.erase) lampHold_.erase(u.key);
    else {
      lampHold_[u.key] = u.end;
      nextHoldExpiry_ = std::min(nextHoldExpiry_, u.end);
    }
  }
}

void SignalSim::hsLampsBatch(const std::vector<int64_t>& keys, ChunkMap& w, const DirtyFn& dirty) {
  HsTimer timer{hsLampsNs_};
  const auto& reg = blockRegistry();
  std::vector<LampHoldUpdate>& holds = hsTlsLampHolds();
  holds.clear();
  // 4) Lamps (terminate the signal): plain lamps light on any adjacent power; color lamps take
  //    the strongest adjacent strength (palette index = (s-1) mod N in the renderer).
  // lampHold_ is read here (concurrent reads are safe: nobody writes it during
  // a batch) but never written: hold updates go to the thread-local list and
  // are merged by the sim thread via hsApplyLampHolds (hs-design §3.3).
  auto one = [&](int64_t k) {
    int x, y, z; unpackPos(k, x, y, z);
    if (!area_.contains(x, z)) return;
    int v = 0;
    const LampFast* fast = nullptr;
    if (!fullPass_) if (const auto it = lampFast_.find(k); it != lampFast_.end()) fast = &it->second;
    if (fast) { for (const CellState* p : fast->wires) v = std::max(v, int(stValue(*p))); }
    else {
    v = strongPowered(w, x, y, z) ? kMaxSignal : 0;
    for (auto& d : k6) {
      const int nx = x + d[0], ny = y + d[1], nz = z + d[2];
      if (isFlatWire(w.get(nx, ny, nz))) v = std::max(v, stValue(w.getState(nx, ny, nz)));
      else if (strongPowered(w, nx, ny, nz)) v = kMaxSignal;
    }
    for (int d = 0; d < 4; ++d) {
      const int nx = x + kDirX[d], nz = z + kDirZ[d];
      v = std::max(v, emitToward(w, nx, y, nz, dirBack(d)));
    }
    }
    if (lampHoldTicks_ > 0) {  // afterglow: hold the last lit colour for lampHoldTicks_ ticks
      if (v > 0) { holds.push_back({k, tickNo_ + uint64_t(lampHoldTicks_), false}); }
      else if (auto h = lampHold_.find(k); h != lampHold_.end()) {
        if (h->second > tickNo_) { holds.push_back({k, h->second, false}); return; }  // still glowing (legacy also re-affirms nextHoldExpiry_)
        holds.push_back({k, 0, true});  // expired: fall through and go dark
      }
    }
    if (fast) {
      const CellState ns = fast->color ? makeState(v, 0, 0, v > 0) : CellState(v > 0 ? 1 : 0);
      if (*fast->self != ns) { *fast->self = ns; dirty(x, y, z); }
      return;
    }
    const bool color = reg.def(w.get(x, y, z)).role == SimRole::ColorLamp;
    const CellState ns = color ? makeState(v, 0, 0, v > 0) : CellState(v > 0 ? 1 : 0);
    if (w.getState(x, y, z) != ns) { w.setState(x, y, z, ns); dirty(x, y, z); }
  };
  for (int64_t k : keys) one(k);
}

}  // namespace lithokraft
