#include "litholattice/net/protocol.h"

#include <algorithm>
#include <cstdint>

namespace litholattice {
namespace net {

bool encodeHeader(const PacketHeader& h, quantize::ByteWriter& out) {
  out.putU16LE(kMagic);
  out.putU8(h.version);
  out.putU8(h.flags);
  out.putU16LE(h.seq);
  out.putU16LE(h.ack);
  out.putU16LE(h.ackBits);
  out.putU16LE(static_cast<uint16_t>(h.type));
  out.putU16LE(h.payloadLen);
  return true;
}

bool decodeHeader(quantize::ByteReader& in, PacketHeader& out, uint8_t acceptedVersion,
                  HeaderReject* reject) {
  PacketHeader h;
  uint16_t magic, type;
  if (!in.getU16LE(magic) || magic != kMagic) return false;
  if (!in.getU8(h.version)) return false;
  if (h.version != acceptedVersion) {
    if (reject) {
      reject->versionMismatch = true;
      reject->got = h.version;
    }
    return false;
  }
  if (!in.getU8(h.flags)) return false;
  if (!in.getU16LE(h.seq)) return false;
  if (!in.getU16LE(h.ack)) return false;
  if (!in.getU16LE(h.ackBits)) return false;
  if (!in.getU16LE(type)) return false;
  h.type = static_cast<PayloadType>(type);
  if (!in.getU16LE(h.payloadLen)) return false;
  out = h;
  return true;
}

uint8_t entityDiffMask(const SnapshotEntity& base, const SnapshotEntity& cur) {
  uint8_t mask = 0;
  if (base.x != cur.x || base.y != cur.y || base.z != cur.z) mask |= kFieldPos;
  // Effective velocity: atRest implies zero on both sides (the codec
  // normalizes stored velocity to 0 while atRest; see encodeSnapshot).
  const int32_t bvx = base.atRest ? 0 : base.vx;
  const int32_t bvy = base.atRest ? 0 : base.vy;
  const int32_t bvz = base.atRest ? 0 : base.vz;
  const int32_t cvx = cur.atRest ? 0 : cur.vx;
  const int32_t cvy = cur.atRest ? 0 : cur.vy;
  const int32_t cvz = cur.atRest ? 0 : cur.vz;
  if (bvx != cvx || bvy != cvy || bvz != cvz) mask |= kFieldVel;
  const bool bOrient = (base.flags & kEntHasOrient) != 0;
  const bool cOrient = (cur.flags & kEntHasOrient) != 0;
  if (bOrient != cOrient || (cOrient && base.orient != cur.orient)) mask |= kFieldOrient;
  if (base.flags != cur.flags) mask |= kFieldFlags;
  if (base.atRest != cur.atRest) mask |= kFieldAtRest;
  return mask;
}

namespace {

// Binary search over a sorted entity vector. Returns index or -1.
int findEntity(const std::vector<SnapshotEntity>& v, uint32_t id) {
  auto it = std::lower_bound(v.begin(), v.end(), id,
                             [](const SnapshotEntity& e, uint32_t i) { return e.id < i; });
  if (it != v.end() && it->id == id) return static_cast<int>(it - v.begin());
  return -1;
}

// Saturating int32 add (audit LOW 2026-10-11): wire deltas span the full
// int32 range (attacker-controlled), so base + delta in int32 was signed-
// overflow UB. Accumulate in int64 and clamp: decode stays total with
// defined (clamped) results instead of wrapping to garbage positions.
int32_t satAdd(int32_t a, int32_t b) {
  const int64_t s = static_cast<int64_t>(a) + static_cast<int64_t>(b);
  if (s > static_cast<int64_t>(INT32_MAX)) return INT32_MAX;
  if (s < static_cast<int64_t>(INT32_MIN)) return INT32_MIN;
  return static_cast<int32_t>(s);
}

// Encode one entity record into `tmp`. `base` is null for spawned records
// (absolute encoding); otherwise only changed fields are written as deltas.
// Saturating int32 delta: computed in 64-bit to avoid signed-overflow UB when
// positions disagree wildly (corrupt/malicious base). A >2B-block delta is not
// a legitimate teleport; saturate rather than invoke UB.
static int32_t satDelta(int32_t cur, int32_t base) {
  const int64_t d = int64_t(cur) - int64_t(base);
  if (d > int64_t(INT32_MAX)) return INT32_MAX;
  if (d < int64_t(INT32_MIN)) return INT32_MIN;
  return int32_t(d);
}

void encodeRecord(quantize::ByteWriter& tmp, const SnapshotEntity& cur,
                  const SnapshotEntity* base) {
  uint8_t mask;
  if (!base) {
    mask = kFieldSpawned | kFieldPos | kFieldFlags;
    if (!cur.atRest) mask |= kFieldVel;
    if (cur.flags & kEntHasOrient) mask |= kFieldOrient;
    if (cur.atRest) mask |= kFieldAtRest;
  } else {
    mask = entityDiffMask(*base, cur);
  }
  tmp.putU8(mask);
  if (!base) {
    // Absolute record. Field presence on the wire:
    //   pos: always. vel: iff !atRest (mask bit). orient: iff kFieldOrient
    //   (which the encoder set exactly when flags carries kEntHasOrient).
    //   flags: always.
    tmp.putU32LE(static_cast<uint32_t>(cur.x));
    tmp.putU32LE(static_cast<uint32_t>(cur.y));
    tmp.putU32LE(static_cast<uint32_t>(cur.z));
    if (!cur.atRest) {
      tmp.putU32LE(static_cast<uint32_t>(cur.vx));
      tmp.putU32LE(static_cast<uint32_t>(cur.vy));
      tmp.putU32LE(static_cast<uint32_t>(cur.vz));
    }
    if (mask & kFieldOrient) tmp.putU32LE(cur.orient);
    tmp.putU32LE(cur.flags);
  } else {
    if (mask & kFieldPos) {
      tmp.putVarint(satDelta(cur.x, base->x));
      tmp.putVarint(satDelta(cur.y, base->y));
      tmp.putVarint(satDelta(cur.z, base->z));
    }
    if (mask & kFieldVel) {
      const int32_t bvx = base->atRest ? 0 : base->vx;
      const int32_t bvy = base->atRest ? 0 : base->vy;
      const int32_t bvz = base->atRest ? 0 : base->vz;
      tmp.putVarint(satDelta(cur.vx, bvx));
      tmp.putVarint(satDelta(cur.vy, bvy));
      tmp.putVarint(satDelta(cur.vz, bvz));
    }
    if (mask & kFieldOrient) tmp.putU32LE(cur.orient);
    if (mask & kFieldFlags) tmp.putUVarint(cur.flags);
    // kFieldAtRest carries no bytes; the bit alone flips the state.
  }
}

}  // namespace

bool encodeSnapshot(const Snapshot& current, const Snapshot* baseline, uint32_t baselineSeq,
                    size_t maxBytes, quantize::ByteWriter& out, bool& truncated,
                    EncodePackedInfo* packedOut) {
  truncated = false;
  if (packedOut) {
    packedOut->packedIds.clear();
    packedOut->removedIds.clear();
  }
  // Normalize + sort: entities hit the wire in id order for gap coding.
  // atRest normalizes stored velocity to zero so future deltas stay small and
  // round-trips stay exact.
  std::vector<SnapshotEntity> sorted = current.entities;
  for (auto& e : sorted)
    if (e.atRest) {
      e.vx = e.vy = e.vz = 0;
    }
  std::sort(sorted.begin(), sorted.end(),
            [](const SnapshotEntity& a, const SnapshotEntity& b) { return a.id < b.id; });

  const std::vector<SnapshotEntity>* baseVec = baseline ? &baseline->entities : nullptr;

  // Removals: baseline ids not present now. Linear merge over two sorted
  // ranges. Packed first: a ghost entity is worse than a stale position.
  quantize::ByteWriter remBody;
  std::vector<uint32_t> removed;
  if (baseVec) {
    size_t bi = 0, ci = 0;
    while (bi < baseVec->size()) {
      const uint32_t bid = (*baseVec)[bi].id;
      const uint32_t cid = ci < sorted.size() ? sorted[ci].id : UINT32_MAX;
      if (bid < cid) {
        removed.push_back(bid);
        ++bi;
      } else if (bid == cid) {
        ++bi;
        ++ci;
      } else {
        ++ci;
      }
    }
  }
  remBody.putUVarint(static_cast<uint32_t>(removed.size()));
  uint32_t prev = 0;
  bool rfirst = true;
  for (uint32_t id : removed) {
    remBody.putUVarint(rfirst ? id : id - prev - 1);
    rfirst = false;
    prev = id;
  }

  // Records, packed until the byte budget would be exceeded. Each record is
  // staged in a temp buffer so the budget check is exact.
  quantize::ByteWriter recBody;
  quantize::ByteWriter gap, rec;
  uint32_t prevId = 0;
  bool first = true;
  uint32_t nPacked = 0;
  // Reserve room for the fixed prefix (tick 4 + baselineSeq 4 + count <=5).
  size_t budget = maxBytes > 13 ? maxBytes - 13 : 0;
  for (const auto& e : sorted) {
    const SnapshotEntity* base = nullptr;
    if (baseVec) {
      const int bi = findEntity(*baseVec, e.id);
      if (bi >= 0) base = &(*baseVec)[static_cast<size_t>(bi)];
    }
    if (base && entityDiffMask(*base, e) == 0) continue;  // unchanged: zero bytes
    gap.buf.clear();
    rec.buf.clear();
    gap.putUVarint(first ? e.id : e.id - prevId - 1);
    encodeRecord(rec, e, base);
    if (recBody.size() + remBody.size() + gap.size() + rec.size() > budget) {
      truncated = true;
      break;
    }
    recBody.putBytes(gap.data(), gap.size());
    recBody.putBytes(rec.data(), rec.size());
    first = false;
    prevId = e.id;
    ++nPacked;
    if (packedOut) packedOut->packedIds.push_back(e.id);  // wire order = id ascending
  }

  out.putU32LE(current.tick);
  out.putU32LE(baselineSeq);
  out.putUVarint(nPacked);
  out.putBytes(recBody.data(), recBody.size());
  out.putBytes(remBody.data(), remBody.size());
  if (packedOut) packedOut->removedIds = removed;
  return true;
}

bool decodeSnapshotPrefix(quantize::ByteReader& in, uint32_t& tick, uint32_t& baselineSeq) {
  return in.getU32LE(tick) && in.getU32LE(baselineSeq);
}

bool decodeSnapshotBody(quantize::ByteReader& in, const Snapshot* baseline, uint32_t tick,
                        Snapshot& out) {
  // Decoded update/spawn records (sorted by id) and removed ids (sorted).
  std::vector<SnapshotEntity> records;
  std::vector<uint32_t> removedIds;
  uint32_t nUpdated;
  if (!in.getUVarint(nUpdated)) return false;

  const std::vector<SnapshotEntity>* baseVec = baseline ? &baseline->entities : nullptr;
  uint32_t prevId = 0;
  bool first = true;
  for (uint32_t i = 0; i < nUpdated; ++i) {
    uint32_t gap;
    if (!in.getUVarint(gap)) return false;
    const uint64_t id64 = first ? gap : static_cast<uint64_t>(prevId) + 1 + gap;
    if (id64 > UINT32_MAX) return false;  // malformed: id overflow
    const uint32_t id = static_cast<uint32_t>(id64);
    first = false;
    prevId = id;

    uint8_t mask;
    if (!in.getU8(mask)) return false;
    const bool spawned = (mask & kFieldSpawned) != 0;

    int baseIdx = -1;
    if (baseVec) baseIdx = findEntity(*baseVec, id);
    if (spawned && baseIdx >= 0) return false;  // "spawn" of a known entity
    if (!spawned && baseIdx < 0) return false;  // delta for an unknown entity

    SnapshotEntity e;
    e.id = id;
    if (spawned) {
      uint32_t x, y, z;
      if (!in.getU32LE(x) || !in.getU32LE(y) || !in.getU32LE(z)) return false;
      e.x = static_cast<int32_t>(x);
      e.y = static_cast<int32_t>(y);
      e.z = static_cast<int32_t>(z);
      e.atRest = (mask & kFieldAtRest) != 0;
      if (!e.atRest) {
        uint32_t vx, vy, vz;
        if (!in.getU32LE(vx) || !in.getU32LE(vy) || !in.getU32LE(vz)) return false;
        e.vx = static_cast<int32_t>(vx);
        e.vy = static_cast<int32_t>(vy);
        e.vz = static_cast<int32_t>(vz);
      }
      // For spawns the mask's kFieldOrient bit is the orient presence bit
      // (encoder sets it iff flags carries kEntHasOrient).
      if (mask & kFieldOrient) {
        uint32_t o;
        if (!in.getU32LE(o)) return false;
        e.orient = o;
      }
      uint32_t flags;
      if (!in.getU32LE(flags)) return false;
      e.flags = flags;
      e.hasOrient = (flags & kEntHasOrient) != 0;
    } else {
      e = (*baseVec)[static_cast<size_t>(baseIdx)];  // start from the baseline
      if (mask & kFieldPos) {
        int32_t dx, dy, dz;
        if (!in.getVarint(dx) || !in.getVarint(dy) || !in.getVarint(dz)) return false;
        e.x = satAdd(e.x, dx);
        e.y = satAdd(e.y, dy);
        e.z = satAdd(e.z, dz);
      }
      if (mask & kFieldVel) {
        int32_t dvx, dvy, dvz;
        if (!in.getVarint(dvx) || !in.getVarint(dvy) || !in.getVarint(dvz)) return false;
        // Baseline side still has the OLD atRest state here (the flip is
        // applied below), matching the encoder's delta basis.
        const int32_t bvx = e.atRest ? 0 : e.vx;
        const int32_t bvy = e.atRest ? 0 : e.vy;
        const int32_t bvz = e.atRest ? 0 : e.vz;
        e.vx = satAdd(bvx, dvx);
        e.vy = satAdd(bvy, dvy);
        e.vz = satAdd(bvz, dvz);
      }
      if (mask & kFieldFlags) {
        uint32_t f;
        if (!in.getUVarint(f)) return false;
        e.flags = f;
      }
      if (mask & kFieldOrient) {
        uint32_t o;
        if (!in.getU32LE(o)) return false;
        e.orient = o;
      }
      if (mask & kFieldAtRest) e.atRest = !e.atRest;
      e.hasOrient = (e.flags & kEntHasOrient) != 0;
      if (e.atRest) {
        e.vx = e.vy = e.vz = 0;  // normalize, mirrors the encoder
      }
    }
    if (!records.empty() && records.back().id >= id) return false;  // id order
    records.push_back(e);
  }

  uint32_t nRemoved;
  if (!in.getUVarint(nRemoved)) return false;
  {
    uint32_t prev = 0;
    bool rfirst = true;
    for (uint32_t i = 0; i < nRemoved; ++i) {
      uint32_t gap;
      if (!in.getUVarint(gap)) return false;
      const uint64_t id64 = rfirst ? gap : static_cast<uint64_t>(prev) + 1 + gap;
      if (id64 > UINT32_MAX) return false;
      rfirst = false;
      prev = static_cast<uint32_t>(id64);
      // A removal must name a baseline entity absent from the new set.
      if (!baseVec || findEntity(*baseVec, prev) < 0) return false;
      if (findEntity(records, prev) >= 0) return false;
      removedIds.push_back(prev);
    }
  }

  // A delta + its baseline = the full new state: carry over every baseline
  // entity that was neither removed nor updated, then merge the records in.
  // All three lists are sorted by id, so this is ONE linear three-way merge
  // (opt 2026-10-11): no second vector, no second pass.
  Snapshot result;
  result.tick = tick;
  if (baseVec) {
    result.entities.reserve(baseVec->size() + records.size());
    size_t ri = 0, ui = 0;
    for (const auto& be : *baseVec) {
      while (ri < removedIds.size() && removedIds[ri] < be.id) ++ri;  // defensive
      if (ri < removedIds.size() && removedIds[ri] == be.id) continue;  // removed
      while (ui < records.size() && records[ui].id < be.id)
        result.entities.push_back(std::move(records[ui++]));  // record before this base id
      if (ui < records.size() && records[ui].id == be.id)
        result.entities.push_back(std::move(records[ui++]));  // updated wins
      else
        result.entities.push_back(be);  // carried over
    }
    while (ui < records.size())
      result.entities.push_back(std::move(records[ui++]));  // tail records
  } else {
    // Full snapshot: the records ARE the state (sorted, spawn-only).
    result.entities = std::move(records);
  }

  out = result;
  return true;
}

bool decodeSnapshot(quantize::ByteReader& in, const Snapshot* baseline, Snapshot& out) {
  uint32_t tick, baselineSeq;
  if (!decodeSnapshotPrefix(in, tick, baselineSeq)) return false;
  if (baselineSeq == 0) {
    if (baseline != nullptr) return false;
  } else {
    if (baseline == nullptr || baseline->seq != baselineSeq) return false;
  }
  return decodeSnapshotBody(in, baseline, tick, out);
}

// ---------------------------------------------------------------------------
// v3: stable-ID wire records (Infinite Registry).
// ---------------------------------------------------------------------------

void encodeBlockUpdate(const BlockUpdate& u, quantize::ByteWriter& out) {
  out.putVarint(u.x);
  out.putVarint(u.y);
  out.putVarint(u.z);
  out.putUVarint64(u.stableId);
}

bool decodeBlockUpdate(quantize::ByteReader& in, BlockUpdate& out) {
  BlockUpdate tmp;
  if (!in.getVarint(tmp.x)) return false;
  if (!in.getVarint(tmp.y)) return false;
  if (!in.getVarint(tmp.z)) return false;
  if (!in.getUVarint64(tmp.stableId)) return false;
  out = tmp;
  return true;
}

}  // namespace net
}  // namespace litholattice
