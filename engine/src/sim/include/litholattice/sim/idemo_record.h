// Demo recording (IDemoRecord — JD's name: the interface that records,
// producing the demo record). Built on the control-serialization layer
// (control_stream.h, §9 of Control-Serialization-2026-10-13): a session is
// nothing but the stamped control stream that drove the world, framed for
// the file system. Replaying the record through submitStamped() into a
// fresh stream over the same initial world reproduces the session
// bit-identically, because the world is a pure function of (initial
// state, stamped stream).
//
// File format (v1, little-endian):
//   Preamble (8 bytes): 'LLDM', u16 demo format version (= 1), u16
//   reserved. Then zero or more segment frames, then — only on a clean
//   finish — one trailer.
//   Segment frame: u32 payloadLen, then payloadLen bytes holding ONE
//   complete LLCS v1 stream (writeControlStream) whose header carries the
//   session's seed / mode / tick rate / phases, declaredEvents = that
//   segment's event count, declaredTicks = 0. Segments are the recorder's
//   pump() drains: each pump appends exactly one frame, so a crash loses
//   at most the events not yet pumped.
//   Trailer (40 bytes): 'LLDT', u16 demo format version, u16 reserved,
//   u64 finalWorldHash, u64 totalEvents (all segments), u64 totalTicks,
//   u64 checksum = DeterministicScheduler::hashBytes over ALL preceding
//   file bytes (the same FNV-1a the LLCS format uses).
//
// World-permanence doctrine governs the parser: fail-closed on CORRUPTION
// (bad segment magic / version / checksum, header disagreement, trailer
// mismatch — distinct DemoError codes, earlier segments kept for
// forensics), but a torn TAIL is not corruption — a crash mid-session or
// mid-write yields truncated=true with every whole event recovered,
// including a record-by-record salvage of a final frame cut mid-write.
// A recorder is passive: it only ever calls setRecording()/takeRecord()
// on the stream; it never submits or extracts.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "litholattice/sim/control_stream.h"

namespace litholattice {

inline constexpr uint16_t kDemoFormatVersion = 1;
inline constexpr char kDemoMagic[4] = {'L', 'L', 'D', 'M'};
inline constexpr char kDemoTrailerMagic[4] = {'L', 'L', 'D', 'T'};

enum class DemoError : uint8_t {
  None = 0, Io, EmptyFile, BadMagic, UnsupportedVersion,
  SegmentBadMagic, SegmentUnsupportedVersion, SegmentChecksum,
  SegmentMalformed, HeaderMismatch,
  TrailerBadMagic, TrailerChecksum, TrailerCountMismatch,
  State  // API misuse: begin twice, finish before begin, submit before load
};
const char* demoErrorName(DemoError e);

struct DemoSessionInfo {
  uint64_t worldSeed = 0;
  uint8_t mode = 0;              // 0 legacy / 1 handshake (matches ControlStreamHeader::mode)
  uint16_t tickRate = 0;
  uint16_t phasesPerCycle = 0;
  uint32_t streamFormatVersion = kControlStreamFormatVersion;
  uint16_t demoFormatVersion = kDemoFormatVersion;
};
struct DemoTrailerInfo {
  bool present = false;
  uint64_t finalWorldHash = 0;
  uint64_t totalEvents = 0;
  uint64_t totalTicks = 0;
};
struct DemoParseResult {
  bool complete = false;    // trailer present, all checksums + counts valid
  bool truncated = false;   // a valid prefix was recovered (crash mid-session)
  DemoError error = DemoError::None;  // hard error (corruption class) when set
  std::string errorDetail;
  DemoSessionInfo session;  // from the first segment header
  bool sessionKnown = false;
  std::vector<ControlEvent> events;   // recovered events, application order
  DemoTrailerInfo trailer;
  size_t segmentsRecovered = 0;
};
class IDemoRecord {
 public:
  virtual ~IDemoRecord() = default;
  virtual const DemoSessionInfo& session() const = 0;
  virtual DemoError lastError() const = 0;
};
class DemoRecorder final : public IDemoRecord {
 public:
  // Attaches to the stream's recording tap (setRecording(true)) and starts
  // the file image with the demo preamble. False + State error if begun twice.
  bool begin(ControlStream& stream, const DemoSessionInfo& info);
  // Drains the tap (takeRecord) and appends ONE framed LLCS segment holding
  // the drained events; returns events appended (0 = nothing drained, no frame).
  size_t pump();
  // Final pump, then appends the trailer. False if not begun.
  bool finish(uint64_t finalWorldHash, uint64_t totalTicks);
  bool recording() const;
  uint64_t eventsRecorded() const;
  const std::vector<uint8_t>& bytes() const;
  bool saveToFile(const std::string& path) const;
  const DemoSessionInfo& session() const override;
  DemoError lastError() const override;
  // Detaches cleanly without a trailer (turns the tap off), leaving the
  // bytes written so far as a truncated demo image — the crash path.
  void abort();

 private:
  ControlStream* stream_ = nullptr;
  DemoSessionInfo session_;
  std::vector<uint8_t> bytes_;
  uint64_t eventsRecorded_ = 0;
  bool active_ = false;
  bool begun_ = false;
  mutable DemoError lastError_ = DemoError::None;
};
class DemoPlayer final : public IDemoRecord {
 public:
  bool load(const uint8_t* data, size_t n);   // parseDemo; true iff complete || truncated
  bool loadFile(const std::string& path);     // Io error on unreadable file
  const DemoParseResult& parsed() const;
  bool submitInto(ControlStream& stream) const;  // submitStamped(all parsed events); false if !load'ed ok
  const DemoSessionInfo& session() const override;
  DemoError lastError() const override;

 private:
  DemoParseResult parsed_;
  bool loaded_ = false;
  mutable DemoError lastError_ = DemoError::None;
};
DemoParseResult parseDemo(const uint8_t* data, size_t n);
struct DemoSummary {
  uint64_t totalEvents = 0;
  uint64_t byKind[6] = {};    // index (uint8_t)ControlKind (1..5 used)
  uint64_t bySource[5] = {};  // index (uint8_t)ControlSource (0..4)
  uint64_t firstTick = 0, lastTick = 0;
  bool hasEvents = false;
};
DemoSummary summarizeDemo(const DemoParseResult& p);
std::vector<ControlEvent> demoEventsInTickRange(const DemoParseResult& p, uint64_t lo, uint64_t hi);  // inclusive

}  // namespace litholattice
