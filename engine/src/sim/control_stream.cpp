// Control-event serialization: the stream, its accounting, and the v1
// binary record format (see control_stream.h for the design contract).
#include "litholattice/sim/control_stream.h"

#include <algorithm>
#include <cstring>
#include "litholattice/sim/det_sched.h"

namespace litholattice {

const char* controlSourceName(ControlSource s) {
  switch (s) {
    case ControlSource::Local: return "local";
    case ControlSource::McpPilot: return "mcp-pilot";
    case ControlSource::Script: return "script";
    case ControlSource::Replay: return "replay";
    case ControlSource::Test: return "test";
  }
  return "unknown";
}

const char* controlKindName(ControlKind k) {
  switch (k) {
    case ControlKind::Edit: return "edit";
    case ControlKind::Ray: return "ray";
    case ControlKind::PlayerChunk: return "player-chunk";
    case ControlKind::Locomotion: return "locomotion";
    case ControlKind::ControlFrame: return "control-frame";
  }
  return "unknown";
}

// ---------------------------------------------------------------- stream

void ControlStream::submit(ControlEvent e) {
  e.seq = 0;
  e.tick = 0;  // stamps are assigned at the PRE phase, never by producers
  {
    std::lock_guard l(inboundM_);
    inbound_.push_back(e);
  }
  submitted_.fetch_add(1, std::memory_order_relaxed);
}

void ControlStream::submitStamped(std::vector<ControlEvent> evts) {
  if (evts.empty()) return;
  {
    std::lock_guard l(injectM_);
    inject_.insert(inject_.end(), std::make_move_iterator(evts.begin()), std::make_move_iterator(evts.end()));
  }
  injected_.fetch_add(evts.size(), std::memory_order_relaxed);
}

void ControlStream::insertSorted(ControlEvent e) {
  // pending_ stays sorted by (tick, seq); equal keys keep insertion order
  // (upper_bound), so application order is stable across identical stamps.
  const auto it = std::upper_bound(pending_.begin(), pending_.end(), e,
                                   [](const ControlEvent& a, const ControlEvent& b) {
                                     return a.tick != b.tick ? a.tick < b.tick : a.seq < b.seq;
                                   });
  pending_.insert(it, std::move(e));
}

void ControlStream::prePhase(uint64_t stampTick) {
  std::vector<ControlEvent> live, injected;
  {
    std::lock_guard l(inboundM_);
    live.swap(inbound_);
  }
  {
    std::lock_guard l(injectM_);
    injected.swap(inject_);
  }
  for (ControlEvent& e : injected) {
    if (e.seq >= nextSeq_) nextSeq_ = e.seq + 1;  // the seq space never collides
    insertSorted(std::move(e));
  }
  for (ControlEvent& e : live) {
    e.seq = nextSeq_++;
    e.tick = stampTick;
    stamped_.fetch_add(1, std::memory_order_relaxed);
    insertSorted(std::move(e));
  }
  const uint64_t now = pending_.size();
  pendingNow_.store(now, std::memory_order_relaxed);
  uint64_t m = maxPending_.load(std::memory_order_relaxed);
  while (now > m && !maxPending_.compare_exchange_weak(m, now, std::memory_order_relaxed)) {
  }
}

size_t ControlStream::extractDue(uint64_t tick, std::vector<ControlEvent>& out) {
  size_t n = 0;
  while (!pending_.empty() && pending_.front().tick <= tick) {
    ControlEvent e = pending_.front();
    pending_.erase(pending_.begin());
    if (haveApplied_ && (e.tick < lastAppliedTick_ || (e.tick == lastAppliedTick_ && e.seq <= lastAppliedSeq_)))
      orderViolations_.fetch_add(1, std::memory_order_relaxed);
    if (e.tick < tick) lateApplied_.fetch_add(1, std::memory_order_relaxed);
    lastAppliedTick_ = e.tick;
    lastAppliedSeq_ = e.seq;
    haveApplied_ = true;
    applied_.fetch_add(1, std::memory_order_relaxed);
    ++n;
    out.push_back(e);
    if (recording_.load(std::memory_order_acquire)) {
      std::lock_guard l(recordM_);
      record_.push_back(e);
    }
  }
  pendingNow_.store(pending_.size(), std::memory_order_relaxed);
  return n;
}

void ControlStream::takeRecord(std::vector<ControlEvent>& out) {
  std::lock_guard l(recordM_);
  out.insert(out.end(), std::make_move_iterator(record_.begin()), std::make_move_iterator(record_.end()));
  record_.clear();
}

ControlStreamStats ControlStream::stats() const {
  ControlStreamStats s;
  s.submitted = submitted_.load(std::memory_order_relaxed);
  s.injected = injected_.load(std::memory_order_relaxed);
  s.stamped = stamped_.load(std::memory_order_relaxed);
  s.applied = applied_.load(std::memory_order_relaxed);
  s.lateApplied = lateApplied_.load(std::memory_order_relaxed);
  s.dropped = 0;  // no code path discards an event (see the header contract)
  s.orderViolations = orderViolations_.load(std::memory_order_relaxed);
  s.pendingNow = pendingNow_.load(std::memory_order_relaxed);
  s.maxPending = maxPending_.load(std::memory_order_relaxed);
  return s;
}

// ---------------------------------------------------------------- format

namespace {

void putU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
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
void putI32(std::vector<uint8_t>& b, int32_t v) { putU32(b, uint32_t(v)); }
void putF32(std::vector<uint8_t>& b, float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  putU32(b, bits);
}

struct Reader {
  const uint8_t* p = nullptr;
  size_t n = 0, at = 0;
  bool u8(uint8_t& v) {
    if (at + 1 > n) return false;
    v = p[at++];
    return true;
  }
  bool u16(uint16_t& v) {
    uint8_t a, b;
    if (!u8(a) || !u8(b)) return false;
    v = uint16_t(a) | (uint16_t(b) << 8);
    return true;
  }
  bool u32(uint32_t& v) {
    v = 0;
    for (int i = 0; i < 4; ++i) {
      uint8_t b;
      if (!u8(b)) return false;
      v |= uint32_t(b) << (8 * i);
    }
    return true;
  }
  bool u64(uint64_t& v) {
    v = 0;
    for (int i = 0; i < 8; ++i) {
      uint8_t b;
      if (!u8(b)) return false;
      v |= uint64_t(b) << (8 * i);
    }
    return true;
  }
  bool i32(int32_t& v) {
    uint32_t u;
    if (!u32(u)) return false;
    v = int32_t(u);
    return true;
  }
  bool f32(float& v) {
    uint32_t bits;
    if (!u32(bits)) return false;
    std::memcpy(&v, &bits, 4);
    return true;
  }
};

constexpr size_t kHeaderBytes = 48;  // magic(4) ver(2) hdrLen(2) seed(8) mode(1) rsvd(1) tps(2) phases(2) rsvd(2) ticks(8) events(8) rsvd(8)

}  // namespace

bool writeControlStream(const ControlStreamHeader& header, const std::vector<ControlEvent>& events,
                        std::vector<uint8_t>& out) {
  out.clear();
  out.reserve(kHeaderBytes + events.size() * 64 + 8);
  out.insert(out.end(), kControlStreamMagic, kControlStreamMagic + 4);
  putU16(out, uint16_t(kControlStreamFormatVersion));
  putU16(out, uint16_t(kHeaderBytes));
  putU64(out, header.worldSeed);
  putU8(out, header.mode);
  putU8(out, 0);
  putU16(out, header.tickRate);
  putU16(out, header.phasesPerCycle);
  putU16(out, 0);
  putU64(out, header.declaredTicks);
  putU64(out, header.declaredEvents);
  putU64(out, 0);  // reserved (keeps the header at 48 bytes)
  for (const ControlEvent& e : events) {
    putU64(out, e.seq);
    putU64(out, e.tick);
    putU8(out, uint8_t(e.kind));
    putU8(out, uint8_t(e.source));
    putU16(out, e.sourceId);
    switch (e.kind) {
      case ControlKind::Edit:
        putU8(out, uint8_t(e.edit.kind));
        putU8(out, e.edit.facing);
        putU16(out, e.edit.block);
        putI32(out, e.edit.x);
        putI32(out, e.edit.y);
        putI32(out, e.edit.z);
        putU16(out, e.edit.command.code);
        putI32(out, e.edit.command.x);
        putI32(out, e.edit.command.y);
        putI32(out, e.edit.command.z);
        putI32(out, e.edit.command.a);
        putI32(out, e.edit.command.b);
        break;
      case ControlKind::Ray:
        putF32(out, e.ray.ox); putF32(out, e.ray.oy); putF32(out, e.ray.oz);
        putF32(out, e.ray.dx); putF32(out, e.ray.dy); putF32(out, e.ray.dz);
        break;
      case ControlKind::PlayerChunk:
        putI32(out, e.chunk.cx);
        putI32(out, e.chunk.cz);
        break;
      case ControlKind::Locomotion:
        putF32(out, e.loco.wishX);
        putF32(out, e.loco.wishZ);
        putU8(out, e.loco.flags);
        putU8(out, 0); putU8(out, 0); putU8(out, 0);
        break;
      case ControlKind::ControlFrame:
        putF32(out, e.frame.moveForward);
        putF32(out, e.frame.moveStrafe);
        putF32(out, e.frame.lookDx);
        putF32(out, e.frame.lookDy);
        putU8(out, e.frame.flags);
        putU8(out, uint8_t(e.frame.hotbarSlot));
        putU16(out, 0);
        break;
      default:
        return false;  // unknown kind: refuse to write an unreadable record
    }
  }
  const uint64_t sum = DeterministicScheduler::hashBytes(out.data(), out.size());
  putU64(out, sum);
  return true;
}

ControlStreamParse parseControlStream(const uint8_t* data, size_t n) {
  ControlStreamParse r;
  auto fail = [&](const char* why) {
    r.ok = false;
    r.error = why;
    r.events.clear();
    return r;
  };
  if (n < kHeaderBytes + 8) return fail("control stream: shorter than header + checksum");
  // Checksum first (fail-closed): everything after this is trusted layout.
  uint64_t stored = 0;
  for (int i = 0; i < 8; ++i) stored |= uint64_t(data[n - 8 + size_t(i)]) << (8 * i);
  if (DeterministicScheduler::hashBytes(data, n - 8) != stored) return fail("control stream: checksum mismatch");
  Reader rd{data, n - 8, 0};
  uint8_t m0, m1, m2, m3;
  if (!rd.u8(m0) || !rd.u8(m1) || !rd.u8(m2) || !rd.u8(m3) || m0 != 'L' || m1 != 'L' || m2 != 'C' || m3 != 'S')
    return fail("control stream: bad magic");
  uint16_t version = 0, hdrLen = 0;
  if (!rd.u16(version) || !rd.u16(hdrLen)) return fail("control stream: truncated header");
  if (version != kControlStreamFormatVersion) return fail("control stream: unsupported format version");
  if (hdrLen != kHeaderBytes) return fail("control stream: unexpected header length");
  uint16_t tps = 0, phases = 0, rsvd16 = 0;
  uint8_t rsvd8 = 0;
  uint64_t rsvd64 = 0;
  if (!rd.u64(r.header.worldSeed) || !rd.u8(r.header.mode) || !rd.u8(rsvd8) || !rd.u16(tps) || !rd.u16(phases) ||
      !rd.u16(rsvd16) || !rd.u64(r.header.declaredTicks) || !rd.u64(r.header.declaredEvents) || !rd.u64(rsvd64))
    return fail("control stream: truncated header");
  r.header.tickRate = tps;
  r.header.phasesPerCycle = phases;
  while (rd.at < rd.n) {
    ControlEvent e;
    uint8_t kind = 0, source = 0;
    if (!rd.u64(e.seq) || !rd.u64(e.tick) || !rd.u8(kind) || !rd.u8(source) || !rd.u16(e.sourceId))
      return fail("control stream: truncated record");
    if (kind < uint8_t(ControlKind::Edit) || kind > uint8_t(ControlKind::ControlFrame))
      return fail("control stream: unknown event kind");
    if (source > uint8_t(ControlSource::Test)) return fail("control stream: unknown event source");
    e.kind = ControlKind(kind);
    e.source = ControlSource(source);
    bool ok = true;
    switch (e.kind) {
      case ControlKind::Edit: {
        uint8_t ek = 0;
        ok = rd.u8(ek) && rd.u8(e.edit.facing) && rd.u16(e.edit.block) && rd.i32(e.edit.x) && rd.i32(e.edit.y) &&
             rd.i32(e.edit.z) && rd.u16(e.edit.command.code) && rd.i32(e.edit.command.x) && rd.i32(e.edit.command.y) &&
             rd.i32(e.edit.command.z) && rd.i32(e.edit.command.a) && rd.i32(e.edit.command.b);
        if (ek > uint8_t(EditCmd::Kind::Command)) return fail("control stream: unknown edit kind");
        e.edit.kind = EditCmd::Kind(ek);
        e.edit.tick = e.tick;  // the record's stamp is the edit's tick
        break;
      }
      case ControlKind::Ray:
        ok = rd.f32(e.ray.ox) && rd.f32(e.ray.oy) && rd.f32(e.ray.oz) && rd.f32(e.ray.dx) && rd.f32(e.ray.dy) &&
             rd.f32(e.ray.dz);
        break;
      case ControlKind::PlayerChunk:
        ok = rd.i32(e.chunk.cx) && rd.i32(e.chunk.cz);
        break;
      case ControlKind::Locomotion: {
        uint8_t pad = 0;
        ok = rd.f32(e.loco.wishX) && rd.f32(e.loco.wishZ) && rd.u8(e.loco.flags) && rd.u8(pad) && rd.u8(pad) &&
             rd.u8(pad);
        break;
      }
      case ControlKind::ControlFrame: {
        uint8_t slot = 0;
        uint16_t pad = 0;
        ok = rd.f32(e.frame.moveForward) && rd.f32(e.frame.moveStrafe) && rd.f32(e.frame.lookDx) &&
             rd.f32(e.frame.lookDy) && rd.u8(e.frame.flags) && rd.u8(slot) && rd.u16(pad);
        e.frame.hotbarSlot = int8_t(slot);
        break;
      }
    }
    if (!ok) return fail("control stream: truncated payload");
    r.events.push_back(e);
  }
  if (rd.at != rd.n) return fail("control stream: trailing bytes");
  r.ok = true;
  return r;
}

std::string controlEventToText(const ControlEvent& e) {
  char buf[512];
  const char* src = controlSourceName(e.source);
  switch (e.kind) {
    case ControlKind::Edit: {
      const char* ek = e.edit.kind == EditCmd::Kind::Break    ? "break"
                       : e.edit.kind == EditCmd::Kind::Place  ? "place"
                       : e.edit.kind == EditCmd::Kind::Use    ? "use"
                                                              : "command";
      if (e.edit.kind == EditCmd::Kind::Command)
        std::snprintf(buf, sizeof buf, "seq=%llu tick=%llu src=%s#%u kind=edit edit=%s code=%u a=%d b=%d",
                      (unsigned long long)e.seq, (unsigned long long)e.tick, src, e.sourceId, ek,
                      e.edit.command.code, e.edit.command.a, e.edit.command.b);
      else if (e.edit.kind == EditCmd::Kind::Use)
        std::snprintf(buf, sizeof buf, "seq=%llu tick=%llu src=%s#%u kind=edit edit=%s cell=(%d,%d,%d)",
                      (unsigned long long)e.seq, (unsigned long long)e.tick, src, e.sourceId, ek, e.edit.x, e.edit.y,
                      e.edit.z);
      else
        std::snprintf(buf, sizeof buf, "seq=%llu tick=%llu src=%s#%u kind=edit edit=%s block=%u facing=%u",
                      (unsigned long long)e.seq, (unsigned long long)e.tick, src, e.sourceId, ek, e.edit.block,
                      e.edit.facing);
      break;
    }
    case ControlKind::Ray:
      std::snprintf(buf, sizeof buf, "seq=%llu tick=%llu src=%s#%u kind=ray o=(%.2f,%.2f,%.2f) d=(%.3f,%.3f,%.3f)",
                    (unsigned long long)e.seq, (unsigned long long)e.tick, src, e.sourceId, e.ray.ox, e.ray.oy,
                    e.ray.oz, e.ray.dx, e.ray.dy, e.ray.dz);
      break;
    case ControlKind::PlayerChunk:
      std::snprintf(buf, sizeof buf, "seq=%llu tick=%llu src=%s#%u kind=player-chunk chunk=(%d,%d)",
                    (unsigned long long)e.seq, (unsigned long long)e.tick, src, e.sourceId, e.chunk.cx, e.chunk.cz);
      break;
    case ControlKind::Locomotion:
      std::snprintf(buf, sizeof buf, "seq=%llu tick=%llu src=%s#%u kind=locomotion wish=(%.3f,%.3f) flags=%u",
                    (unsigned long long)e.seq, (unsigned long long)e.tick, src, e.sourceId, e.loco.wishX, e.loco.wishZ,
                    e.loco.flags);
      break;
    case ControlKind::ControlFrame:
      std::snprintf(buf, sizeof buf,
                    "seq=%llu tick=%llu src=%s#%u kind=control-frame move=(%.3f,%.3f) look=(%.1f,%.1f) flags=%u slot=%d",
                    (unsigned long long)e.seq, (unsigned long long)e.tick, src, e.sourceId, e.frame.moveForward,
                    e.frame.moveStrafe, e.frame.lookDx, e.frame.lookDy, e.frame.flags, int(e.frame.hotbarSlot));
      break;
    default:
      std::snprintf(buf, sizeof buf, "seq=%llu tick=%llu src=%s#%u kind=unknown(%u)", (unsigned long long)e.seq,
                    (unsigned long long)e.tick, src, e.sourceId, unsigned(e.kind));
      break;
  }
  return buf;
}

}  // namespace litholattice
