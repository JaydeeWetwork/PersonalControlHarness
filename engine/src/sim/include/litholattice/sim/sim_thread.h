// Tick scheduler / simulation thread: owns the active dimension's ChunkMap while running.
// Fixed-timestep world tick (32/64/128/256 TPS, runtime-selectable), clock domains the game
// declares (each at its own power-of-two rate), player edits, raycast for the block highlight,
// the game's IWorldSimulation, and remesh snapshots. No GL, no render types: remeshing is
// requested through the SubmitMesh callback with an immutable MeshInput.
#pragma once
#include <chrono>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>
#include "litholattice/core/fail_fast.h"
#include "litholattice/core/threading.h"
#include "litholattice/core/time.h"
#include "litholattice/sim/clock_domain.h"
#include "litholattice/sim/random_ticks.h"
#include "litholattice/sim/timing_handshake.h"
#include "litholattice/sim/world_simulation.h"
#include "litholattice/world/chunk_map.h"
#include "litholattice/world/mesh_input.h"

namespace litholattice {

class ControlStream;   // control_stream.h — the serialized control-event stream
struct ControlEvent;   // control_stream.h — one stamped event (the POST scratch holds these)

constexpr int kTickModes[] = {32, 64, 128, 256};  // world TPS modes (powers of two)
constexpr int kDefaultTps = 32;

struct RayInput { float ox = 0, oy = 0, oz = 0, dx = 0, dy = 0, dz = 1; };  // render -> sim

// Player actions (render -> sim). Break/Place act on the current ray hit (reach kReach);
// Use carries an explicit cell (right-click on an interactive block); Command is game-defined
// and goes to IWorldSimulation::command.
constexpr float kReach = 8.f, kLookRange = 64.f;
struct EditCmd {
  enum class Kind : uint8_t { Break, Place, Use, Command } kind = Kind::Break;
  BlockId block = 0; uint8_t facing = 0;   // Place
  int x = 0, y = 0, z = 0;                 // Use (and copied into Command)
  SimCommand command{};                    // Command
  /// Sim-stamped target world tick (set on drain). Edit application is a pure
  /// function of tick index: the edit lands on this tick, whenever it runs.
  uint64_t tick = 0;
};

/// Historical sentinel for the paused path's apply-everything call. Control
/// serialization made it unnecessary (paused application is stamp-driven:
/// events stamped at drain carry the current tick and are due immediately);
/// retained because the value is part of this header's vocabulary.
inline constexpr uint64_t kApplyAllEdits = UINT64_MAX;

constexpr int kMaxClockDomains = 4;

/// Everything SimThread needs besides the world and the game's simulation.
struct SimConfig {
  int tickRate = kDefaultTps;      ///< world TPS (one of kTickModes)
  int radiusChunks = 8;            ///< tick radius around the player
  uint32_t epoch = 0;              ///< dimension load generation (stamped on mesh snapshots)
  BlockId belowWorldFill = kAir;   ///< what the mesher sees below y = 0 (solid hides bottom faces)
  int randomTicksPerSection = 8;   ///< per 16^3 section per world tick (per dimension; 0 = off)
  std::vector<ClockDomain> domains;  ///< up to kMaxClockDomains, power-of-two rates
  // Timing Handshake (hs-design §5): DEFAULT ON since 2026-10-14 (JD's
  // promotion directive — the handshake is the standard timing path). An
  // unconfigured SimThread constructs the authority; legacy mode is the
  // explicit opt-out (timingHandshake = false), which constructs no
  // authority and runs the legacy path byte-for-byte.
  bool timingHandshake = true;
  int handshakePhasesPerCycle = 4;   ///< phases per cycle (Signal needs >= 4)
  int handshakeWorkerThreads = 0;    ///< 0 = authority auto
  int handshakeWatchdogMs = 0;       ///< 0 = authority default (250 ms)
  bool handshakePureSpin = false;    ///< D9 A/B flag: true = pull workers pure-spin instead of the hybrid poll (review A-L9)
};

struct SimStats {              // sim -> render, every wake-up (lock-free triple buffer)
  uint64_t worldTick = 0;
  int targetTps = 0, masterHz = 0;
  float tps = 0.f, tickMs = 0.f, tickMaxMs = 0.f, budgetPct = 0.f;  // tick ms: avg/max over last 1 s
  uint64_t droppedTicks = 0;
  uint32_t active = 0;                     // IWorldSimulation::activeCount()
  uint64_t randomTicks = 0;                // opted-in cells random-ticked so far
  struct Domain { int hz = 0; float achieved = 0.f; bool enabled = false; };
  std::array<Domain, kMaxClockDomains> domains{};  // in SimConfig::domains order
  int domainCount = 0;
  uint64_t hsOverrunPhases = 0;   // Timing Handshake stats (0 in legacy mode)
  uint32_t hsOffenders = 0;       // components flagged repeat overrun offenders
  bool hsFrozen = false;          // watchdog freeze latch
  bool hit = false, inReach = false; float hitDist = 0;  // looked-at block (up to kLookRange)
  int hx = 0, hy = 0, hz = 0; BlockId hitBlock = 0; CellState hitState = 0;
  char hitInfo[256] = {};  ///< look-at inspector line from IWorldSimulation::inspect
};

class SimThread {
 public:
  using SubmitMesh = std::function<void(std::shared_ptr<MeshInput>)>;
  /// `simulation` is used on the sim thread only; `blocks` must outlive the thread.
  SimThread(ChunkMap& chunks, const BlockRegistry& blocks, std::unique_ptr<IWorldSimulation> simulation,
            SimConfig config, SubmitMesh submit);
  ~SimThread();
  void start();
  // Stops the sim thread, then the handshake authority. Returns false in
  // handshake mode when the authority's no-detach stop() failed: a pull
  // worker is still outstanding (authority poisoned), the sim must NOT be
  // treated as safely stopped for save-completion or dimension switch,
  // and start() will refuse until a retried stop() succeeds. Callers that
  // ignore the result keep the old behaviour only in legacy mode, where
  // stop() always returns true. (P0a / review C2 — see timing_handshake.h.)
  bool stop();
  /// True while a handshake pull worker is outstanding after a failed
  /// stop() (poisoned; see stop()).
  bool handshakeWorkersOutstanding() const { return handshake_ && handshake_->workersOutstanding(); }
  // Render thread -> sim. Control inputs cross the serialized control
  // stream (control_stream.h): the sim thread stamps them at the PRE phase
  // and applies them at the POST phase, in (tick, seq) order.
  /// Sets the player chunk (the tick area). A stream event now, not an
  /// atomic: the area in effect for tick T is the last PlayerChunk event
  /// stamped <= T, so the tick area is a function of the stream.
  void setPlayerChunk(int cx, int cz);
  void setTickRate(int tps) { validateTickRate(tps); requestedTps_.store(tps); notifyWake(); }
  /// Paused: no ticks run (edits / raycast still apply); unpausing does not catch up.
  void setPaused(bool p) { paused_.store(p); notifyWake(); }
  bool paused() const { return paused_.load(); }
  /// Step N WORLD ticks while paused (creative-mode tick debugger).
  /// If not paused, pauses first. Each counted step is one full world tick
  /// run through the SAME step body as the live path (clock domains, tick-
  /// quantized edits, sim tick, randomTick AND runRandomTicks, window/stat
  /// accounting): with faster clock domains configured the stepper advances
  /// as many master steps as it takes until N world ticks have run, so
  /// stepTicks(n) advances worldTick by exactly n (review CH-H5).
  void stepTicks(uint64_t n) { stepRequested_.fetch_add(n); if (!paused()) setPaused(true); notifyWake(); }
  /// Run `fn` between ticks: pauses the sim, waits for any in-flight tick to
  /// finish (the block mutex is held across the sim's whole mutation window),
  /// runs fn with the mutex held, then restores the pause state.
  /// Hot-swap refresh uses this: pause one tick, swap block definitions,
  /// resume — no torn reads mid-tick.
  void runBetweenTicks(std::function<void()> fn);
  /// Ticks stepped so far (for test verification).
  uint64_t ticksStepped() const { return ticksStepped_.load(std::memory_order_acquire); }
  /// Flips clock domain `index` on / off (SimConfig::domains order).
  void toggleDomain(int index) { if (index >= 0 && index < kMaxClockDomains) { domainToggles_.fetch_xor(1u << index); notifyWake(); } }
  /// Aim ray. Two consumers, two paths (the alpha-presentation doctrine):
  /// the HUD look-at highlight reads the raw triple-buffer slot
  /// (presentation-only, latest wins), while edit resolution reads the
  /// STAMPED ray from the control stream — so a Break/Place resolves
  /// against the aim in effect on its own tick, and a replayed stream
  /// resolves identically.
  void setRay(const RayInput& r);
  /// Sim loop iterations started so far (each drains edits, then ticks). Waiting for two more than
  /// a reading guarantees one whole iteration ran after it: tests wait on progress, not wall time.
  uint64_t loops() const { return loops_.load(std::memory_order_acquire); }
  /// Single-block read for threads other than the sim thread (game thread entity
  /// collision, MCP world_get_block). Race-free: takes blockMutex_, which the sim
  /// thread holds across its whole mutation window (edit drain + every tick of the
  /// iteration), so no ChunkMap write can interleave with this read. May block for
  /// up to one catch-up burst; fine for a handful of queries per frame, not for
  /// bulk scans.
  BlockId getBlock(int x, int y, int z) {
    std::lock_guard l(blockMutex_);
    return chunks_.get(x, y, z);
  }
  /// Blocks until `n` more iterations have started, or `timeout` passes (false then).
  bool waitLoops(uint64_t n, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) const;
  /// A player edit (Local source). Crosses the control stream: stamped at
  /// the next PRE phase with the tick it lands on, applied in (tick, seq)
  /// order at that tick's POST phase. Producers that know a different
  /// source (MCP pilots, scripts) submit via controlStream() directly.
  void edit(EditCmd c);
  /// The serialized control stream (producers submit; IDemoRecord taps
  /// the record; tests inject stamped streams). Owned by this thread.
  ControlStream& controlStream() { return *control_; }
  const ControlStream& controlStream() const { return *control_; }
  /// Re-snapshots every loaded sub-chunk for meshing at the next tick (textures were reloaded).
  void requestRemeshAll() { remeshAll_.store(true); notifyWake(); }
  /// Columns whose cells changed (player edits, simulation ticks, random ticks, commands) since
  /// the last call: what a partial save must write. Clears the set. Call only while stopped.
  std::unordered_set<uint64_t> takeChangedColumns() { std::unordered_set<uint64_t> out; out.swap(changedCols_); return out; }
  const SimStats& stats() { return stats_.read(); }  // render thread only
  /// Game thread: simulation events (sounds, ...) since the last call (appended to `out`).
  void drainEvents(std::vector<SimEvent>& out) { events_.drainInto(out); }
  /// The game's simulation. Touch it only while the thread is stopped (before start() or after
  /// stop()), e.g. to save or restore its state; start() may be called again afterwards.
  IWorldSimulation& simulation() { return *sim_; }
  /// Timing Handshake authority (null in legacy mode). Registration is
  /// pre-start only; test harnesses may add components before start().
  TimingHandshakeAuthority* handshakeAuthority() { return handshake_.get(); }
  bool handshakeFrozen() const { return handshake_ && handshake_->frozen(); }
  /// Recovery from a watchdog freeze (the tick_unfreeze path in handshake
  /// mode): clears the authority latch; the interrupted cycle is abandoned
  /// and the simulation has already forced a full pass for the next tick.
  void resetHandshakeAfterFreeze() {
    if (handshake_) handshake_->resetAfterFreeze();
    freezeReported_.store(false);
    stepRefusalLogged_.store(false);
  }

 private:
  void run();
  /// POST phase of the control coupling: extract every stream event
  /// stamped for `tick` or earlier and apply them in (tick, seq) order —
  /// Ray events move the stamped aim, PlayerChunk events move the tick
  /// area, Edit events run the edit application below against the stamped
  /// ray. Events not yet due stay pending. Returns true if any EDIT was
  /// due (the remesh-at-once signal, as the old applyEdits returned).
  bool applyControlEvents(uint64_t tick);
  /// One edit against the current stamped-ray cast (the pre-serialization
  /// applyEdits loop body, one event at a time so stream order — including
  /// Ray events between edits — is honored exactly). Returns true when the
  /// edit changed the world (the caller re-casts before the next edit).
  bool applyOneEdit(EditCmd& e, const std::function<void(int, int, int)>& dirty);
  /// Look-at raycast for the HUD highlight only (no edit application).
  void castRay(const RayInput& ray);
  /// simFailFast unless tps is a supported tick mode (kTickModes ∪ {512},
  /// review A-L1). Domain rates are likewise fail-fasted in the ctor when
  /// non-pow2 or above kMaxDomainHz.
  static void validateTickRate(int tps);
  void handleHandshakeFreeze();  // watchdog: pause + report (never simFailFast)
  // Paused-loop parking (review A-M7): bumps the wake epoch under wakeM_
  // and notifies wakeCv_. Every external change a parked paused loop must
  // react to (pause toggle, step request, edit, ray, rate/domain change,
  // remesh request, stop) goes through here. The epoch bump happens under
  // wakeM_ so a parked loop cannot miss it between predicate check and
  // wait: the bumper cannot acquire wakeM_ until the waiter has released
  // it by actually waiting.
  void notifyWake() {
    { std::lock_guard l(wakeM_); wakeEpoch_.fetch_add(1, std::memory_order_acq_rel); }
    wakeCv_.notify_all();
  }
  void markDirty(int x, int y, int z);
  void popAttached(int x, int y, int z);
  void simChanged(int x, int y, int z);
  void flushDirty();

  // ---- Timing Handshake (null in legacy mode) ----
  std::unique_ptr<TimingHandshakeAuthority> handshake_;
  bool handshakeMode_ = false;
  std::atomic<bool> freezeReported_{false};  // the current freeze was reported
  std::atomic<bool> stepRefusalLogged_{false};  // frozen step refusal logged once
  uint64_t hsMasterStep_ = 0;     // sim thread: current master step (domain cadence)
  int hsMasterHz_ = 1;            // sim thread: current master rate (domain cadence)
  bool hsCountWindow_ = false;    // sim thread: count domain windowTicks this step
  std::vector<std::string> hsDomainNames_;  // owned names for domain components

  ChunkMap& chunks_;
  // getBlock() <-> sim-thread mutation-window protocol: the sim thread holds this
  // across the edit drain + every tick of each loop iteration; getBlock() takes it
  // for the read. Uncontended on the sim thread; getBlock() may wait one burst.
  // Recursive: runBetweenTicks holds it across fn(), and fn() may legitimately
  // call getBlock() (which takes the same mutex). A plain mutex deadlocks there.
  std::recursive_mutex blockMutex_;
  const BlockRegistry& blocks_;
  std::unique_ptr<IWorldSimulation> sim_;
  const int radius_;
  int randomPerSection_ = 8;
  std::vector<uint8_t> randomTickable_;
  uint64_t randomTicks_ = 0;
  std::atomic<uint64_t> loops_{0};
  const uint32_t epoch_;
  const BlockId belowWorld_;
  SubmitMesh submit_;
  std::atomic<bool> running_{false};
  std::atomic<int> requestedTps_;
  std::atomic<uint32_t> domainToggles_{0};
  std::atomic<bool> remeshAll_{false};
  std::thread thread_;
  TripleBuffer<SimStats> stats_;
  TripleBuffer<RayInput> ray_;  // presentation only (HUD look-at); edit resolution uses stampedRay_
  // The serialized control stream: every sim-affecting control input
  // crosses here (edits, the stamped ray, the player chunk). Constructed
  // with the SimThread; producers submit from any thread; the sim thread
  // alone stamps (PRE) and extracts (POST).
  std::unique_ptr<ControlStream> control_;
  std::unordered_set<uint64_t> changedCols_;  // sim thread (or caller while stopped)
  HandoffQueue<SimEvent> events_;       // sim -> game
  std::vector<SimEvent> eventBuf_;
  void publishEvents();

  // ---- sim-thread-owned state ----
  std::vector<ClockDomain> domains_;
  std::vector<bool> domainInitial_;
  std::unordered_set<int64_t> dirtySubs_;
  std::vector<ControlEvent> controlDue_;  // POST-phase extraction scratch (stream order)
  // Stamped control state (sim thread): the aim and tick area in effect
  // for the tick being run — pure functions of the applied stream prefix.
  RayInput stampedRay_{};
  uint64_t areaChunkKey_ = 0;  // ChunkKey::make(0, 0), as the old atomic default
  // Pooled MeshInput buffers: the pool is shared with the submit() deleter so it
  // outlives any in-flight snapshot (a SimThread may die while mesh workers still
  // hold snapshots). Cap bounds the retained memory (~64 x 24 KB).
  struct MeshInputPool {
    std::mutex m;
    std::vector<std::unique_ptr<MeshInput>> free;
  };
  std::shared_ptr<MeshInputPool> meshPool_ = std::make_shared<MeshInputPool>();
  RandomTickCache randomTickCache_;  // ticking column pointers (opt: skip hash lookups)
  uint64_t meshSeq_ = 1ull << 32;  // above the initial-load sequence numbers

  // ---- persistent tick state (review CH-H8) — sim thread only ----
  // Initialised ONCE at construction and deliberately NOT reset by
  // stop()/start(): masterStep_/worldTickN_/dropped_ and the accumulator /
  // window state survive the routine stop/start cycles of every save and
  // stream pump, so keyed random streams (seeded by world tick) never
  // replay, edit stamps never regress, and SimStats.worldTick never goes
  // backwards across a restart. A freshly constructed SimThread still
  // starts at 0 — only restarts changed. (Wall-clock pacing — the `last`
  // timestamp in run() — stays a run() local, re-based on each start.)
  uint64_t masterStep_ = 0, worldTickN_ = 0, dropped_ = 0;
  std::chrono::nanoseconds acc_{0};
  Clock::time_point windowStart_{};  // zero = unset; run() bases it on entry
  uint64_t windowWorldTicks_ = 0;
  double windowWorldNs_ = 0, windowAllNs_ = 0, windowWorldMaxNs_ = 0;
  float statTps_ = 0, statTickMs_ = 0, statTickMaxMs_ = 0, statBudget_ = 0;
  int cfgWorldTps_ = 0, cfgMasterHz_ = 0, cfgWorldDiv_ = 1;
  std::chrono::nanoseconds stepDur_{0};
  uint32_t togglesSeen_ = 0;
  bool configured_ = false;
  // Shared step-body scratch (CH-H5): set per loop iteration before the
  // step body runs; the body reports edit/raycast work back through these.
  bool stepEditsApplied_ = false, stepRaycastDone_ = false;

  // ---- paused-loop parking (review A-M7) ----
  std::mutex wakeM_;
  std::condition_variable wakeCv_;
  std::atomic<uint64_t> wakeEpoch_{0};  // bumped (under wakeM_) by notifyWake()
  bool hit_ = false; float hitDist_ = 0; int hit_xyz_[3]{}, prev_xyz_[3]{};
  // Look-at inspector cache: sim_->inspect is re-run only when the looked-at cell
  // changes (formatting it every loop iteration was pure churn).
  std::string hitInfoCache_;
  int hitInfoXyz_[3]{};
  bool hitInfoValid_ = false;
 std::atomic<bool> paused_{false};
  std::atomic<uint64_t> stepRequested_{0};  ///< pending single-step ticks (tick debugger)
  std::atomic<uint64_t> ticksStepped_{0};   ///< ticks completed via stepTicks
};


}  // namespace litholattice
