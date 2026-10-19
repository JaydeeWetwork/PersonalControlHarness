// Control-event serialization (JD's design, 2026-10-13: the Timing
// Handshake x control-system coupling). Every sim-affecting control input
// — player edits, the aim ray those edits resolve against, the player
// chunk that defines the tick area, locomotion samples, MCP pilot control
// frames — crosses ONE stream. The sim thread is the single sequencing
// point: at the PRE phase of each loop iteration (before the handshake
// cycle / the tick) it drains every producer's submissions and stamps
// each event with (seq, tick) — seq a global order, tick the world tick
// the event lands on. At the POST phase (inside the world-tick step,
// after the handshake's domain phase, before IWorldSimulation::tick) the
// events whose stamp has matured are applied in (tick, seq) order:
// exhaustive, exactly-once, never silently dropped — a late event is
// applied and ACCOUNTED, in the handshake's discipline language.
//
// One gear only: the stamp/application quantum is the world tick itself
// (world TPS is already a power of two, 32..512). The control system does
// not need the handshake's orders of magnitude of rates.
//
// Determinism contract: the world is a pure function of (initial state,
// stamped stream). Live stamping is arrival-dependent BY NATURE (the
// outside world is not deterministic); the stamped stream is the record,
// and replaying a record through submitStamped() reproduces the world
// bit-identically in both modes. This stream is the layer the future
// IDemoRecord (the demo record / demo recorder) records and replays.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "litholattice/sim/sim_thread.h"  // EditCmd, RayInput (the Edit/Ray payloads)

namespace litholattice {

/// Who produced a control event. Recorded verbatim in the stream so a
/// demo record shows every source that drove the world.
enum class ControlSource : uint8_t {
  Local = 0,     ///< the local player's devices, via the game's action handling
  McpPilot = 1,  ///< an MCP pilot (IPlayerController), via the same action handling
  Script = 2,    ///< a script / console command acting on the world
  Replay = 3,    ///< a recorded stream being replayed (IDemoRecord playback)
  Test = 4,      ///< test harnesses
};

enum class ControlKind : uint8_t {
  Edit = 1,         ///< a player edit (EditCmd payload) — sim-affecting
  Ray = 2,          ///< the aim ray edits resolve against (RayInput payload)
  PlayerChunk = 3,  ///< the player chunk: defines the sim's TickArea
  Locomotion = 4,   ///< one tick of locomotion input (game-side consumer today;
                    /// in the format now so demo records carry it)
  ControlFrame = 5, ///< one IPlayerController::ControlFrame (MCP pilot input,
                    /// mirrored field-for-field; the adapter is game-side)
};

const char* controlSourceName(ControlSource s);
const char* controlKindName(ControlKind k);

/// Locomotion payload: mirrors entity LocomotionInput (wish direction +
/// pose buttons), float-quantized at the adapter.
struct LocomotionPayload {
  float wishX = 0.f, wishZ = 0.f;
  uint8_t flags = 0;  // bit0 jump, bit1 sneak, bit2 sprint, bit3 crawl
};

/// ControlFrame payload: mirrors mcp::IPlayerController::ControlFrame
/// field-for-field (the sim library does not depend on the mcp library;
/// the game-side adapter translates).
struct ControlFramePayload {
  float moveForward = 0.f, moveStrafe = 0.f;  // -1..1, sticky until changed
  float lookDx = 0.f, lookDy = 0.f;           // one-shot look deltas (pixels)
  uint8_t flags = 0;  // bit0 jump, bit1 sneak, bit2 sprint, bit3 usePressed,
                      // bit4 attackPressed, bit5 placePressed, bit6 dropPressed
  int8_t hotbarSlot = -1;  // 0..7 select; -1 = no change
};

struct PlayerChunkPayload {
  int cx = 0, cz = 0;
};

/// One stamped control event. `seq` and `tick` are assigned by the stream
/// at the PRE phase for live submissions (submit() ignores whatever the
/// producer put there); submitStamped() events keep theirs — they ARE a
/// record being replayed.
struct ControlEvent {
  uint64_t seq = 0;
  uint64_t tick = 0;  ///< world tick the event lands on (the stamp)
  ControlSource source = ControlSource::Local;
  uint16_t sourceId = 0;  ///< pilot index / device id, source-defined
  ControlKind kind = ControlKind::Edit;
  // Payloads (exactly one is meaningful, per kind):
  EditCmd edit{};                 // Edit
  RayInput ray{};                 // Ray
  PlayerChunkPayload chunk{};     // PlayerChunk
  LocomotionPayload loco{};       // Locomotion
  ControlFramePayload frame{};    // ControlFrame

  static ControlEvent editEvent(const EditCmd& e, ControlSource s = ControlSource::Local, uint16_t id = 0) {
    ControlEvent ev; ev.kind = ControlKind::Edit; ev.source = s; ev.sourceId = id; ev.edit = e; return ev;
  }
  static ControlEvent rayEvent(const RayInput& r, ControlSource s = ControlSource::Local, uint16_t id = 0) {
    ControlEvent ev; ev.kind = ControlKind::Ray; ev.source = s; ev.sourceId = id; ev.ray = r; return ev;
  }
  static ControlEvent chunkEvent(int cx, int cz, ControlSource s = ControlSource::Local, uint16_t id = 0) {
    ControlEvent ev; ev.kind = ControlKind::PlayerChunk; ev.source = s; ev.sourceId = id; ev.chunk = {cx, cz}; return ev;
  }
};

struct ControlStreamStats {
  uint64_t submitted = 0;  ///< live submissions accepted (producer side)
  uint64_t injected = 0;   ///< pre-stamped events accepted (replay/test side)
  uint64_t stamped = 0;    ///< live events stamped at PRE phases
  uint64_t applied = 0;    ///< events extracted for application (POST)
  uint64_t lateApplied = 0;  ///< applied with stamp < the extraction tick (accounted, never dropped)
  uint64_t dropped = 0;    ///< by construction always 0 — the field exists so
                           /// tests and IDemoRecord can ASSERT it stays 0
  uint64_t orderViolations = 0;  ///< applications out of (tick, seq) order; 0 by construction
  uint64_t pendingNow = 0;       ///< stamped, not yet applied
  uint64_t maxPending = 0;
};

/// The serialized control stream. Producers (any thread) submit raw
/// events; the owning sim thread runs the PRE/POST phases. All sim-thread
/// methods are sim-thread-only; stats() is any-thread.
class ControlStream {
 public:
  // ---- producer side (any thread) ----
  /// Queue a raw (unstamped) event. Its seq/tick fields are ignored: the
  /// stamp is assigned at the next PRE phase, in drain order.
  void submit(ControlEvent e);
  /// Queue pre-stamped events (a record being replayed, a test script).
  /// They merge at the next PRE phase keeping their (seq, tick); the live
  /// sequence counter advances past their seqs so a later live stamp can
  /// never collide.
  void submitStamped(std::vector<ControlEvent> evts);

  // ---- sim thread side ----
  /// PRE phase: drain both queues; stamp live events with (nextSeq++,
  /// `stampTick`); merge everything into the pending buffer, which stays
  /// sorted by (tick, seq).
  void prePhase(uint64_t stampTick);
  /// POST phase: move every pending event with tick <= `tick` into `out`,
  /// in (tick, seq) order. Returns the count. Application order is checked
  /// against the last applied event: a regression is counted in
  /// orderViolations (and still applied — accounted, never dropped).
  size_t extractDue(uint64_t tick, std::vector<ControlEvent>& out);
  /// True when any pending event matures at or before `tick`.
  bool hasDue(uint64_t tick) const { return !pending_.empty() && pending_.front().tick <= tick; }
  size_t pendingCount() const { return pending_.size(); }

  // ---- recording tap (IDemoRecord consumes exactly this) ----
  /// When recording is on, extractDue() also appends every applied event
  /// (with its stamp) to the record buffer. Default off (zero retention).
  void setRecording(bool on) { recording_.store(on, std::memory_order_release); }
  bool recording() const { return recording_.load(std::memory_order_acquire); }
  /// Drain the record buffer (the events applied since the last drain, in
  /// application order — a verbatim demo record segment).
  void takeRecord(std::vector<ControlEvent>& out);

  ControlStreamStats stats() const;

 private:
  void insertSorted(ControlEvent e);
  std::mutex inboundM_;
  std::vector<ControlEvent> inbound_;
  std::mutex injectM_;
  std::vector<ControlEvent> inject_;
  // Sim-thread only:
  std::vector<ControlEvent> pending_;  // sorted by (tick, seq)
  uint64_t nextSeq_ = 0;
  uint64_t lastAppliedSeq_ = 0, lastAppliedTick_ = 0;
  bool haveApplied_ = false;
  std::atomic<uint64_t> submitted_{0}, injected_{0};
  std::atomic<uint64_t> stamped_{0}, applied_{0}, lateApplied_{0}, orderViolations_{0};
  std::atomic<uint64_t> pendingNow_{0}, maxPending_{0};
  std::atomic<bool> recording_{false};
  std::mutex recordM_;
  std::vector<ControlEvent> record_;
};

// ---- stream format (the demo-record file format, v1) ----
// Binary, little-endian, fail-closed on parse: bad magic, an unsupported
// version, a truncated or trailing record, an unknown kind, or a checksum
// mismatch all fail the WHOLE parse (no partial event list).
inline constexpr uint32_t kControlStreamFormatVersion = 1;
inline constexpr char kControlStreamMagic[4] = {'L', 'L', 'C', 'S'};

struct ControlStreamHeader {
  uint64_t worldSeed = 0;
  uint8_t mode = 0;              ///< 0 = legacy, 1 = timing handshake
  uint16_t tickRate = 0;         ///< world TPS — the single gear
  uint16_t phasesPerCycle = 0;   ///< handshake phases (0 in legacy)
  uint64_t declaredTicks = 0;    ///< advisory: total ticks recorded (0 = unknown / live)
  uint64_t declaredEvents = 0;   ///< advisory: total events recorded (0 = unknown / live)
};

bool writeControlStream(const ControlStreamHeader& header, const std::vector<ControlEvent>& events,
                        std::vector<uint8_t>& out);

struct ControlStreamParse {
  bool ok = false;
  std::string error;  // when !ok
  ControlStreamHeader header;
  std::vector<ControlEvent> events;
};
ControlStreamParse parseControlStream(const uint8_t* data, size_t n);

/// One-line human-readable form (the "line format"): for demo inspection,
/// logs, and test diffs. Lossless for stamps/sources/kinds; payload floats
/// print at inspection precision (the binary form is the exact record).
std::string controlEventToText(const ControlEvent& e);

}  // namespace litholattice
