// IDemoRecord proofs (the demo recorder / player built on the control
// serialization layer, 2026-10-13). Three layers:
//   runDemoSelfTest() — pure unit checks on a bare ControlStream: the
//                       record -> bytes -> parseDemo round trip, stream-
//                       level non-perturbation, the fail-closed /
//                       recovery matrix, and the analysis queries
//                       (summarizeDemo / demoEventsInTickRange).
//   runDemoTests()    — --demo-test: the SimThread proofs. A scripted
//                       session is recorded while the real SimThread
//                       (paused + stepped) drives it, proven not to
//                       perturb the world, then replayed bit-identically
//                       in legacy and handshake modes.
//   runDemoDump() / runDemoVerify() — the --demo-dump / --demo-verify
//                       analysis CLI (wired in main.cpp).
#include "demo_record_test.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#include "handshake_test.h"
#include "litholattice/sim/control_stream.h"
#include "litholattice/sim/det_sched.h"
#include "litholattice/sim/idemo_record.h"
#include "litholattice/sim/sim_thread.h"
#include "lithokraft/content/blocks.h"

namespace lithokraft {
namespace {

using namespace litholattice;

int gFails = 0;
void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++gFails;
}

// ------------------------------------------------------------ unit layer

bool sameEvent(const ControlEvent& a, const ControlEvent& b) {
  if (a.seq != b.seq || a.tick != b.tick || a.source != b.source || a.sourceId != b.sourceId || a.kind != b.kind)
    return false;
  switch (a.kind) {
    case ControlKind::Edit:
      return a.edit.kind == b.edit.kind && a.edit.facing == b.edit.facing && a.edit.block == b.edit.block &&
             a.edit.x == b.edit.x && a.edit.y == b.edit.y && a.edit.z == b.edit.z &&
             a.edit.command.code == b.edit.command.code && a.edit.command.x == b.edit.command.x &&
             a.edit.command.y == b.edit.command.y && a.edit.command.z == b.edit.command.z &&
             a.edit.command.a == b.edit.command.a && a.edit.command.b == b.edit.command.b;
    case ControlKind::Ray:
      return a.ray.ox == b.ray.ox && a.ray.oy == b.ray.oy && a.ray.oz == b.ray.oz && a.ray.dx == b.ray.dx &&
             a.ray.dy == b.ray.dy && a.ray.dz == b.ray.dz;
    case ControlKind::PlayerChunk:
      return a.chunk.cx == b.chunk.cx && a.chunk.cz == b.chunk.cz;
    case ControlKind::Locomotion:
      return a.loco.wishX == b.loco.wishX && a.loco.wishZ == b.loco.wishZ && a.loco.flags == b.loco.flags;
    case ControlKind::ControlFrame:
      return a.frame.moveForward == b.frame.moveForward && a.frame.moveStrafe == b.frame.moveStrafe &&
             a.frame.lookDx == b.frame.lookDx && a.frame.lookDy == b.frame.lookDy && a.frame.flags == b.frame.flags &&
             a.frame.hotbarSlot == b.frame.hotbarSlot;
  }
  return false;
}

bool allSame(const std::vector<ControlEvent>& a, const std::vector<ControlEvent>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (!sameEvent(a[i], b[i])) return false;
  return true;
}

// The scripted set, mirroring control_stream_test.cpp's sampleEvents():
// all FIVE kinds (Edit x3, Ray, PlayerChunk, Locomotion, ControlFrame),
// varied sources (Local / McpPilot / Script), ticks 100..125.
std::vector<ControlEvent> sampleEvents() {
  std::vector<ControlEvent> v;
  EditCmd cmd{EditCmd::Kind::Command};
  cmd.command = SimCommand{9, 1, 2, 3, -77, 0};
  ControlEvent e = ControlEvent::editEvent(cmd, ControlSource::Local);
  e.seq = 0; e.tick = 100;
  v.push_back(e);
  EditCmd use{EditCmd::Kind::Use};
  use.x = 24; use.y = 65; use.z = -30;
  ControlEvent u = ControlEvent::editEvent(use, ControlSource::McpPilot, 2);
  u.seq = 1; u.tick = 120;
  v.push_back(u);
  EditCmd place{EditCmd::Kind::Place};
  place.block = 4242; place.facing = 3;
  ControlEvent p = ControlEvent::editEvent(place, ControlSource::Script);
  p.seq = 2; p.tick = 121;
  v.push_back(p);
  ControlEvent r = ControlEvent::rayEvent(RayInput{8.5f, 70.25f, -3.5f, 0.0f, -1.0f, 0.5f}, ControlSource::Local);
  r.seq = 3; r.tick = 122;
  v.push_back(r);
  ControlEvent c = ControlEvent::chunkEvent(-7, 12, ControlSource::Local);
  c.seq = 4; c.tick = 123;
  v.push_back(c);
  ControlEvent l;
  l.kind = ControlKind::Locomotion; l.source = ControlSource::McpPilot; l.sourceId = 1;
  l.seq = 5; l.tick = 124; l.loco = LocomotionPayload{0.5f, -1.0f, 0x5};
  v.push_back(l);
  ControlEvent f;
  f.kind = ControlKind::ControlFrame; f.source = ControlSource::McpPilot; f.sourceId = 3;
  f.seq = 6; f.tick = 125; f.frame = ControlFramePayload{1.0f, -0.5f, 12.5f, -3.25f, 0x19, 4};
  v.push_back(f);
  return v;
}

DemoSessionInfo unitSession() {
  DemoSessionInfo s{};
  s.worldSeed = 0x1122334455667788ULL;
  s.mode = 1;
  s.tickRate = 512;
  s.phasesPerCycle = 4;
  s.streamFormatVersion = kControlStreamFormatVersion;
  s.demoFormatVersion = uint16_t(kDemoFormatVersion);
  return s;
}

constexpr uint64_t kUnitFinalHash = 0xD310D310D310D310ULL;  // fixed placeholder final hash
// Events with tick <= 122 — the content of the first pumped segment, and
// therefore the recoverable prefix when the final segment is torn.
constexpr size_t kFirstSegmentEvents = 4;

uint32_t readU32LE(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t readU64LE(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (8 * i);
  return v;
}
void writeU64LE(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = uint8_t((v >> (8 * i)) & 0xff);
}
// Recompute the trailer checksum: per the format, it is hashBytes over
// ALL file bytes preceding the trailer, stored in the trailer's last
// field (trailer offset +32 = file size - 8 for a 40-byte trailer).
void recomputeTrailerChecksum(std::vector<uint8_t>& bytes) {
  const uint64_t sum = DeterministicScheduler::hashBytes(bytes.data(), bytes.size() - 40);
  writeU64LE(bytes.data() + bytes.size() - 8, sum);
}

std::vector<uint8_t> testRoundTrip(const std::vector<ControlEvent>& script) {
  std::printf("== demo record: round trip through a bare ControlStream\n");
  ControlStream cs;
  DemoRecorder rec;
  rec.begin(cs, unitSession());
  check(rec.recording(), "demo: recorder begin turns the stream recording tap on");
  const DemoSessionInfo rs = rec.session();
  check(rs.worldSeed == 0x1122334455667788ULL && rs.mode == 1 && rs.tickRate == 512 && rs.phasesPerCycle == 4,
        "demo: recorder session echoes the session info (seed, mode, tick rate, phases)");
  cs.submitStamped(script);
  cs.prePhase(0);  // merges the pre-stamped script into the pending buffer
  std::vector<ControlEvent> out;
  const size_t first = cs.extractDue(122, out);
  check(first == kFirstSegmentEvents && out.size() == kFirstSegmentEvents,
        "demo: events extract at their stamp ticks (4 of 7 by tick 122)");
  rec.pump();  // segment 1: the first four events
  const size_t rest = cs.extractDue(125, out);
  check(rest == script.size() - kFirstSegmentEvents, "demo: the remaining events extract by tick 125");
  rec.pump();  // segment 2: the rest — at least two segments by construction
  check(rec.eventsRecorded() == script.size(), "demo: the recorder captured every applied event");
  check(rec.finish(kUnitFinalHash, 126), "demo: recorder finish writes the trailer");  // totalTicks = last tick + 1
  check(rec.lastError() == DemoError::None, "demo: recorder reports no error after a clean finish");
  const std::vector<uint8_t> bytes = rec.bytes();
  check(bytes.size() > 8 + 40 && std::memcmp(bytes.data(), "LLDM", 4) == 0,
        "demo: the record starts with the LLDM preamble and carries a trailer");

  const DemoParseResult p = parseDemo(bytes.data(), bytes.size());
  check(p.complete && !p.truncated && p.error == DemoError::None,
        "demo: parseDemo of the round trip is complete, not truncated, no error");
  check(p.sessionKnown && p.session.worldSeed == 0x1122334455667788ULL && p.session.mode == 1 &&
            p.session.tickRate == 512 && p.session.phasesPerCycle == 4 &&
            p.session.demoFormatVersion == kDemoFormatVersion &&
            p.session.streamFormatVersion == kControlStreamFormatVersion,
        "demo: parsed session round-trips (seed, mode, tick rate, phases, stream + demo versions)");
  check(p.trailer.present && p.trailer.finalWorldHash == kUnitFinalHash && p.trailer.totalEvents == script.size() &&
            p.trailer.totalTicks == 126,
        "demo: trailer fields exact (final hash, total events, total ticks)");
  check(allSame(p.events, script), "demo: every event round-trips field-for-field (all five kinds)");
  check(p.segmentsRecovered >= 2, "demo: at least two segments were framed and recovered");

  DemoPlayer player;
  check(player.load(bytes.data(), bytes.size()) && player.parsed().complete,
        "demo: DemoPlayer loads the round-trip bytes (complete)");
  ControlStream cs2;
  std::vector<ControlEvent> replayed;
  const bool submitted = player.submitInto(cs2);
  cs2.prePhase(0);
  cs2.extractDue(125, replayed);
  check(submitted && allSame(replayed, script),
        "demo: DemoPlayer submitInto reproduces the exact stamped stream on a fresh stream");
  return bytes;
}

void testNonPerturbation(const std::vector<ControlEvent>& script) {
  std::printf("== demo record: stream-level non-perturbation (attach / detach)\n");
  ControlStream plain;
  plain.submitStamped(script);
  plain.prePhase(0);
  std::vector<ControlEvent> outPlain;
  plain.extractDue(1000, outPlain);

  ControlStream tapped;
  DemoRecorder rec;
  rec.begin(tapped, unitSession());  // attach: tap on
  rec.abort();                       // detach: tap off, no trailer
  tapped.submitStamped(script);
  tapped.prePhase(0);
  std::vector<ControlEvent> outTapped;
  tapped.extractDue(1000, outTapped);
  check(allSame(outTapped, outPlain),
        "demo: attach/detach of a recorder leaves the extracted stream identical (recording off)");
  const ControlStreamStats st = tapped.stats();
  check(st.dropped == 0 && st.orderViolations == 0,
        "demo: recorder attach/detach leaves dropped == 0 and orderViolations == 0");
}

void testRecoveryMatrix(const std::vector<uint8_t>& bytes, const std::vector<ControlEvent>& script) {
  std::printf("== demo record: fail-closed / recovery matrix\n");
  {  // (i) file magic flipped
    std::vector<uint8_t> bad = bytes;
    bad[0] = 'X';
    const DemoParseResult p = parseDemo(bad.data(), bad.size());
    check(!p.complete && p.error == DemoError::BadMagic, "demo: flipped file magic -> BadMagic");
  }
  {  // (ii) demo version bumped (u16 version in the 8-byte preamble)
    std::vector<uint8_t> bad = bytes;
    bad[4] = 2;
    bad[5] = 0;
    const DemoParseResult p = parseDemo(bad.data(), bad.size());
    check(!p.complete && p.error == DemoError::UnsupportedVersion,
          "demo: bumped demo format version -> UnsupportedVersion");
  }
  {  // (iii) one payload byte flipped inside a segment: preamble (8) +
     // frame length (4) + LLCS header (48) + record header (20) + deep
     // in the first record's payload (a coordinate byte, so the record
     // structure stays valid and only the checksum can catch it).
    std::vector<uint8_t> bad = bytes;
    bad[12 + 48 + 30] ^= 0x40;
    const DemoParseResult p = parseDemo(bad.data(), bad.size());
    check(!p.complete && !p.truncated && p.error == DemoError::SegmentChecksum,
          "demo: flipped payload byte inside a segment -> SegmentChecksum (hard error, not truncated)");
  }
  {  // (iv) file cut mid-record in the final segment: keep the preamble,
     // the whole first frame, and the second frame's length + LLCS header
     // + part of its first record. Only segment 1's events can recover.
    const uint32_t frame1Len = readU32LE(bytes.data() + 8);
    const size_t cut = 8 + 4 + size_t(frame1Len) + 4 + 48 + 10;
    const DemoParseResult p = parseDemo(bytes.data(), std::min(cut, bytes.size()));
    check(p.truncated && !p.complete && p.events.size() == kFirstSegmentEvents,
          "demo: cut mid-record in the final segment -> truncated, prefix events recovered");
  }
  {  // (v) trailer removed entirely (last 40 bytes)
    const DemoParseResult p = parseDemo(bytes.data(), bytes.size() - 40);
    check(p.truncated && !p.complete && p.events.size() == script.size() && allSame(p.events, script),
          "demo: trailer removed -> truncated with ALL events recovered");
  }
  {  // (vi) trailer totalEvents tampered, trailer checksum recomputed:
     // the count no longer matches the recovered events.
    std::vector<uint8_t> bad = bytes;
    bool found = false;
    for (size_t off = bad.size() - 40; off + 8 <= bad.size() - 8 && !found; ++off) {
      if (readU64LE(bad.data() + off) == script.size()) {
        writeU64LE(bad.data() + off, script.size() + 1);
        found = true;
      }
    }
    if (found) recomputeTrailerChecksum(bad);
    const DemoParseResult p = parseDemo(bad.data(), bad.size());
    check(found && !p.complete && p.error == DemoError::TrailerCountMismatch,
          "demo: trailer totalEvents tampered (checksum recomputed) -> TrailerCountMismatch");
  }
  {  // (vii) trailer checksum broken (last byte of the stored checksum)
    std::vector<uint8_t> bad = bytes;
    bad[bad.size() - 1] ^= 0x40;
    const DemoParseResult p = parseDemo(bad.data(), bad.size());
    check(!p.complete && p.error == DemoError::TrailerChecksum,
          "demo: broken trailer checksum -> TrailerChecksum");
  }
  {  // (viii) segment LLCS magic smashed (first segment starts at
     // preamble (8) + frame length (4) = offset 12). Nothing is
     // recomputed, so the segment's own LLCS checksum fails too; the
     // contract maps a smashed segment magic to a HARD segment error —
     // SegmentBadMagic when the magic is checked first, SegmentChecksum
     // when the checksum is checked first. Either is the documented
     // mapping order; what matters is: hard error, not truncation.
    std::vector<uint8_t> bad = bytes;
    bad[12] = 'X';
    const DemoParseResult p = parseDemo(bad.data(), bad.size());
    std::printf("  (segment magic mapping produced: %s)\n", demoErrorName(p.error));
    check(!p.complete && (p.error == DemoError::SegmentBadMagic || p.error == DemoError::SegmentChecksum),
          "demo: smashed segment LLCS magic -> hard segment error (SegmentBadMagic or SegmentChecksum)");
  }
}

void testAnalysis(const std::vector<uint8_t>& bytes, const std::vector<ControlEvent>& script) {
  std::printf("== demo record: analysis queries on the known script\n");
  const DemoParseResult p = parseDemo(bytes.data(), bytes.size());
  const DemoSummary sum = summarizeDemo(p);
  check(sum.hasEvents && sum.totalEvents == script.size(), "demo: summarizeDemo total matches the script size");
  // byKind is indexed by kind value (Edit=1, Ray=2, PlayerChunk=3,
  // Locomotion=4, ControlFrame=5); the script holds 3 edits + 1 of each
  // other kind.
  check(sum.byKind[1] == 3 && sum.byKind[2] == 1 && sum.byKind[3] == 1 && sum.byKind[4] == 1 && sum.byKind[5] == 1,
        "demo: summarizeDemo byKind exact (3 edits, 1 ray, 1 chunk, 1 locomotion, 1 frame)");
  // bySource is indexed by source value (Local=0, McpPilot=1, Script=2,
  // Replay=3, Test=4); the script holds Local x3, McpPilot x3, Script x1.
  check(sum.bySource[0] == 3 && sum.bySource[1] == 3 && sum.bySource[2] == 1 && sum.bySource[3] == 0 &&
            sum.bySource[4] == 0,
        "demo: summarizeDemo bySource exact (Local 3, McpPilot 3, Script 1)");
  check(sum.firstTick == 100 && sum.lastTick == 125, "demo: summarizeDemo tick span exact (100..125)");
  const std::vector<ControlEvent> mid = demoEventsInTickRange(p, 120, 123);
  check(mid.size() == 4 && mid.front().seq == 1 && mid.back().seq == 4,
        "demo: demoEventsInTickRange interior range returns exactly its events (seq 1..4)");
  check(demoEventsInTickRange(p, 1000, 2000).empty(), "demo: demoEventsInTickRange outside all ticks returns empty");
}

// ------------------------------------------------------- scenario driver

// The §3 scripted stream (mirrors control_stream_test.cpp's
// scenarioScript): the fixture PlayerChunk every arm shares, then
//   tick 100: Ray (aim down at the first red trace) -> Break
//   tick 120: Use the first repeater (explicit cell)
//   tick 150: Ray again -> Place the trace back
std::vector<ControlEvent> scenarioScript(const ControlScenarioWorld& sc) {
  std::vector<ControlEvent> v;
  uint64_t seq = 0;
  ControlEvent fixture = ControlEvent::chunkEvent(4, 4, ControlSource::Test);
  fixture.seq = seq++;
  fixture.tick = 0;
  v.push_back(fixture);
  const float ox = float(sc.wireX) + 0.5f, oz = float(sc.wireZ) + 0.5f, oy = float(sc.y) + 3.0f;
  ControlEvent ray = ControlEvent::rayEvent(RayInput{ox, oy, oz, 0.f, -1.f, 0.f}, ControlSource::Test);
  ray.seq = seq++; ray.tick = 100;
  v.push_back(ray);
  ControlEvent brk = ControlEvent::editEvent(EditCmd{EditCmd::Kind::Break}, ControlSource::Test);
  brk.seq = seq++; brk.tick = 100;
  v.push_back(brk);
  EditCmd use{EditCmd::Kind::Use};
  use.x = sc.repX; use.y = sc.y; use.z = sc.repZ;
  ControlEvent u = ControlEvent::editEvent(use, ControlSource::Test);
  u.seq = seq++; u.tick = 120;
  v.push_back(u);
  ControlEvent ray2 = ray;
  ray2.seq = seq++; ray2.tick = 150;
  v.push_back(ray2);
  EditCmd place{EditCmd::Kind::Place};
  place.block = kRedTrace;
  place.facing = 0;
  ControlEvent pl = ControlEvent::editEvent(place, ControlSource::Test);
  pl.seq = seq++; pl.tick = 150;
  v.push_back(pl);
  return v;
}

struct DrivenResult {
  uint64_t hash = 0;
  ControlStreamStats stats{};
  bool stoppedCleanly = false;
  bool steppedAll = false;
  bool submitOk = true;
  bool finishOk = false;
  uint64_t eventsRecorded = 0;
};

// Drives the circuit scenario through the real SimThread (paused +
// stepped, 512-tick chunks). Exactly one input source is used per arm:
// `script` (submitStamped), `player` (DemoPlayer::submitInto), or
// neither. When `recorder` is given it is attached BEFORE start(),
// pumped after each 512-tick chunk, and finished (with the outcome
// hash) before the SimThread is destroyed — its stream reference must
// never outlive the thread's stream.
DrivenResult driveDemo(int ticks, bool handshake, int workers, const std::vector<ControlEvent>* script,
                       DemoPlayer* player, DemoRecorder* recorder, const DemoSessionInfo* recSession) {
  DrivenResult res;
  ControlScenarioWorld sc = buildControlScenarioWorld();
  ChunkMap& w = sc.wafer->chunks;

  SimConfig cfg;
  cfg.tickRate = 512;
  cfg.radiusChunks = 8;
  cfg.belowWorldFill = sc.wafer->belowWorldFill;
  cfg.timingHandshake = handshake;
  cfg.handshakePhasesPerCycle = 4;
  cfg.handshakeWorkerThreads = workers;
  SimThread st(w, blockRegistry(), std::move(sc.ws), std::move(cfg), [](std::shared_ptr<MeshInput>) {});
  SignalWorldSimulation& sig = static_cast<SignalWorldSimulation&>(st.simulation());

  if (recorder) recorder->begin(st.controlStream(), *recSession);  // attached BEFORE start
  if (script) st.controlStream().submitStamped(*script);
  if (player) res.submitOk = player->submitInto(st.controlStream());
  st.setPaused(true);  // step-only driving: the stream, not the wall clock, decides
  st.start();

  bool ok = true;
  uint64_t stepped = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(240);
  while (stepped < uint64_t(ticks) && ok) {
    const uint64_t chunk = std::min<uint64_t>(512, uint64_t(ticks) - stepped);
    st.stepTicks(chunk);
    const uint64_t target = stepped + chunk;
    while (st.ticksStepped() < target) {
      if (std::chrono::steady_clock::now() > deadline) { ok = false; break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stepped = target;
    if (recorder) recorder->pump();  // one framed segment per 512-tick chunk
  }
  res.steppedAll = ok && st.stats().worldTick == uint64_t(ticks);
  res.stoppedCleanly = st.stop();
  res.stats = st.controlStream().stats();
  res.hash = hashControlWorld(w, sig.signal(), uint64_t(ticks));
  if (recorder) {
    recorder->pump();
    res.finishOk = recorder->finish(res.hash, uint64_t(ticks));
    res.eventsRecorded = recorder->eventsRecorded();
  }
  return res;
}

void testSimThreadProofs() {
  std::printf("== demo proofs: recorded SimThread session, non-perturbation, replay (2048 ticks)\n");
  ControlScenarioWorld probe = buildControlScenarioWorld();
  const std::vector<ControlEvent> script = scenarioScript(probe);
  probe.ws.reset();  // the probe world is done; arms rebuild their own

  DemoSessionInfo session{};
  // buildControlScenarioWorld() exposes no seed constant; the scenario
  // world is the Wafer, whose worldgen is pinned to seed 0 — so 0 is
  // the value the builder effectively uses.
  session.worldSeed = 0;
  session.mode = 0;  // legacy (the recorded arm); replay is cross-checked in handshake mode
  session.tickRate = 512;
  session.phasesPerCycle = 4;
  session.streamFormatVersion = kControlStreamFormatVersion;
  session.demoFormatVersion = uint16_t(kDemoFormatVersion);

  // RECORDED SESSION (legacy), recorder attached before start.
  DemoRecorder rec;
  const DrivenResult recorded = driveDemo(2048, false, 0, &script, nullptr, &rec, &session);
  std::printf("  recorded (legacy): hash=%016llx applied=%llu eventsRecorded=%llu\n",
              (unsigned long long)recorded.hash, (unsigned long long)recorded.stats.applied,
              (unsigned long long)recorded.eventsRecorded);
  check(recorded.steppedAll && recorded.stoppedCleanly, "demo-proof: recorded session stepped all 2048 ticks and stopped cleanly");
  check(recorded.finishOk && recorded.eventsRecorded == script.size(),
        "demo-proof: recorder finished and captured every scripted event");
  check(recorded.hash == 0x5041cf1795c33a8cULL,
        "demo-proof: recorded hash matches the control crew's scripted hash 5041cf1795c33a8c");
  check(recorded.stats.applied == script.size() && recorded.stats.dropped == 0 && recorded.stats.orderViolations == 0,
        "demo-proof: recorded session applied every event exactly once, none dropped/misordered");
  const std::vector<uint8_t> recBytes = rec.bytes();
  const DemoParseResult recParsed = parseDemo(recBytes.data(), recBytes.size());
  check(recParsed.complete && recParsed.trailer.present && recParsed.trailer.finalWorldHash == recorded.hash &&
            recParsed.events.size() == script.size(),
        "demo-proof: the recorded bytes parse complete with the session hash in the trailer");
  check(rec.saveToFile("./demo_record_proof.lldemo"), "demo-proof: recorder saved ./demo_record_proof.lldemo");

  // NON-PERTURBATION: identical drive, NO recorder attached.
  const DrivenResult bare = driveDemo(2048, false, 0, &script, nullptr, nullptr, nullptr);
  std::printf("  no recorder (legacy): hash=%016llx applied=%llu\n", (unsigned long long)bare.hash,
              (unsigned long long)bare.stats.applied);
  check(bare.hash == recorded.hash, "demo-proof: non-perturbation — no-recorder hash is identical to the recorded hash");

  // REPLAY (legacy): a fresh SimThread fed only from the saved record.
  DemoPlayer playerLegacy;
  const bool loadedLegacy = playerLegacy.loadFile("./demo_record_proof.lldemo");
  check(loadedLegacy && playerLegacy.parsed().complete && playerLegacy.parsed().trailer.present &&
            playerLegacy.parsed().trailer.finalWorldHash == recorded.hash,
        "demo-proof: DemoPlayer loads the saved record (complete, trailer hash matches)");
  const uint64_t trailerHash = playerLegacy.parsed().trailer.finalWorldHash;
  const DrivenResult replayLegacy = driveDemo(2048, false, 0, nullptr, &playerLegacy, nullptr, nullptr);
  std::printf("  replay (legacy): hash=%016llx applied=%llu dropped=%llu violations=%llu\n",
              (unsigned long long)replayLegacy.hash, (unsigned long long)replayLegacy.stats.applied,
              (unsigned long long)replayLegacy.stats.dropped, (unsigned long long)replayLegacy.stats.orderViolations);
  check(replayLegacy.submitOk && replayLegacy.hash == trailerHash,
        "demo-proof: legacy replay reproduces the recorded world bit-identically");
  check(replayLegacy.stats.applied == script.size() && replayLegacy.stats.dropped == 0 &&
            replayLegacy.stats.orderViolations == 0,
        "demo-proof: replay applied every event exactly once (dropped == 0, orderViolations == 0)");

  // REPLAY (handshake, 2 workers): cross-mode bit-identity.
  DemoPlayer playerHs;
  const bool loadedHs = playerHs.loadFile("./demo_record_proof.lldemo");
  const DrivenResult replayHs = driveDemo(2048, true, 2, nullptr, &playerHs, nullptr, nullptr);
  std::printf("  replay (handshake, 2 workers): hash=%016llx applied=%llu\n", (unsigned long long)replayHs.hash,
              (unsigned long long)replayHs.stats.applied);
  check(loadedHs && replayHs.submitOk && replayHs.hash == trailerHash,
        "demo-proof: handshake replay (2 workers) is bit-identical to the recorded legacy session");

  // SANITY: the recorded session actually did something.
  check(recorded.hash != 0x2a2d19a416a1826eULL,
        "demo-proof: recorded hash differs from the no-input baseline 2a2d19a416a1826e");
}

// ------------------------------------------------------------ CLI helpers

bool readFileBytes(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

}  // namespace

int runDemoSelfTest() {
  const int before = gFails;
  const std::vector<ControlEvent> script = sampleEvents();
  const std::vector<uint8_t> bytes = testRoundTrip(script);
  testNonPerturbation(script);
  testRecoveryMatrix(bytes, script);
  testAnalysis(bytes, script);
  return gFails - before;
}

int runDemoTests() {
  gFails = 0;
  runDemoSelfTest();
  testSimThreadProofs();
  std::printf("%s\n", gFails ? "DEMO TESTS: FAILED" : "DEMO TESTS: OK");
  return gFails ? 1 : 0;
}

int runDemoDump(const std::string& file, bool hasRange, uint64_t lo, uint64_t hi) {
  std::vector<uint8_t> bytes;
  if (!readFileBytes(file, bytes)) {
    std::printf("demo-dump: cannot read '%s'\n", file.c_str());
    return 1;
  }
  const DemoParseResult p = parseDemo(bytes.data(), bytes.size());
  if (p.sessionKnown) {
    const DemoSessionInfo& s = p.session;
    std::printf("session: seed=%016llx mode=%u tickRate=%u phases=%u streamVersion=%u demoVersion=%u\n",
                (unsigned long long)s.worldSeed, unsigned(s.mode), unsigned(s.tickRate), unsigned(s.phasesPerCycle),
                unsigned(s.streamFormatVersion), unsigned(s.demoFormatVersion));
  } else {
    std::printf("session: unknown\n");
  }
  if (p.complete) {
    std::printf("status: COMPLETE (%zu events recovered, %zu segments)\n", p.events.size(), p.segmentsRecovered);
  } else if (p.truncated) {
    std::printf("status: PARTIAL (truncated, %zu events recovered)\n", p.events.size());
  } else {
    std::printf("status: INVALID: %s %s\n", demoErrorName(p.error), p.errorDetail.c_str());
  }
  const DemoSummary sum = summarizeDemo(p);
  std::printf("counts: total=%llu\n", (unsigned long long)sum.totalEvents);
  for (int k = 1; k <= 5; ++k)
    std::printf("  kind %-12s %llu\n", controlKindName(ControlKind(k)), (unsigned long long)sum.byKind[k]);
  for (int s = 0; s <= 4; ++s)
    std::printf("  source %-10s %llu\n", controlSourceName(ControlSource(s)), (unsigned long long)sum.bySource[s]);
  if (sum.hasEvents)
    std::printf("ticks: first=%llu last=%llu\n", (unsigned long long)sum.firstTick, (unsigned long long)sum.lastTick);
  else
    std::printf("ticks: none\n");
  if (p.trailer.present)
    std::printf("trailer: hash=%016llx events=%llu ticks=%llu\n", (unsigned long long)p.trailer.finalWorldHash,
                (unsigned long long)p.trailer.totalEvents, (unsigned long long)p.trailer.totalTicks);
  else
    std::printf("trailer: ABSENT\n");
  if (hasRange) {
    const std::vector<ControlEvent> inRange = demoEventsInTickRange(p, lo, hi);
    std::printf("events in ticks [%llu, %llu]: %zu\n", (unsigned long long)lo, (unsigned long long)hi, inRange.size());
    for (const ControlEvent& e : inRange) std::printf("  %s\n", controlEventToText(e).c_str());
  }
  return (p.complete || p.truncated) ? 0 : 1;
}

int runDemoVerify(const std::string& file) {
  std::vector<uint8_t> bytes;
  if (!readFileBytes(file, bytes)) {
    std::printf("demo-verify: INVALID: cannot read '%s'\n", file.c_str());
    return 1;
  }
  const DemoParseResult p = parseDemo(bytes.data(), bytes.size());
  if (p.complete) {
    std::printf("demo-verify: COMPLETE (%zu events, %zu segments)\n", p.events.size(), p.segmentsRecovered);
    return 0;
  }
  if (p.truncated) {
    std::printf("demo-verify: PARTIAL (truncated, %zu events recovered)\n", p.events.size());
    return 2;
  }
  std::printf("demo-verify: INVALID: %s %s\n", demoErrorName(p.error), p.errorDetail.c_str());
  return 1;
}

}  // namespace lithokraft
