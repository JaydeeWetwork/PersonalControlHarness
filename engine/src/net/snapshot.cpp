#include "litholattice/net/snapshot.h"

#include <algorithm>

namespace litholattice {
namespace net {

// --- SnapshotClient ----------------------------------------------------------

SnapshotClient::SnapshotClient(SnapshotConfig cfg) : cfg_(cfg) {
  if (cfg_.snapshotHz == 0) cfg_.snapshotHz = 32;
  if (cfg_.historyDepth == 0) cfg_.historyDepth = 64;
  if (cfg_.maxPayloadBytes == 0) cfg_.maxPayloadBytes = 1200;
}

uint32_t SnapshotClient::divisor() const {
  const uint32_t d = (cfg_.simHz + cfg_.snapshotHz / 2) / cfg_.snapshotHz;
  return d == 0 ? 1 : d;
}

const Snapshot* SnapshotClient::findSnapshot(uint32_t seq) const {
  for (auto it = history_.rbegin(); it != history_.rend(); ++it)
    if (it->seq == seq) return &(*it);
  return nullptr;
}

void SnapshotClient::evictThrough(uint32_t seq) {
  // Drop everything strictly older than `seq`; the baseline entry itself stays.
  while (!history_.empty() && seqGreater(static_cast<uint16_t>(seq), static_cast<uint16_t>(history_.front().seq)))
    history_.pop_front();
}

std::vector<uint8_t> SnapshotClient::build(uint64_t tick,
                                           const std::vector<SnapshotEntity>& entities,
                                           size_t byteBudget) {
  return buildNormalized(normalizeForSnapshot(tick, entities), byteBudget);
}

Snapshot SnapshotClient::normalizeForSnapshot(uint64_t tick,
                                              const std::vector<SnapshotEntity>& entities) {
  // Normalized working copy: atRest zeroes velocity, sorted by id. This is
  // exactly what the client will decode, so it is also the stored baseline.
  Snapshot cur;
  cur.tick = static_cast<uint32_t>(tick);
  cur.entities = entities;
  for (auto& e : cur.entities)
    if (e.atRest) {
      e.vx = e.vy = e.vz = 0;
    }
  std::sort(cur.entities.begin(), cur.entities.end(),
            [](const SnapshotEntity& a, const SnapshotEntity& b) { return a.id < b.id; });
  return cur;
}

void SnapshotClient::requestFull() {
  if (forceFull_) return;  // a full is already pending: nothing to do
  // Rate-limit the client-driven path (audit LOW 2026-10-11): a malicious
  // client setting kFlagNeedFull on every ack gets forced fulls at ~1/s,
  // not 32 Hz. Excess requests are dropped and counted.
  const uint64_t since =
      everForcedFull_ ? buildCount_ - lastForcedFullBuild_ : UINT64_MAX;
  if (since < cfg_.forcedFullMinInterval) {
    ++ignoredForcedFulls_;
    return;
  }
  forceFull_ = true;
  lastForcedFullBuild_ = buildCount_;
  everForcedFull_ = true;
}

std::vector<uint8_t> SnapshotClient::buildNormalized(const Snapshot& normalized,
                                                     size_t byteBudget) {
  ++buildCount_;
  const Snapshot& cur = normalized;  // caller-normalized: sorted, atRest zeroed

  const Snapshot* base = nullptr;
  uint32_t encBaseline = 0;
  if (!forceFull_ && baselineSeq_ != 0) {
    base = findSnapshot(baselineSeq_);
    if (base) {
      encBaseline = baselineSeq_;
    } else {
      forceFull_ = true;  // baseline evicted before ack advanced: resync
    }
  }

  const size_t budget0 = byteBudget ? byteBudget : cfg_.maxPayloadBytes;
  // Never emit a datagram over the ~1400-byte cap (header included).
  const size_t budget =
      budget0 > kMaxUdpPayload - kHeaderLen ? kMaxUdpPayload - kHeaderLen : budget0;
  quantize::ByteWriter payload;
  bool truncated = false;
  EncodePackedInfo packed;  // side-channel: which records/removals shipped
  encodeSnapshot(cur, base, encBaseline, budget, payload, truncated, &packed);
  forceFull_ = false;

  PacketHeader h;
  h.version = version_;  // negotiated version once the handshake agrees
  h.flags = truncated ? kFlagTruncated : 0;
  h.seq = nextSeq_++;
  if (nextSeq_ == 0) nextSeq_ = 1;  // seq 0 is reserved ("no baseline")
  h.type = PayloadType::Snapshot;
  h.payloadLen = static_cast<uint16_t>(payload.size());

  // The stored baseline must be exactly what the client decodes (the
  // byte-identical post-image), NOT the full input: under a byte budget the
  // wire snapshot may be truncated, and the client's next delta decodes
  // against the truncated state. A full-input baseline would silently diverge
  // (server: "unchanged", client: "never seen"). Splice the post-image from
  // the encode side-channel (opt 2026-10-11: no second codec pass over our
  // own output): packed records decode to the current values bit-exactly,
  // updates cut by the budget keep the baseline value, cut spawns never
  // shipped. cur.entities is id-ascending (normalizeForSnapshot) and
  // packed.packedIds is wire order (also id-ascending): linear merge.
  Snapshot post;
  post.tick = cur.tick;
  post.entities.reserve(cur.entities.size());
  {
    size_t pi = 0, bi = 0;
    const std::vector<SnapshotEntity>* bv = base ? &base->entities : nullptr;
    for (const auto& e : cur.entities) {
      while (pi < packed.packedIds.size() && packed.packedIds[pi] < e.id) ++pi;  // defensive
      if (pi < packed.packedIds.size() && packed.packedIds[pi] == e.id) {
        post.entities.push_back(e);  // packed: the client decoded exactly this
        ++pi;
        continue;
      }
      if (bv) {
        while (bi < bv->size() && (*bv)[bi].id < e.id) ++bi;
        if (bi < bv->size() && (*bv)[bi].id == e.id)
          post.entities.push_back((*bv)[bi]);  // update cut by the budget:
                                               // the client kept the base value
        // else: spawn cut by the budget: the client never saw it
      }
      // else (full snapshot): cut records simply never shipped
    }
  }
  post.seq = h.seq;
  history_.push_back(std::move(post));
  // Depth cap. Never evict the live baseline: if the oldest entry IS the
  // baseline (client silent for > depth snapshots), force a full resync
  // instead of orphaning it.
  while (history_.size() > cfg_.historyDepth) {
    if (history_.front().seq == baselineSeq_) {
      forceFull_ = true;
      baselineSeq_ = 0;
    }
    history_.pop_front();
  }

  quantize::ByteWriter packet;
  encodeHeader(h, packet);
  packet.putBytes(payload.data(), payload.size());
  return packet.buf;
}

void SnapshotClient::onAck(uint16_t ack, uint16_t ackBits) {
  if (ack == 0) return;  // seq 0 is reserved; nothing acked yet
  // Newest acked snapshot we still hold: header ack first, then ackBits.
  uint16_t best = 0;
  bool have = false;
  auto consider = [&](uint16_t s) {
    if (s == 0 || !findSnapshot(s)) return;
    if (!have || seqGreater(s, best)) {
      best = s;
      have = true;
    }
  };
  consider(ack);
  for (int i = 0; i < 16; ++i)
    if (ackBits & (1u << i)) consider(static_cast<uint16_t>(ack - 1 - i));
  // Never regress: an old or duplicate ack changes nothing.
  if (have && (baselineSeq_ == 0 || seqGreater(best, static_cast<uint16_t>(baselineSeq_)))) {
    baselineSeq_ = best;
    evictThrough(baselineSeq_);
  }
}

// --- SnapshotReceiver --------------------------------------------------------

const Snapshot* SnapshotReceiver::findSnapshot(uint32_t seq) const {
  for (auto it = history_.rbegin(); it != history_.rend(); ++it)
    if (it->seq == seq) return &(*it);
  return nullptr;
}

uint16_t SnapshotReceiver::ackBits() const {
  if (!haveSeq_) return 0;
  uint16_t bits = 0;
  for (int i = 0; i < 16; ++i) {
    const uint16_t s = static_cast<uint16_t>(highestSeq_ - 1 - i);
    if (seenSeq_[s & 63] == s) bits |= static_cast<uint16_t>(1u << i);
  }
  return bits;
}

SnapshotReceiver::Result SnapshotReceiver::receive(const uint8_t* data, size_t len) {
  quantize::ByteReader in(data, len);
  PacketHeader h;
  {
    lastReject_ = HeaderReject{};  // cleared per datagram; set below on a version mismatch
    bool ok = false;
    for (uint8_t v : accepted_) {
      quantize::ByteReader r(data, len);
      HeaderReject rej;
      if (decodeHeader(r, h, v, &rej)) {
        in = r;
        ok = true;
        break;
      }
      if (!rej.versionMismatch) break;  // bad magic/truncated: no version fixes that
      lastReject_ = rej;
    }
    if (!ok) return Result::Dropped;
  }
  if (h.type != PayloadType::Snapshot) return Result::Dropped;
  if (in.remaining() != h.payloadLen) return Result::Dropped;  // exact framing
  if (haveSeq_ && !seqGreater(h.seq, highestSeq_)) return Result::Dropped;  // dup / old

  // Peek at the baseline BEFORE decoding so we can look it up.
  // Re-reader over the payload for the two-phase decode.
  const uint8_t* payload = data + kHeaderLen;
  quantize::ByteReader pr(payload, h.payloadLen);
  uint32_t tick, baselineSeq;
  if (!decodeSnapshotPrefix(pr, tick, baselineSeq)) return Result::Dropped;

  const Snapshot* base = nullptr;
  if (baselineSeq != 0) {
    base = findSnapshot(baselineSeq);
    if (!base) {
      // Baseline unknown (never received, or evicted): record the seq so
      // acks stay fresh, flag needFull, and wait for the server's full
      // snapshot. Self-healing; no retransmits in this model.
      highestSeq_ = h.seq;
      haveSeq_ = true;
      seenSeq_[h.seq & 63] = h.seq;
      needFull_ = true;
      return Result::NeedBaseline;
    }
  }

  Snapshot snap;
  if (!decodeSnapshotBody(pr, base, tick, snap)) return Result::Dropped;
  if (!pr.eof()) return Result::Dropped;  // trailing garbage
  snap.seq = h.seq;

  history_.push_back(snap);
  while (history_.size() > 64) history_.pop_front();
  interp_.push_back(snap);
  while (interp_.size() > 3) interp_.pop_front();

  highestSeq_ = h.seq;
  haveSeq_ = true;
  seenSeq_[h.seq & 63] = h.seq;
  needFull_ = false;
  return Result::Applied;
}

bool SnapshotReceiver::sample(uint64_t tick, Snapshot& out) const {
  if (interp_.size() < 2) return false;
  if (tick <= interp_.front().tick) {
    out = interp_.front();  // clamp: hold oldest
    out.tick = static_cast<uint32_t>(tick);
    return true;
  }
  if (tick >= interp_.back().tick) {
    out = interp_.back();  // clamp: hold newest, never extrapolate
    out.tick = static_cast<uint32_t>(tick);
    return true;
  }
  for (size_t i = 0; i + 1 < interp_.size(); ++i) {
    if (interp_[i].tick <= tick && tick <= interp_[i + 1].tick) {
      interpolateSnapshots(interp_[i], interp_[i + 1], tick, out);
      return true;
    }
  }
  return false;  // unreachable
}

// --- interpolation -----------------------------------------------------------

void interpolateSnapshots(const Snapshot& a, const Snapshot& b, uint64_t tick, Snapshot& out) {
  Snapshot result;
  result.tick = static_cast<uint32_t>(tick);
  result.seq = b.seq;
  if (b.tick <= a.tick || tick <= a.tick) {
    result.entities = a.entities;
    out = result;
    return;
  }
  if (tick >= b.tick) {
    result.entities = b.entities;
    out = result;
    return;
  }
  // Q16 alpha in [0, 65536]. (tick - a.tick) < 2^32 so the shift is safe.
  const uint64_t alphaQ16 = ((tick - a.tick) << 16) / (b.tick - a.tick);
  auto lerp = [&](int32_t av, int32_t bv) -> int32_t {
    const int64_t d = static_cast<int64_t>(bv) - static_cast<int64_t>(av);
    return static_cast<int32_t>(static_cast<int64_t>(av) + ((d * static_cast<int64_t>(alphaQ16)) >> 16));
  };
  size_t ai = 0, bi = 0;
  while (ai < a.entities.size() || bi < b.entities.size()) {
    const uint32_t aid = ai < a.entities.size() ? a.entities[ai].id : UINT32_MAX;
    const uint32_t bid = bi < b.entities.size() ? b.entities[bi].id : UINT32_MAX;
    if (aid == bid) {
      const auto& ea = a.entities[ai];
      const auto& eb = b.entities[bi];
      SnapshotEntity e;
      e.id = aid;
      e.x = lerp(ea.x, eb.x);
      e.y = lerp(ea.y, eb.y);
      e.z = lerp(ea.z, eb.z);
      e.vx = lerp(ea.vx, eb.vx);
      e.vy = lerp(ea.vy, eb.vy);
      e.vz = lerp(ea.vz, eb.vz);
      e.orient = eb.orient;  // no slerp at this layer: newest wins
      e.flags = eb.flags;
      e.atRest = eb.atRest;
      e.hasOrient = eb.hasOrient;
      result.entities.push_back(e);
      ++ai;
      ++bi;
    } else if (bid < aid) {
      result.entities.push_back(b.entities[bi]);  // spawned between: hold spawn state
      ++bi;
    } else {
      ++ai;  // despawned between: omitted
    }
  }
  out = result;
}

}  // namespace net
}  // namespace litholattice
