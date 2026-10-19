// Demo recording: the IDemoRecord recorder/player and the LLDM file
// format (see idemo_record.h for the design contract). The segment
// payloads are complete LLCS v1 streams — writeControlStream /
// parseControlStream are the ground truth for their byte layout, and the
// salvage decoder below mirrors control_stream.cpp's record layout
// field-for-field so a frame cut mid-write still yields its whole records.
#include "litholattice/sim/idemo_record.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include "litholattice/sim/det_sched.h"

namespace litholattice {

const char* demoErrorName(DemoError e) {
  switch (e) {
    case DemoError::None: return "None";
    case DemoError::Io: return "Io";
    case DemoError::EmptyFile: return "EmptyFile";
    case DemoError::BadMagic: return "BadMagic";
    case DemoError::UnsupportedVersion: return "UnsupportedVersion";
    case DemoError::SegmentBadMagic: return "SegmentBadMagic";
    case DemoError::SegmentUnsupportedVersion: return "SegmentUnsupportedVersion";
    case DemoError::SegmentChecksum: return "SegmentChecksum";
    case DemoError::SegmentMalformed: return "SegmentMalformed";
    case DemoError::HeaderMismatch: return "HeaderMismatch";
    case DemoError::TrailerBadMagic: return "TrailerBadMagic";
    case DemoError::TrailerChecksum: return "TrailerChecksum";
    case DemoError::TrailerCountMismatch: return "TrailerCountMismatch";
    case DemoError::State: return "State";
  }
  return "unknown";
}

namespace {

void putU16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(uint8_t(v & 0xff));
  b.push_back(uint8_t((v >> 8) & 0xff));
}
void putU32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(uint8_t((v >> (8 * i)) & 0xff));
}
void putU64(std::vector<uint8_t>& b, uint64_t v) {
  for (int i = 0; i < 8; ++i) b.push_back(uint8_t((v >> (8 * i)) & 0xff));
}

uint16_t getU16(const uint8_t* p) { return uint16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8)); }
uint32_t getU32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= uint32_t(p[i]) << (8 * i);
  return v;
}
uint64_t getU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (8 * i);
  return v;
}
int32_t getI32(const uint8_t* p) { return int32_t(getU32(p)); }
float getF32(const uint8_t* p) {
  const uint32_t bits = getU32(p);
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}

constexpr size_t kPreambleBytes = 8;
constexpr size_t kTrailerBytes = 40;
constexpr size_t kLlcHeaderBytes = 48;  // mirrors control_stream.cpp's kHeaderBytes
constexpr uint32_t kMaxPlausiblePayload = 0x80000000u;  // 2^31 sanity bound

DemoSessionInfo sessionFromHeader(const ControlStreamHeader& h) {
  DemoSessionInfo s;
  s.worldSeed = h.worldSeed;
  s.mode = h.mode;
  s.tickRate = h.tickRate;
  s.phasesPerCycle = h.phasesPerCycle;
  s.streamFormatVersion = kControlStreamFormatVersion;
  s.demoFormatVersion = kDemoFormatVersion;
  return s;
}

bool sessionDisagrees(const DemoSessionInfo& a, const DemoSessionInfo& b) {
  return a.worldSeed != b.worldSeed || a.mode != b.mode || a.tickRate != b.tickRate ||
         a.phasesPerCycle != b.phasesPerCycle;
}

// Salvage decoder for a frame cut mid-write: `p`/`avail` are the payload
// bytes actually on disk (avail < the frame's declared payloadLen). The
// payload has no valid checksum (it was cut before / inside it), so trust
// extends only to the 48-byte header's magic/version and to records that
// are byte-whole and field-valid; the walk stops at the first incomplete
// or invalid record. recovered events are appended to `r`. A header that
// fails validation, or that disagrees with the session already known,
// recovers nothing — a torn tail is not corruption evidence.
void salvageSegment(const uint8_t* p, size_t avail, DemoParseResult& r) {
  if (avail < kLlcHeaderBytes) return;
  if (std::memcmp(p, kControlStreamMagic, 4) != 0) return;
  if (getU16(p + 4) != kControlStreamFormatVersion) return;
  if (getU16(p + 6) != kLlcHeaderBytes) return;
  ControlStreamHeader h;
  h.worldSeed = getU64(p + 8);
  h.mode = p[16];
  h.tickRate = getU16(p + 18);
  h.phasesPerCycle = getU16(p + 20);
  h.declaredTicks = getU64(p + 24);
  h.declaredEvents = getU64(p + 32);
  const DemoSessionInfo segSession = sessionFromHeader(h);
  if (r.sessionKnown && sessionDisagrees(r.session, segSession)) return;
  if (!r.sessionKnown) {
    r.session = segSession;
    r.sessionKnown = true;
  }
  size_t at = kLlcHeaderBytes;
  uint64_t recovered = 0;
  while (at < avail) {
    if (h.declaredEvents > 0 && recovered >= h.declaredEvents) break;
    if (avail - at < 20) break;  // seq(8) tick(8) kind(1) source(1) sourceId(2)
    const uint8_t kind = p[at + 16];
    const uint8_t source = p[at + 17];
    if (kind < uint8_t(ControlKind::Edit) || kind > uint8_t(ControlKind::ControlFrame)) break;
    if (source > uint8_t(ControlSource::Test)) break;
    size_t payloadBytes = 0;
    switch (ControlKind(kind)) {
      case ControlKind::Edit: payloadBytes = 38; break;
      case ControlKind::Ray: payloadBytes = 24; break;
      case ControlKind::PlayerChunk: payloadBytes = 8; break;
      case ControlKind::Locomotion: payloadBytes = 12; break;
      case ControlKind::ControlFrame: payloadBytes = 20; break;
    }
    if (avail - at < 20 + payloadBytes) break;
    const uint8_t* rec = p + at;
    const uint8_t* pay = rec + 20;
    ControlEvent e;
    e.seq = getU64(rec);
    e.tick = getU64(rec + 8);
    e.kind = ControlKind(kind);
    e.source = ControlSource(source);
    e.sourceId = getU16(rec + 18);
    bool valid = true;
    switch (e.kind) {
      case ControlKind::Edit: {
        const uint8_t ek = pay[0];
        if (ek > uint8_t(EditCmd::Kind::Command)) { valid = false; break; }
        e.edit.kind = EditCmd::Kind(ek);
        e.edit.facing = pay[1];
        e.edit.block = getU16(pay + 2);
        e.edit.x = getI32(pay + 4);
        e.edit.y = getI32(pay + 8);
        e.edit.z = getI32(pay + 12);
        e.edit.command.code = getU16(pay + 16);
        e.edit.command.x = getI32(pay + 18);
        e.edit.command.y = getI32(pay + 22);
        e.edit.command.z = getI32(pay + 26);
        e.edit.command.a = getI32(pay + 30);
        e.edit.command.b = getI32(pay + 34);
        e.edit.tick = e.tick;  // the record's stamp is the edit's tick
        break;
      }
      case ControlKind::Ray:
        e.ray.ox = getF32(pay);
        e.ray.oy = getF32(pay + 4);
        e.ray.oz = getF32(pay + 8);
        e.ray.dx = getF32(pay + 12);
        e.ray.dy = getF32(pay + 16);
        e.ray.dz = getF32(pay + 20);
        break;
      case ControlKind::PlayerChunk:
        e.chunk.cx = getI32(pay);
        e.chunk.cz = getI32(pay + 4);
        break;
      case ControlKind::Locomotion:
        e.loco.wishX = getF32(pay);
        e.loco.wishZ = getF32(pay + 4);
        e.loco.flags = pay[8];
        break;
      case ControlKind::ControlFrame:
        e.frame.moveForward = getF32(pay);
        e.frame.moveStrafe = getF32(pay + 4);
        e.frame.lookDx = getF32(pay + 8);
        e.frame.lookDy = getF32(pay + 12);
        e.frame.flags = pay[16];
        e.frame.hotbarSlot = int8_t(pay[17]);
        break;
    }
    if (!valid) break;
    r.events.push_back(e);
    ++recovered;
    at += 20 + payloadBytes;
  }
}

}  // namespace

// ------------------------------------------------------------------ parser

DemoParseResult parseDemo(const uint8_t* data, size_t n) {
  DemoParseResult r;
  auto hardError = [&](DemoError e, const std::string& detail) {
    r.error = e;
    r.errorDetail = detail;
    r.complete = false;
    r.truncated = false;
    return r;
  };
  if (n == 0) return hardError(DemoError::EmptyFile, "demo: empty file");
  if (data == nullptr || n < 4 || std::memcmp(data, kDemoMagic, 4) != 0)
    return hardError(DemoError::BadMagic, "demo: bad magic");
  if (n < kPreambleBytes) {  // magic intact, preamble itself torn by a crash
    r.truncated = true;
    return r;
  }
  if (getU16(data + 4) != kDemoFormatVersion)
    return hardError(DemoError::UnsupportedVersion, "demo: unsupported format version");

  uint64_t declaredSum = 0;
  size_t offset = kPreambleBytes;
  while (offset < n) {
    const size_t remaining = n - offset;
    const uint8_t* at = data + offset;

    // A trailer is exactly 40 bytes at a frame boundary, at end of file.
    if (remaining == kTrailerBytes) {
      if (std::memcmp(at, kDemoTrailerMagic, 4) != 0) {
        // A torn segment frame can also leave exactly 40 bytes behind;
        // its payload would begin with the LLCS header magic.
        if (remaining >= 8 && std::memcmp(at + 4, kControlStreamMagic, 4) == 0) {
          salvageSegment(at + 4, remaining - 4, r);
          r.truncated = true;
          return r;
        }
        return hardError(DemoError::TrailerBadMagic, "demo: bad trailer magic");
      }
      if (getU16(at + 4) != kDemoFormatVersion)
        return hardError(DemoError::UnsupportedVersion, "demo: unsupported trailer format version");
      const uint64_t storedSum = getU64(at + 32);
      if (DeterministicScheduler::hashBytes(data, offset) != storedSum)
        return hardError(DemoError::TrailerChecksum, "demo: trailer checksum mismatch");
      r.trailer.finalWorldHash = getU64(at + 8);
      r.trailer.totalEvents = getU64(at + 16);
      r.trailer.totalTicks = getU64(at + 24);
      r.trailer.present = true;
      if (r.trailer.totalEvents != declaredSum || r.trailer.totalEvents != r.events.size())
        return hardError(DemoError::TrailerCountMismatch,
                         "demo: trailer event count does not match the segments");
      r.complete = true;
      r.truncated = false;
      r.error = DemoError::None;
      return r;
    }
    if (remaining > kTrailerBytes && std::memcmp(at, kDemoTrailerMagic, 4) == 0)
      return hardError(DemoError::TrailerBadMagic, "demo: trailer is not the final 40 bytes");

    if (remaining < 4) {  // torn frame-length field: a short non-trailer tail
      r.truncated = true;
      return r;
    }
    const uint32_t payloadLen = getU32(at);
    if (payloadLen > kMaxPlausiblePayload || uint64_t(payloadLen) > uint64_t(remaining - 4)) {
      // The final frame was cut mid-write (or its length is implausible):
      // salvage the payload bytes that did land, then stop — torn tail.
      salvageSegment(at + 4, remaining - 4, r);
      r.truncated = true;
      return r;
    }

    // Byte-complete frame: the LLCS parser is fail-closed, so its verdict
    // is mapped to the distinct segment error by inspection.
    const uint8_t* payload = at + 4;
    ControlStreamParse seg = parseControlStream(payload, payloadLen);
    if (!seg.ok) {
      DemoError mapped = DemoError::SegmentMalformed;
      if (payloadLen < 4 || std::memcmp(payload, kControlStreamMagic, 4) != 0)
        mapped = DemoError::SegmentBadMagic;
      else if (payloadLen < 6 || getU16(payload + 4) != kControlStreamFormatVersion)
        mapped = DemoError::SegmentUnsupportedVersion;
      else if (seg.error.find("checksum") != std::string::npos)
        mapped = DemoError::SegmentChecksum;
      return hardError(mapped, seg.error);
    }
    const DemoSessionInfo segSession = sessionFromHeader(seg.header);
    if (r.sessionKnown && sessionDisagrees(r.session, segSession))
      return hardError(DemoError::HeaderMismatch, "demo: segment header disagrees with the first segment");
    if (!r.sessionKnown) {
      r.session = segSession;
      r.sessionKnown = true;
    }
    declaredSum += seg.header.declaredEvents;
    r.events.insert(r.events.end(), seg.events.begin(), seg.events.end());
    ++r.segmentsRecovered;
    offset += 4 + size_t(payloadLen);
  }
  // Bytes exhausted with no trailer and no hard error: crash before finish.
  r.truncated = true;
  return r;
}

// ---------------------------------------------------------------- recorder

bool DemoRecorder::begin(ControlStream& stream, const DemoSessionInfo& info) {
  if (begun_) {
    lastError_ = DemoError::State;
    return false;
  }
  begun_ = true;
  active_ = true;
  stream_ = &stream;
  session_ = info;
  session_.streamFormatVersion = kControlStreamFormatVersion;
  session_.demoFormatVersion = kDemoFormatVersion;
  bytes_.clear();
  bytes_.insert(bytes_.end(), kDemoMagic, kDemoMagic + 4);
  putU16(bytes_, kDemoFormatVersion);
  putU16(bytes_, 0);
  eventsRecorded_ = 0;
  stream_->setRecording(true);
  lastError_ = DemoError::None;
  return true;
}

size_t DemoRecorder::pump() {
  if (!active_ || stream_ == nullptr) {
    lastError_ = DemoError::State;
    return 0;
  }
  std::vector<ControlEvent> drained;
  stream_->takeRecord(drained);
  if (drained.empty()) {
    lastError_ = DemoError::None;
    return 0;
  }
  ControlStreamHeader header;
  header.worldSeed = session_.worldSeed;
  header.mode = session_.mode;
  header.tickRate = session_.tickRate;
  header.phasesPerCycle = session_.phasesPerCycle;
  header.declaredTicks = 0;
  header.declaredEvents = drained.size();
  std::vector<uint8_t> payload;
  if (!writeControlStream(header, drained, payload)) {
    lastError_ = DemoError::State;
    return 0;
  }
  putU32(bytes_, uint32_t(payload.size()));
  bytes_.insert(bytes_.end(), payload.begin(), payload.end());
  eventsRecorded_ += drained.size();
  lastError_ = DemoError::None;
  return drained.size();
}

bool DemoRecorder::finish(uint64_t finalWorldHash, uint64_t totalTicks) {
  if (!active_) {
    lastError_ = DemoError::State;
    return false;
  }
  pump();  // final pump: everything applied up to here lands in a segment
  const uint64_t sum = DeterministicScheduler::hashBytes(bytes_.data(), bytes_.size());
  bytes_.insert(bytes_.end(), kDemoTrailerMagic, kDemoTrailerMagic + 4);
  putU16(bytes_, kDemoFormatVersion);
  putU16(bytes_, 0);
  putU64(bytes_, finalWorldHash);
  putU64(bytes_, eventsRecorded_);
  putU64(bytes_, totalTicks);
  putU64(bytes_, sum);
  if (stream_ != nullptr) stream_->setRecording(false);
  stream_ = nullptr;
  active_ = false;
  lastError_ = DemoError::None;
  return true;
}

void DemoRecorder::abort() {
  if (stream_ != nullptr) stream_->setRecording(false);
  stream_ = nullptr;
  active_ = false;
}

bool DemoRecorder::recording() const { return active_; }
uint64_t DemoRecorder::eventsRecorded() const { return eventsRecorded_; }
const std::vector<uint8_t>& DemoRecorder::bytes() const { return bytes_; }

bool DemoRecorder::saveToFile(const std::string& path) const {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    lastError_ = DemoError::Io;
    return false;
  }
  if (!bytes_.empty())
    out.write(reinterpret_cast<const char*>(bytes_.data()), std::streamsize(bytes_.size()));
  out.flush();
  if (!out.good()) {
    lastError_ = DemoError::Io;
    return false;
  }
  return true;
}

const DemoSessionInfo& DemoRecorder::session() const { return session_; }
DemoError DemoRecorder::lastError() const { return lastError_; }

// ------------------------------------------------------------------ player

bool DemoPlayer::load(const uint8_t* data, size_t n) {
  parsed_ = parseDemo(data, n);
  loaded_ = parsed_.complete || parsed_.truncated;
  lastError_ = parsed_.error;
  return loaded_;
}

bool DemoPlayer::loadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    parsed_ = DemoParseResult{};
    loaded_ = false;
    lastError_ = DemoError::Io;
    return false;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) {
    parsed_ = DemoParseResult{};
    loaded_ = false;
    lastError_ = DemoError::Io;
    return false;
  }
  return load(data.empty() ? nullptr : data.data(), data.size());
}

const DemoParseResult& DemoPlayer::parsed() const { return parsed_; }

bool DemoPlayer::submitInto(ControlStream& stream) const {
  if (!loaded_) {
    lastError_ = DemoError::State;
    return false;
  }
  stream.submitStamped(parsed_.events);
  lastError_ = DemoError::None;
  return true;
}

const DemoSessionInfo& DemoPlayer::session() const { return parsed_.session; }
DemoError DemoPlayer::lastError() const { return lastError_; }

// ----------------------------------------------------------------- summary

DemoSummary summarizeDemo(const DemoParseResult& p) {
  DemoSummary s;
  s.totalEvents = p.events.size();
  s.hasEvents = !p.events.empty();
  if (s.hasEvents) {
    s.firstTick = p.events.front().tick;
    s.lastTick = p.events.back().tick;
  }
  for (const ControlEvent& e : p.events) {
    const uint8_t k = uint8_t(e.kind);
    if (k < 6) ++s.byKind[k];
    const uint8_t src = uint8_t(e.source);
    if (src < 5) ++s.bySource[src];
  }
  return s;
}

std::vector<ControlEvent> demoEventsInTickRange(const DemoParseResult& p, uint64_t lo, uint64_t hi) {
  std::vector<ControlEvent> out;
  for (const ControlEvent& e : p.events)
    if (e.tick >= lo && e.tick <= hi) out.push_back(e);
  return out;
}

}  // namespace litholattice
