#include "litholattice/storage/storage.h"
#include "litholattice/storage/file_contract.h"
#include "litholattice/core/job_system.h"
#include "litholattice/core/log.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>

namespace litholattice::storage {

uint32_t crc32(const uint8_t* d, size_t n) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

namespace {
// Header-level integrity check for recoverCrashedWrite: magic + version +
// every index entry in-bounds. (Per-record CRCs are still checked on read().)
bool regionImageIntact(const std::filesystem::path& p, std::string* err) {
  std::ifstream f(p, std::ios::binary);
  if (!f) {
    if (err) *err = "cannot open candidate";
    return false;
  }
  // Header + full index: 8 magic/version bytes + 1024 x 16-byte entries.
  constexpr size_t kHead = 8 + size_t(RegionFile::kSpan) * RegionFile::kSpan * 16;
  Bytes head(kHead);
  f.read(reinterpret_cast<char*>(head.data()), head.size());
  if (size_t(f.gcount()) < 8) {
    if (err) *err = "candidate smaller than the region header";
    return false;
  }
  head.resize(size_t(f.gcount()));
  ByteReader r(head.data(), head.size());
  if (r.u32() != RegionFile::kMagic || r.u16() != RegionFile::kVersion || (r.u16(), !r.ok())) {
    if (err) *err = "bad region magic/version";
    return false;
  }
  // File size for the index bounds check (entries may point past the header
  // read when the region holds data).
  f.clear();
  f.seekg(0, std::ios::end);
  const uint64_t fileSize = uint64_t(f.tellg());
  for (int i = 0; i < RegionFile::kSpan * RegionFile::kSpan; ++i) {
    const uint64_t off = r.u64();
    const uint32_t size = r.u32();
    r.u32();  // crc
    if (!r.ok()) {
      if (err) *err = "truncated region index";
      return false;
    }
    if (size != 0 && (off > fileSize || size > fileSize - off)) {
      if (err) *err = "region index out of range";
      return false;
    }
  }
  return true;
}
}

// ---- palette codec
void encodePalette(const uint16_t* v, ByteWriter& w) {
  std::vector<uint16_t> pal;
  std::unordered_map<uint16_t, uint16_t> idx;
  for (int i = 0; i < kSubChunkVolume; ++i)
    if (idx.emplace(v[i], uint16_t(pal.size())).second) pal.push_back(v[i]);
  w.u16(uint16_t(pal.size()));
  for (uint16_t p : pal) w.u16(p);
  int bits = 0;
  while ((size_t(1) << bits) < pal.size()) ++bits;
  if (!bits) return;
  const size_t words = (size_t(kSubChunkVolume) * size_t(bits) + 63) / 64;
  std::vector<uint64_t> packed(words, 0);
  for (int i = 0; i < kSubChunkVolume; ++i) {
    const uint64_t x = idx[v[i]];
    const size_t bit = size_t(i) * size_t(bits), wi = bit / 64, sh = bit % 64;
    packed[wi] |= x << sh;
    if (sh + size_t(bits) > 64) packed[wi + 1] |= x >> (64 - sh);
  }
  for (uint64_t p : packed) w.u64(p);
}

bool decodePalette(ByteReader& r, uint16_t* v) {
  const uint16_t n = r.u16();
  if (!r.ok() || n == 0 || n > kSubChunkVolume) return false;
  std::vector<uint16_t> pal(n);
  for (auto& p : pal) p = r.u16();
  int bits = 0;
  while ((size_t(1) << bits) < pal.size()) ++bits;
  if (!bits) { std::fill(v, v + kSubChunkVolume, pal[0]); return r.ok(); }
  const size_t words = (size_t(kSubChunkVolume) * size_t(bits) + 63) / 64;
  std::vector<uint64_t> packed(words);
  for (auto& p : packed) p = r.u64();
  if (!r.ok()) return false;
  const uint64_t mask = (uint64_t(1) << bits) - 1;
  for (int i = 0; i < kSubChunkVolume; ++i) {
    const size_t bit = size_t(i) * size_t(bits), wi = bit / 64, sh = bit % 64;
    uint64_t x = packed[wi] >> sh;
    if (sh + size_t(bits) > 64) x |= packed[wi + 1] << (64 - sh);
    x &= mask;
    if (x >= n) return false;
    v[i] = pal[size_t(x)];
  }
  return true;
}

// ---- payload registry
PayloadRegistry::PayloadRegistry() {
  PayloadType blocks;
  blocks.tag = kBlocksTag;
  blocks.version = kBlocksVersion;
  // The on-disk BLKS codec is UNCHANGED (bit-packed uint16 BlockIds): the StableId
  // voxel storage is translated through the registry at save/load. registry_ is
  // installed once at startup and only read afterwards (worker-thread safe).
  blocks.save = [this](const ChunkColumn& c) {
    const BlockRegistry* reg = registry_;
    if (!reg) {
      // C-L4 (P3-2): with NO registry installed, a 64-bit-width palette's raw
      // index IS the StableId (up to 2^64 distinct values) — the old fallback
      // below silently truncated it to u16, writing a DIFFERENT block than
      // the world holds. Fail loud instead: log + an empty payload, which
      // decode() then rejects as corrupt rather than misreading. Narrower
      // widths are safe: at most kSubChunkVolume distinct cells exist, so
      // their raw indices always fit u16.
      for (int s = 0; s < kSubChunks; ++s)
        if (c.subs[size_t(s)] && c.subs[size_t(s)]->nonAir &&
            c.subs[size_t(s)]->blocks.width() == VoxelPalette::Width::W64) {
          logError("storage: BLKS save refused for column %d,%d: 64-bit palette with no block registry "
                   "installed (StableIds cannot be truncated to u16)", c.cx, c.cz);
          return Bytes{};
        }
    }
    ByteWriter w;
    uint64_t present = 0;
    for (int s = 0; s < kSubChunks; ++s) if (c.subs[size_t(s)] && c.subs[size_t(s)]->nonAir) present |= uint64_t(1) << s;
    w.u64(present);
    // Scratch reused across sub-chunks; the disk codec itself is unchanged.
    std::array<uint16_t, kSubChunkVolume> tmp{};
    std::vector<BlockId> lut;
    for (int s = 0; s < kSubChunks; ++s)
      if (present >> s & 1) {
        const SubChunk* sub = c.subs[size_t(s)].get();
        if (reg) {
          // Resolve the StableId palette to BlockIds once; voxels are O(1) below.
          const bool w64 = sub->blocks.width() == VoxelPalette::Width::W64;
          if (!w64) sub->blocks.resolveBlockIds(*reg, lut);
          for (int i = 0; i < kSubChunkVolume; ++i) {
            if (w64) {  // degenerate width (unreachable in practice)
              const auto id = reg->findByStableId(sub->blocks.at(size_t(i)));
              tmp[size_t(i)] = id ? *id : kAir;
            } else {
              tmp[size_t(i)] = lut[sub->blocks.rawIndex(size_t(i))];
            }
          }
        } else {
          // No registry installed (tests/tools): the palette was built from raw
          // ids directly, so the raw indices ARE the ids.
          for (int i = 0; i < kSubChunkVolume; ++i) tmp[size_t(i)] = uint16_t(sub->blocks.rawIndex(size_t(i)));
        }
        encodePalette(tmp.data(), w);
        encodePalette(sub->state.data(), w);
      }
    return w.out;
  };
  blocks.load = [](const Bytes& b, ChunkColumn& c) {
    ByteReader r(b.data(), b.size());
    const uint64_t present = r.u64();
    if (!r.ok() || (present >> kSubChunks)) return false;
    for (int s = 0; s < kSubChunks; ++s) {
      if (!(present >> s & 1)) continue;
      auto sub = std::make_unique<SubChunk>();
      // Decode into the load scratch (save-time BlockIds); remapColumn() consumes
      // it into the StableId paletted storage. The disk codec is unchanged.
      auto scratch = std::vector<uint16_t>(kSubChunkVolume);
      if (!decodePalette(r, scratch.data()) || !decodePalette(r, sub->state.data())) return false;
      sub->nonAir = uint16_t(std::count_if(scratch.begin(), scratch.end(),
                                           [](uint16_t x) { return x != kAir; }));
      sub->loadScratch = std::move(scratch);
      c.subs[size_t(s)] = std::move(sub);
    }
    return r.done();
  };
  add(std::move(blocks));
}

Bytes PayloadRegistry::encode(const ChunkColumn& c) const {
  ByteWriter w;
  w.u16(kRecordVersion);
  w.u32(uint32_t(c.cx)); w.u32(uint32_t(c.cz));
  w.u16(uint16_t(types_.size()));
  for (const auto& [tag, t] : types_) { w.u32(tag); w.u16(t.version); w.bytes(t.save(c)); }
  return w.out;
}

std::unique_ptr<ChunkColumn> PayloadRegistry::decode(const Bytes& rec, std::string* err) const {
  auto fail = [&](std::string m) { if (err) *err = std::move(m); return nullptr; };
  ByteReader r(rec.data(), rec.size());
  const uint16_t rv = r.u16();
  if (!r.ok() || rv == 0 || rv > kRecordVersion) return fail("unsupported chunk record version " + std::to_string(rv));
  auto c = std::make_unique<ChunkColumn>();
  c->cx = int(int32_t(r.u32())); c->cz = int(int32_t(r.u32()));
  const uint16_t n = r.u16();
  for (uint16_t i = 0; i < n && r.ok(); ++i) {
    const uint32_t tag = r.u32();
    uint16_t ver = r.u16();
    Bytes data = r.bytes();
    if (!r.ok()) break;
    const PayloadType* t = find(tag);
    if (!t) continue;  // a payload nobody registers any more: dropped
    if (ver > t->version) return fail("payload newer than this build (tag " + std::to_string(tag) + ")");
    while (ver < t->version) {
      auto m = t->migrate.find(ver);
      if (m == t->migrate.end()) return fail("no migration from payload version " + std::to_string(ver));
      auto up = m->second(data);
      if (!up) return fail("migration from payload version " + std::to_string(ver) + " failed");
      data = std::move(*up); ++ver;
    }
    if (!t->load(data, *c)) return fail("corrupt payload (tag " + std::to_string(tag) + ")");
  }
  if (!r.ok() || !r.done()) return fail("truncated chunk record");
  return c;
}

// ---- region file
bool RegionFile::open(std::string* err) {
  // Crash-window recovery (WORLD PERMANENCE): a crash between backup rotation
  // and the commit rename leaves the target absent with the previous generation
  // safe in <path>.bak.1. Promote the newest intact generation BEFORE reading,
  // so a missing file here really means "new region", not "lost region".
  {
    std::string rerr;
    if (!recoverCrashedWrite(path_, regionImageIntact, &rerr)) {
      if (!rerr.empty()) {
        broken_ = true;  // crash footprint, but no intact generation: fail closed
        if (err) *err = rerr;
        return false;
      }
      // No crash footprint at all: genuinely absent, fall through to new-region.
    }
  }
  std::ifstream f(path_, std::ios::binary);
  if (!f) return true;  // new region
  // C-M3 (P3-2): stat BEFORE the whole-file read and refuse above the cap —
  // the old open() slurped an unbounded file into memory, so a corrupt or
  // hostile multi-GB .llr OOMed the loader instead of failing closed.
  {
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(path_, ec);
    if (!ec && size > kMaxFileBytes) {
      broken_ = true;
      if (err) *err = "region file exceeds the " + std::to_string(kMaxFileBytes) + "-byte cap (" +
                      std::to_string(size) + " bytes): " + path_.string();
      return false;
    }
  }
  const Bytes all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  ByteReader r(all.data(), all.size());
  if (r.u32() != kMagic || r.u16() != kVersion || (r.u16(), !r.ok())) { broken_ = true; if (err) *err = "bad region header: " + path_.string(); return false; }
  struct E { uint64_t off; uint32_t size, crc; };
  std::vector<E> idx(size_t(kSpan * kSpan));
  for (auto& e : idx) { e.off = r.u64(); e.size = r.u32(); e.crc = r.u32(); }
  if (!r.ok()) { broken_ = true; if (err) *err = "truncated region index: " + path_.string(); return false; }
  for (size_t i = 0; i < idx.size(); ++i) {
    if (!idx[i].size) continue;
    if (idx[i].off > all.size() || idx[i].size > all.size() - idx[i].off) { broken_ = true; if (err) *err = "region index out of range"; return false; }
    records_[i].assign(all.begin() + std::ptrdiff_t(idx[i].off), all.begin() + std::ptrdiff_t(idx[i].off + idx[i].size));
    crcs_[i] = idx[i].crc;
  }
  return true;
}

std::optional<Bytes> RegionFile::read(int lx, int lz, std::string* err) const {
  if (lx < 0 || lx >= kSpan || lz < 0 || lz >= kSpan) {
    if (err) *err = "chunk coordinates out of range";
    return std::nullopt;  // fail closed: never index the vectors out of bounds
  }
  const int s = slot(lx, lz);
  if (auto it = pending_.find(s); it != pending_.end()) return it->second;
  const Bytes& b = records_[size_t(s)];
  if (b.empty()) return std::nullopt;
  if (crc32(b.data(), b.size()) != crcs_[size_t(s)]) { if (err) *err = "chunk record CRC mismatch"; return std::nullopt; }
  return b;
}

size_t RegionFile::count() const {
  size_t n = 0;
  for (size_t i = 0; i < records_.size(); ++i) n += !records_[i].empty() || pending_.count(int(i));
  return n;
}

bool RegionFile::flush(std::string* err) { return flush(err, FileWriteOptions{}); }

bool RegionFile::flush(std::string* err, const FileWriteOptions& opts) {
  if (pending_.empty()) return true;
  if (broken_) { if (err) *err = "region was unreadable; not overwriting " + path_.string(); return false; }
  // SV-H1: build the merged image in LOCALS and only commit it into
  // records_/crcs_ (and clear pending_) after the write succeeds. The old
  // code merged + cleared pending_ BEFORE writeFileAtomic, so a failed write
  // (disk full, I/O error) was unretryable: later flushes early-returned
  // true on an empty pending_ and the loss was masked by in-memory reads.
  // read() consults pending_ first, so the uncommitted data stays visible
  // (and retryable) for exactly as long as it is not yet on disk.
  std::vector<Bytes> merged = records_;
  std::vector<uint32_t> mergedCrcs = crcs_;
  for (auto& [s, b] : pending_) { mergedCrcs[size_t(s)] = crc32(b.data(), b.size()); merged[size_t(s)] = b; }
  ByteWriter w;
  w.u32(kMagic); w.u16(kVersion); w.u16(0);
  uint64_t off = 8 + uint64_t(kSpan * kSpan) * 16;
  for (size_t i = 0; i < merged.size(); ++i) {
    w.u64(merged[i].empty() ? 0 : off); w.u32(uint32_t(merged[i].size())); w.u32(mergedCrcs[i]);
    off += merged[i].size();
  }
  for (const Bytes& b : merged) w.out.insert(w.out.end(), b.begin(), b.end());
  // The engine file contract: temp + fsync + atomic rename, with the default
  // backup depth (kBackupDepthDefault last-known-good versions) rotated on
  // success. A crash leaves the old region (or a backup), never a torn one;
  // the rotation/commit crash window self-heals via recoverCrashedWrite on open.
  if (!writeFileAtomic(path_, w.out, err, opts)) return false;  // pending_ kept: a later flush retries
  records_ = std::move(merged);
  crcs_ = std::move(mergedCrcs);
  pending_.clear();
  return true;
}

// ---- world store
namespace {
int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
}  // namespace

WorldStore::WorldStore(std::filesystem::path dir, const PayloadRegistry& payloads, JobSystem* jobs)
    : dir_(std::move(dir)), payloads_(payloads), jobsys_(jobs) {
  std::filesystem::create_directories(dir_);
  if (!jobsys_) thread_ = std::thread([this] { run(); });
}

WorldStore::~WorldStore() {
  flush();
  if (jobsys_) return;  // the engine owns the JobSystem: never stop or join it here
  { std::lock_guard l(m_); stop_ = true; }
  cv_.notify_all();
  thread_.join();
}

std::filesystem::path WorldStore::regionPath(const std::filesystem::path& dir, int rx, int rz) {
  return dir / ("r." + std::to_string(rx) + "." + std::to_string(rz) + ".llr");
}

RegionFile& WorldStore::region(int rx, int rz) {
  auto& r = regions_[{rx, rz}];
  if (!r) {
    r = std::make_unique<RegionFile>(regionPath(dir_, rx, rz));
    std::string err;
    if (!r->open(&err)) {
      ++quarantined_;
      logError("storage: quarantined region r.%d.%d.llr (%s); continuing with an empty region",
               rx, rz, err.c_str());
    }
  }
  return *r;
}

void WorldStore::post(std::function<void()> job) {
  bool kick = false;
  {
    std::lock_guard l(m_);
    jobs_.push_back(std::move(job));
    if (jobsys_ && !pumping_) { pumping_ = true; kick = true; }
  }
  if (jobsys_) {
    if (kick) pumpOne();
  } else {
    cv_.notify_one();
  }
}

// JobSystem mode: run one queued store job per submitIo(), tail-chained. Strict
// FIFO is preserved (the queue is the order), no I/O thread ever blocks waiting
// for more work, and other I/O can interleave between chained jobs.
void WorldStore::pumpOne() {
  jobsys_->submitIo([this] {
    std::function<void()> job;
    {
      std::lock_guard l(m_);
      if (jobs_.empty()) { pumping_ = false; idle_.notify_all(); return; }
      job = std::move(jobs_.front());
      jobs_.pop_front();
      busy_ = true;
    }
    try {
      job();
    } catch (const std::exception& e) {
      logError("storage: queued job threw: %s", e.what());
    } catch (...) {
      logError("storage: queued job threw a non-std exception");
    }
    {
      std::lock_guard l(m_);
      busy_ = false;
      if (jobs_.empty()) { pumping_ = false; idle_.notify_all(); return; }
    }
    idle_.notify_all();
    pumpOne();  // tail-chain: the next store job goes through submitIo() again
  });
}

void WorldStore::run() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock l(m_);
      cv_.wait(l, [&] { return stop_ || !jobs_.empty(); });
      if (jobs_.empty()) return;
      job = std::move(jobs_.front()); jobs_.pop_front(); busy_ = true;
    }
    job();
    { std::lock_guard l(m_); busy_ = false; }
    idle_.notify_all();
  }
}

void WorldStore::saveAsync(const ChunkColumn& c) {
  post([this, cx = c.cx, cz = c.cz, rec = payloads_.encode(c)]() mutable {
    const int rx = floorDiv(cx, RegionFile::kSpan), rz = floorDiv(cz, RegionFile::kSpan);
    region(rx, rz).put(cx - rx * RegionFile::kSpan, cz - rz * RegionFile::kSpan, std::move(rec));
  });
}

void WorldStore::saveAsync(std::unique_ptr<ChunkColumn> c) {
  post([this, c = std::shared_ptr<ChunkColumn>(std::move(c))] {
    const int rx = floorDiv(c->cx, RegionFile::kSpan), rz = floorDiv(c->cz, RegionFile::kSpan);
    region(rx, rz).put(c->cx - rx * RegionFile::kSpan, c->cz - rz * RegionFile::kSpan, payloads_.encode(*c));
  });
}

void WorldStore::loadAsync(int cx, int cz, std::function<void(std::unique_ptr<ChunkColumn>, std::string)> done) {
  post([this, cx, cz, done = std::move(done)] {
    const int rx = floorDiv(cx, RegionFile::kSpan), rz = floorDiv(cz, RegionFile::kSpan);
    std::string err;
    auto rec = region(rx, rz).read(cx - rx * RegionFile::kSpan, cz - rz * RegionFile::kSpan, &err);
    std::unique_ptr<ChunkColumn> c = rec ? payloads_.decode(*rec, &err) : nullptr;
    done(std::move(c), std::move(err));
  });
}

bool WorldStore::flush() {
  flushAsync();
  std::unique_lock l(m_);
  idle_.wait(l, [&] { return jobs_.empty() && !busy_; });
  return lastFlushOk();
}

void WorldStore::flushAsync() {
  post([this] {
    // Batch durability: every region skips its own directory fsync and the
    // directory is fsync'd once after all renames land (one dir fsync per
    // save instead of one per region). A crash before the batch fsync can
    // only revert a rename (old version visible), never tear a file.
    FileWriteOptions opts;
    opts.deferDirFsync = true;
    int failures = 0;
    for (auto& [k, r] : regions_) {
      std::string err;
      if (!r->flush(&err, opts)) {
        ++failures;  // SV-H1: count it and publish it (see lastFlushOk());
                     // the region kept its pending records for the retry.
        std::fprintf(stderr, "storage: %s\n", err.c_str());
        logError("storage: region flush failed (kept pending for retry): %s", err.c_str());
      }
    }
    lastFlushFailures_.store(failures, std::memory_order_relaxed);
    lastFlushOk_.store(failures == 0, std::memory_order_relaxed);
    syncDirContaining(dir_);
  });
}

std::unique_ptr<ChunkColumn> WorldStore::load(int cx, int cz, std::string* err) {
  std::unique_ptr<ChunkColumn> out;
  std::string e;
  loadAsync(cx, cz, [&](std::unique_ptr<ChunkColumn> c, std::string er) { out = std::move(c); e = std::move(er); });
  { std::unique_lock l(m_); idle_.wait(l, [&] { return jobs_.empty() && !busy_; }); }
  if (err) *err = e;
  return out;
}

}  // namespace litholattice::storage
