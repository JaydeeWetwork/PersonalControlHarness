// Signal simulation (sim thread only). Per world tick:
//  1) torches update from the power state of the block they sit on (one-tick inverter delay),
//  2) components (repeater, thresholder, redirector, pulsor, memory cell, fast latch, switch,
//     button, keyboard) compute their outputs from the previous tick's inputs (1-tick delay),
//  3) wire strengths are recomputed by a bucket BFS from every source (torches into their own
//     colour, components and torch-powered blocks into either colour; -1 per block; Pulsor
//     pulses travel the connected run without decay, up to 64 blocks),
//  4) lamps light / color lamps take the strongest adjacent signal. Lamps terminate signals.
// Only component cells are visited; plain terrain costs nothing. Roles come from the BlockRegistry;
// component logic lives in one ComponentBehavior class per block type (litholattice/sim/component.h).
#pragma once
#include <atomic>
#include <map>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <bit>
#include <vector>
#include "litholattice/render/trace_strength_buffer.h"
#include "lithokraft/engine.h"
#include "lithokraft/content/component.h"
#include "litholattice/world/chunk_map.h"

namespace lithokraft {

constexpr int kButtonTicks = 10;    // button pulse length
constexpr int kPulsorAnimTicks = 7; // firing animation flag hold (renderer plays the sidecar strip)

// DEC SIXBIT-style 64-char set: code = ASCII - 32 for ' '..'_' (lowercase folds to uppercase).
// Returns -1 for characters outside the set. Keyboard pulses strength code + 1.
int sixbitCode(char32_t c);
char sixbitChar(int code);
// Color lamp palette index for strength s on an N-colour tier (WRAP mapping; -1 = off).
constexpr int colorLampIndex(int s, int n) { return s <= 0 ? -1 : (s - 1) % n; }
// Keyboard control-port codes (right side).
constexpr int kKeyEnter = 1, kKeyBackspace = 2;

/// A sound the sim wants played: cue, strength (Tone pitch / Pulsor data) and the cell position.
struct SoundEvent {
  SoundCue cue;
  int value;
  int x, y, z;
};

/// One wireless broadcast: a transmittor's output on a channel.
/// Link budget: a receivor hears it when dist <= power + receivor sensitivity.
struct RadioBroadcast {
  int x = 0, y = 0, z = 0;
  uint8_t dim = 1;       ///< dimension index (1 = Wafer/overworld)
  uint8_t channel = 1;   ///< 1..64 (0 = the Daemon, reserved)
  uint8_t strength = 0;  ///< 0..64
  uint8_t colour = 0;    ///< 1 red, 2 blue
  uint16_t power = 0;    ///< broadcast power in blocks
};

/// The wireless signal registry (game-side, sim thread only).
/// Transmittors publish during the component tick; receivors read the previous tick's
/// broadcasts (1-tick latency). Only loaded chunks are indexed: unload = silent.
class RadioRegistry {
 public:
  void publish(RadioBroadcast b) { next_.push_back(b); }
  /// End-of-tick: this tick's broadcasts become readable next tick.
  void flip() {
    cur_.swap(next_);
    next_.clear();
    // Rebuild the per-channel index.
    for (auto& v : byChannel_) v.clear();
    for (const RadioBroadcast& b : cur_)
      if (b.channel < 65) byChannel_[b.channel].push_back(&b);
  }
  void clear() { cur_.clear(); next_.clear(); for (auto& v : byChannel_) v.clear(); }
  /// Strongest broadcast on `channel` within (power + sensitivity) of (x, y, z).
  /// Returns false when nothing is heard. Tie-breaks are canonical: strongest
  /// wins, then nearest, then the lowest packPos — so the result never depends on
  /// broadcast registration order.
  bool query(uint8_t channel, int x, int y, int z, int dim, int sensitivity,
             int& strength, int& colour) const {
    if (channel >= 65) return false;
    bool heard = false;
    int best = -1;
    int64_t bestD2 = 0;
    int64_t bestPos = 0;
    for (const RadioBroadcast* bp : byChannel_[channel]) {
      const RadioBroadcast& b = *bp;
      if (b.strength == 0) continue;
      const int64_t dx = int64_t(b.x) - x, dy = int64_t(b.y) - y, dz = int64_t(b.z) - z;
      // Cross-dimensional: the signal attenuates (half range). The Daemon (ch 0) hears all.
      int64_t reach = int64_t(b.power) + sensitivity;
      if (channel != 0 && b.dim != dim) reach /= 2;
      const int64_t d2 = dx * dx + dy * dy + dz * dz;
      if (d2 > reach * reach) continue;
      const int64_t pos = packPos(b.x, b.y, b.z);
      if (!heard || b.strength > best ||
          (b.strength == best && (d2 < bestD2 || (d2 == bestD2 && pos < bestPos)))) {
        heard = true; best = b.strength; bestD2 = d2; bestPos = pos;
        strength = b.strength; colour = b.colour;
      }
    }
    return heard;
  }

 private:
  std::vector<RadioBroadcast> cur_, next_;
  std::array<std::vector<const RadioBroadcast*>, 65> byChannel_;  ///< channel -> broadcasts
};

/// A jammer zone: blocks all radio channels except its own within `radius`.
struct JammerZone {
  int x = 0, y = 0, z = 0;
  int radius = 0;
  uint8_t channel = 1;  ///< the one channel allowed through
};

/// Active jammers, same one-tick-latency discipline as RadioRegistry.
class JammerRegistry {
 public:
  void publish(JammerZone j) { next_.push_back(j); }
  void flip() { cur_.swap(next_); next_.clear(); }
  void clear() { cur_.clear(); next_.clear(); }
  /// True when (x, y, z) is inside a jammer whose allowed channel isn't `channel`.
  bool jammed(int x, int y, int z, uint8_t channel) const {
    for (const JammerZone& j : cur_) {
      if (j.channel == channel) continue;  // the allowed channel passes through
      const int64_t dx = int64_t(j.x) - x, dy = int64_t(j.y) - y, dz = int64_t(j.z) - z;
      if (dx * dx + dy * dy + dz * dz <= int64_t(j.radius) * j.radius) return true;
    }
    return false;
  }

 private:
  std::vector<JammerZone> cur_, next_;
};

class SignalSim {
 public:
  using DirtyFn = std::function<void(int x, int y, int z)>;  // a cell's look changed

  void scan(const ChunkMap& w);                       // index all components (after worldgen)
  /// Index ONE column's cells (SW-H1): the streaming path inserts disk-loaded
  /// columns after the one full scan() at dimension load, so each inserted
  /// column is scanned on insertion (sim stopped), then its SGNL payload is
  /// importState()d. Existing indexes for other columns are untouched. The
  /// column must be newly inserted (no stale entries can exist for it).
  void scanColumn(const ChunkMap& w, int cx, int cz);
  /// Column eviction (A2 / B-M1): drops every indexed cell of ONE column
  /// (torches / wires / lamps / components, plus lamp holds and wake
  /// entries), zeroes the removed wires' trace-strength slots, and forces
  /// a network rebuild (fullPass) so no derived net/pointer state can
  /// reference the column again. The caller MUST take the column's SGNL
  /// export first (exportColumnState() / exportState()) and save it
  /// before unscanning when the state must survive — after this call the
  /// column's component memory is gone from the sim. Sim stopped only,
  /// like scanColumn().
  void unscanColumn(int cx, int cz);
  /// This column's slice of exportState() (empty when it holds no
  /// components): the SGNL bytes a save-on-evict must persist BEFORE
  /// unscanColumn() drops them.
  std::vector<uint8_t> exportColumnState(int cx, int cz) const;
  /// True when the sim indexes any cell in the column (torch / wire /
  /// lamp / component). Eviction treats such a column as save-first even
  /// when its ChunkColumn::dirty flag is clear (sim fast paths write cell
  /// states through cached pointers, bypassing ChunkMap::setState).
  bool hasIndexedCells(int cx, int cz) const;
  /// Number of indexed cells in the column (tests / diagnostics).
  size_t indexedCellCount(int cx, int cz) const;
  void onBlockChanged(int x, int y, int z, BlockId now);
  // Player chunk + radius: components outside the radius are frozen.
  void tick(ChunkMap& w, int pcx, int pcz, int radius, const DirtyFn& dirty);

  // ---- Timing Handshake surface (hs-design §3.2) ----
  // tick() is reassembled from exactly these pieces, in this order, so the
  // legacy and handshake paths share one implementation:
  //   hsBeginCycle; hsFlipAndTorches; hsComponents; hsMark;
  //   (fullPass ? hsPropagateFull : hsPrepIncremental + hsPropagateNet*)
  //   (+ hsApplyStrengthPending); hsLampsBatch(all); hsFinishCycle; hsCommit.
  // The radio/jammer flips live in hsCommit (cycle end) in BOTH modes (D12):
  // nothing outside tick() reads cur_ between ticks, so end-of-tick flip is
  // the same state the legacy start-of-tick flip produced for the next tick.
  void hsBeginCycle(ChunkMap& w, int pcx, int pcz, int radius);  // tick() bookkeeping (no flips)
  void hsFlipAndTorches(ChunkMap& w, const DirtyFn& dirty);      // collectMountPowered + tickTorches
  void hsComponents(ChunkMap& w, const DirtyFn& dirty);          // == tickComponents (timed)
  void hsMark(ChunkMap& w);                                      // wake sets + net marking + sorted work lists
  void hsPrepIncremental(ChunkMap& w);                           // reset marked nets + record per-net seeds/pulses
  void hsPropagateNet(ChunkMap& w, int32_t netId, const DirtyFn& dirty);  // one net's flood (pull-safe)
  void hsPropagateFull(ChunkMap& w, const DirtyFn& dirty);       // fullPass_ path (affine only)
  void hsLampsBatch(const std::vector<int64_t>& keys, ChunkMap& w, const DirtyFn& dirty);
  void hsFinishCycle(ChunkMap& w);                               // post-lamps tail (full-pass cleanup)
  void hsCommit();                                               // radio_/jammers_ flip
  bool hsCycleSkipped() const { return hsSkip_; }
  bool hsFullPass() const { return fullPass_; }
  const std::vector<int32_t>& hsMarkedNets() const { return hsMarkedNets_; }
  const std::vector<int64_t>& hsLampKeys() const { return hsLampKeys_; }
  uint64_t tickCount() const { return tickNo_; }
  /// Deferred lamp afterglow update produced by hsLampsBatch (the lampHold_
  /// map is sim-thread owned; pull batches return these, the sim thread
  /// merges them after the barrier — hs-design §3.3).
  struct LampHoldUpdate { int64_t key = 0; uint64_t end = 0; bool erase = false; };
  void hsApplyLampHolds(const std::vector<LampHoldUpdate>& updates);
  void hsApplyStrengthPending();  // replay per-net TraceStrengthBuffer writes in canonical order
  /// Thread-local hold updates of the calling thread's last hsLampsBatch:
  /// the caller drains this immediately after the call, on the same thread.
  static std::vector<LampHoldUpdate>& hsTlsLampHolds();
  size_t componentCount() const { return torches_.size() + wires_.size() + lamps_.size() + comps_.size(); }

  // Right-click on an interactive block: repeater delay cycle, switch toggle, button press,
  // keyboard capture toggle. Returns false if the block is not interactive.
  bool use(ChunkMap& w, int x, int y, int z, const DirtyFn& dirty, bool alt = false);
  /// Look-at inspector line: block #id, raw state fields, wire colour + network, component memory.
  std::string inspect(const ChunkMap& w, int x, int y, int z) const;
  /// Component sleep (default on): quiescent components skip ticks until a neighbour changes.
  void setComponentSleep(bool on) { compSleep_ = on; allAwake_ = true; }
  /// Per-class tick-rate override (JD 2026-10-14 rate scaling): when set
  /// for a block id it replaces the class's declared
  /// ComponentBehavior::tickRate() for every placed component of that
  /// class. This is the seam lithography (and tests) drive; shipped
  /// content sets NO overrides and every class declares the neutral
  /// rate, so the shipped behaviour is exactly the pre-scaling one.
  /// Setting a neutral or invalid rate clears the override.
  void setClassTickRate(BlockId block, litholattice::TickRate rate) {
    if (rate.valid() && !rate.neutral()) classRates_[block] = rate;
    else classRates_.erase(block);
  }
  /// Effective tick rate of one placed component: the override for its
  /// block id when present, else its class's declared tickRate().
  /// Validation (review A-H2): a class-declared rate is validated ONCE
  /// per BlockId (the catalog binds exactly one behaviour per BlockId
  /// for the sim's lifetime, so the validated result is cached). An
  /// invalid declaration (see TickRate::valid(): powers of two, multiply
  /// <= kMaxRateMultiply, divide <= kMaxRateDivide) is logged as an
  /// error on that first resolution and the NEUTRAL rate is substituted
  /// — mirroring the Timing Handshake authority's registration refusal
  /// (timing_handshake.cpp), which never holds an unschedulable
  /// component. Without this, multiply 0 would run zero sub-pulls and
  /// publish the default step (wiping cell state), an oversized multiply
  /// would hang the sim thread inside one firing, and a non-pow2 or
  /// oversized divide would silently starve the component.
  litholattice::TickRate classTickRate(BlockId block, const ComponentBehavior* behavior) const;
  size_t lastTickedComponents() const { return lastTicked_; }
  // Keyboard: queue a 1-tick pulse (front = SIXBIT code + 1, control = right-port code; 0 = none).
  void keyPulse(int x, int y, int z, int front, int control);
  /// Wire / lamp states were written from outside the sim (tests): re-run the flood next tick.
  void invalidateNetwork() { fullPass_ = true; }
  /// Lamp afterglow: a lamp that goes dark keeps showing its last colour for n ticks (0 = off).
  /// Lamps feed nothing back into the circuit, so this is purely visual; callers pick n from the
  /// TPS (about 1/30 s) so 1-tick pulses at 512+ TPS still reach a rendered frame.
  void setLampHoldTicks(int n) { lampHoldTicks_ = n < 0 ? 0 : n; }

  /// Save support: the per-component memory (outputs, edge detectors, delay lines, timers, queued
  /// key pulses) of every component, grouped by chunk column key (ChunkKey::make), in a versioned
  /// byte layout (kStateVersion). Everything else the sim needs is in the blocks and cell states,
  /// so scan() + importState() after a load restores the sim exactly.
  static constexpr uint16_t kStateVersion = 2;
  std::map<uint64_t, std::vector<uint8_t>> exportState() const;
  bool importState(const std::vector<uint8_t>& bytes);  ///< false on a malformed blob (nothing applied)

  /// Moves the sound events produced since the last call into `out` (appends).
  void takeSounds(std::vector<SoundEvent>& out) {
    out.insert(out.end(), sounds_.begin(), sounds_.end());
    sounds_.clear();
  }

  bool blockPowered(const ChunkMap& w, int x, int y, int z) const;   // fed by wire or strongly powered
  bool strongPowered(const ChunkMap& w, int x, int y, int z) const;
  void collectMountPowered(const ChunkMap& w);  // supports of attached switches / buttons that are on  // solid+opaque+powerable above a lit torch

 private:
  struct Comp {
    const ComponentBehavior* behavior;  // shared, owned by BehaviorCatalog
    ComponentMemory mem;
    bool rateHeld = false;  ///< divided-gated on an earlier tick: component sleep must not swallow its due tick (rate scaling)
    // Input fast path (filled by buildNets, valid while the networks are): per side 0 = reads
    // nothing, 1 = wire, 2 = torch, 3 = via (colour only), 4 = generic lookup.
    bool fast = false;
    uint8_t kind[4] = {}, ch[4] = {};
    CellState* nb[4] = {};
    CellState* top = nullptr;
    CellState* self = nullptr;
  };
  struct TickArea {  // components outside the player's ticking radius are frozen
    int pcx = 0, pcz = 0, radius = 0;
    bool contains(int x, int z) const { return std::abs((x >> 4) - pcx) <= radius && std::abs((z >> 4) - pcz) <= radius; }
  };
  void tickTorches(ChunkMap& w, const DirtyFn& dirty);     // 1) inverters, previous-tick input
  void tickComponents(ChunkMap& w, const DirtyFn& dirty);  // 2) behaviours, two-phase apply
  void propagateWires(ChunkMap& w, const DirtyFn& dirty);  // 3) decaying BFS + no-decay pulses
  void updateLamps(ChunkMap& w, const DirtyFn& dirty);     // 4) lamps / color lamps
  int inputFrom(const ChunkMap& w, int x, int y, int z, int dir) const;  // signal arriving from neighbour in dir
  int emitToward(const ChunkMap& w, int x, int y, int z, int towardDir) const;  // component at xyz -> dir
  std::unordered_set<int64_t> torches_, wires_, lamps_;
  std::unordered_map<int64_t, Comp> comps_;
  std::unordered_map<BlockId, litholattice::TickRate> classRates_;  ///< per-class tick-rate overrides (rate scaling); empty in shipped content
  mutable std::unordered_map<BlockId, litholattice::TickRate> declaredRates_;  ///< validated class-declared rates, cached per BlockId (A-H2); key-lookup only, never iterated
  RadioRegistry radio_;
  JammerRegistry jammers_;
  litholattice::TraceStrengthBuffer strengthBuf_;  ///< Step 1: GPU strength slots
  uint8_t dim_ = 1;  ///< this sim's dimension (for cross-dimensional radio)
  bool compSleep_ = false, allAwake_ = true;
 public:
  litholattice::TraceStrengthBuffer& strengthBuffer() { return strengthBuf_; }
  void setDimension(uint8_t d) { dim_ = d; }
  /// Phase timing (ms, summed since the last call): components, wakes + marking, wires, lamps.
  struct PhaseMs { double comps = 0, mark = 0, wires = 0, lamps = 0; };
  PhaseMs takePhaseMs() {
    PhaseMs p = phase_; phase_ = {};
    // Wires/lamps are timed inside the hs* methods (atomically: pull workers
    // run them) so the split is real in handshake mode too, not just legacy.
    p.wires = double(hsWiresNs_.exchange(0, std::memory_order_relaxed)) / 1e6;
    p.lamps = double(hsLampsNs_.exchange(0, std::memory_order_relaxed)) / 1e6;
    return p;
  }
 private:
  PhaseMs phase_;
  std::atomic<uint64_t> hsWiresNs_{0}, hsLampsNs_{0};  // hs* method self-timing (ns)
  struct SrcReach { std::vector<int32_t> nets; std::vector<int64_t> lamps; };
  std::unordered_map<int64_t, SrcReach> srcReach_;  // per changed source: nets / lamps within 2 (cleared with the nets)
  std::unordered_set<int64_t> awake_;               // components to tick next tick (sleep on)
  std::vector<std::vector<int64_t>> netComps_;      // per wire network: components reading it
  size_t lastTicked_ = 0;
  void wakeAround(int64_t k);
  std::array<std::vector<int64_t>, kMaxSignal + 1> buckets_;  // BFS work lists by strength
  std::vector<std::pair<int64_t, uint8_t>> torchNext_;
  std::unordered_set<int64_t> mountPowered_;                   // rebuilt before torches and after components        // torches ticked this tick (key, lit)
  TickArea area_;
  // Incremental re-flood. Wires and lamps are a pure function of torch / component outputs,
  // attached-switch power and the block layout. Wires are grouped into networks (connected runs,
  // same link rules as the flood). Each tick only the networks within 2 blocks of a source whose
  // output changed are reset and re-flooded, and only their lamps (plus lamps near the change and
  // expiring afterglows) are updated. Nothing changed: nothing runs. Layout edits, area moves and
  // outside writes force one full pass (and a network rebuild).
  bool fullPass_ = true, netsValid_ = false;
  std::unordered_map<int64_t, int32_t> wireNet_;          // wire -> network id
  std::vector<std::vector<int64_t>> nets_, netLamps_;     // per network: wires, adjacent lamps
  std::vector<uint32_t> netMarkedBits_;  ///< 1 bit per network (Step 2: bit-packed, was vector<uint8_t>)
  /// Bitset helpers (rule 2: 32 flags per uint32).
  void markNet(int32_t id) { netMarkedBits_[size_t(id) >> 5] |= 1u << (id & 31); }
  bool isNetMarked(int32_t id) const { return (netMarkedBits_[size_t(id) >> 5] >> (id & 31)) & 1u; }
  void clearNetMarks() { std::fill(netMarkedBits_.begin(), netMarkedBits_.end(), 0); }
  template <typename F> void forEachMarkedNet(F&& f) const {
    for (size_t i = 0; i < netMarkedBits_.size(); ++i) {
      uint32_t w = netMarkedBits_[i];
      while (w) {  // iterate set bits via count-trailing-zeros
        const unsigned b = std::countr_zero(w);
        f(int32_t(i * 32 + b));
        w &= w - 1;
      }
    }
  }
  std::unordered_set<int64_t> dirtyLamps_;
  std::vector<int64_t> changedSrc_, extraLamps_;
  void buildNets(ChunkMap& w);
  // Flat per-network arrays for the incremental pass (no hash lookups in the flood): cached state
  // pointers (valid until the layout changes, which rebuilds them), CSR links, scratch.
  struct NetArr {
    std::vector<int64_t> keys; std::vector<CellState*> st; std::vector<int32_t> adjStart, adj;
    std::vector<CellState> old; std::vector<uint32_t> stamp; std::vector<uint8_t> via;
    uint32_t pulseGen = 0;  // per-net pulse visited epoch (was the shared stampGen_ for this net's
                            // stamps; stamp arrays are per-net, so a per-net epoch is equivalent)
  };
  std::vector<NetArr> netArr_;
  // ---- handshake per-net work (hsPrepIncremental fills, hsPropagateNet consumes) ----
  bool hsSkip_ = false;                       // hsMark found nothing to do this cycle
  std::vector<int32_t> hsMarkedNets_;         // marked nets, ascending (canonical order)
  std::vector<int64_t> hsLampKeys_;           // this cycle's lamp keys, sorted ascending
  struct NetWork {
    std::vector<std::pair<int32_t, int>> seeds;    // (local idx, strength) in legacy seed order
    std::vector<std::pair<int32_t, int>> pulses;   // (local idx, strength) in legacy pulse order
    std::vector<std::pair<int64_t, uint8_t>> strength;  // pending TraceStrengthBuffer writes, cell order
  };
  std::vector<NetWork> hsNetWork_;  // by net id; only marked nets are filled
  std::unordered_map<int64_t, std::pair<int32_t, int32_t>> wireLoc_;  // in-area wire -> (net, index)
  std::array<std::vector<std::pair<int32_t, int32_t>>, kMaxSignal + 1> nbuckets_;
  uint32_t stampGen_ = 0;
  // Lamps fed only by wires: cached pointers to themselves and their wire neighbours.
  struct LampFast { CellState* self; std::vector<CellState*> wires; bool color; };
  std::unordered_map<int64_t, LampFast> lampFast_;
  size_t netColumns_ = 0;
  void wireLinks(const ChunkMap& w, int x, int y, int z, std::vector<int64_t>& out) const;  // no area test
  bool inScope(int64_t wireKey) const {
    if (fullPass_) return true;
    const auto it = wireNet_.find(wireKey);
    return it != wireNet_.end() && isNetMarked(it->second);
  }
  int lampHoldTicks_ = 0;
  std::unordered_map<int64_t, uint64_t> lampHold_;  // lamp -> tick its afterglow ends
  uint64_t tickNo_ = 0, nextHoldExpiry_ = UINT64_MAX;
  std::vector<SoundEvent> sounds_;
};

// Headless checks (--selftest) of every block's logic. Returns failures.
int runSignalSelfTest();

}  // namespace lithokraft
