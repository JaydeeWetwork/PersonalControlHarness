// Engine defrag/restructure service: RecordStore + DefragService.
#include "litholattice/defrag/defrag.h"

#include <cstring>
#include <fstream>

#include "litholattice/index/text_indexer.h"
#include "litholattice/storage/file_contract.h"

namespace litholattice::defrag {
namespace {

// ---- little-endian codec (on-disk format is LE, versioned) ----
void putU16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
}
void putU32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void putU64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
bool getU16(const uint8_t*& p, const uint8_t* end, uint16_t& v) {
  if (end - p < 2) return false;
  v = static_cast<uint16_t>(p[0] | (p[1] << 8));
  p += 2;
  return true;
}
bool getU32(const uint8_t*& p, const uint8_t* end, uint32_t& v) {
  if (end - p < 4) return false;
  v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
  p += 4;
  return true;
}
bool getU64(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
  if (end - p < 8) return false;
  v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  p += 8;
  return true;
}

constexpr uint32_t kMagic = 0x47524644;  // "DFRG" little-endian
constexpr uint16_t kFormatVersion = 1;

// Header-level integrity check for recoverCrashedWrite: magic + version +
// sane page count (the first 10 bytes). Full structural validation happens in
// deserialize() when the promoted file is actually loaded.
bool storeImageIntact(const std::filesystem::path& p, std::string* err) {
  std::ifstream f(p, std::ios::binary);
  if (!f) {
    if (err) *err = "cannot open candidate";
    return false;
  }
  uint8_t hdr[10];
  f.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
  if (size_t(f.gcount()) != sizeof(hdr)) {
    if (err) *err = "candidate smaller than the store header";
    return false;
  }
  const uint8_t* q = hdr;
  const uint8_t* end = hdr + sizeof(hdr);
  uint32_t magic = 0;
  uint16_t ver = 0;
  uint32_t pageCount = 0;
  if (!getU32(q, end, magic) || magic != kMagic || !getU16(q, end, ver) ||
      ver != kFormatVersion || !getU32(q, end, pageCount) || pageCount > 1'000'000) {
    if (err) *err = "bad store magic/version/page count";
    return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// RecordStore
// ---------------------------------------------------------------------------
size_t RecordStore::pageUsed(const Page& p) {
  return kPageHeaderSize + p.slots.size() * kSlotSize + p.data.size();
}

bool RecordStore::put(uint64_t id, const uint8_t* data, size_t n) {
  // H-DEF-1: nullptr data is refused at ANY n — the old guard let n==0 through,
  // and the insert below then formed `data + n` on a null pointer (UB, benign
  // in practice). An empty record is put(id, <any valid pointer>, 0).
  if (n > kMaxRecordSize || data == nullptr) return false;
  // Replace tombstones the old record: dead space grows, defrag reclaims it.
  auto it = index_.find(id);
  if (it != index_.end()) {
    Page& op = pages_[it->second.first];
    Slot& os = op.slots[it->second.second];
    if (os.live && os.id == id) {
      os.live = false;
      index_.erase(it);
    }
  }
  const size_t need = kSlotSize + n;
  for (size_t pi = 0; pi < pages_.size(); ++pi) {
    Page& pg = pages_[pi];
    if (kPageSize - pageUsed(pg) >= need) {
      const uint32_t off = static_cast<uint32_t>(pg.data.size());
      pg.slots.push_back(Slot{id, off, static_cast<uint32_t>(n), true});
      pg.data.insert(pg.data.end(), data, data + n);
      index_[id] = {pi, pg.slots.size() - 1};
      ++generation_;
      return true;
    }
  }
  Page pg;
  const uint32_t off = 0;
  pg.slots.push_back(Slot{id, off, static_cast<uint32_t>(n), true});
  pg.data.insert(pg.data.end(), data, data + n);
  index_[id] = {pages_.size(), 0};
  pages_.push_back(std::move(pg));
  ++generation_;
  return true;
}

std::optional<std::vector<uint8_t>> RecordStore::get(uint64_t id) const {
  auto it = index_.find(id);
  if (it == index_.end()) return std::nullopt;
  const Page& pg = pages_[it->second.first];
  const Slot& s = pg.slots[it->second.second];
  if (!s.live || s.id != id) return std::nullopt;  // defensive: index drift
  if (static_cast<size_t>(s.offset) + s.length > pg.data.size()) return std::nullopt;
  return std::vector<uint8_t>(pg.data.begin() + s.offset,
                              pg.data.begin() + s.offset + s.length);
}

bool RecordStore::erase(uint64_t id) {
  auto it = index_.find(id);
  if (it == index_.end()) return false;
  Page& pg = pages_[it->second.first];
  Slot& s = pg.slots[it->second.second];
  if (!s.live || s.id != id) {
    index_.erase(it);  // defensive: drop the stale entry
    return false;
  }
  s.live = false;
  index_.erase(it);
  ++generation_;
  return true;
}

size_t RecordStore::recordCount() const { return index_.size(); }

size_t RecordStore::liveBytes() const {
  size_t n = 0;
  for (const auto& pg : pages_)
    for (const auto& s : pg.slots)
      if (s.live) n += s.length;
  return n;
}

size_t RecordStore::deadBytes() const {
  size_t n = 0;
  for (const auto& pg : pages_)
    for (const auto& s : pg.slots)
      if (!s.live) n += s.length;
  return n;
}

void RecordStore::replaceWith(RecordStore&& compacted) {
  pages_ = std::move(compacted.pages_);
  rebuildIndex();
  // Generation preserved: the content is identical, only the layout changed,
  // so no defrag restart is warranted.
}

void RecordStore::rebuildIndex() {
  index_.clear();
  for (size_t pi = 0; pi < pages_.size(); ++pi) {
    const Page& pg = pages_[pi];
    for (size_t si = 0; si < pg.slots.size(); ++si) {
      const Slot& s = pg.slots[si];
      if (s.live) index_[s.id] = {pi, si};
    }
  }
}

std::vector<uint8_t> RecordStore::serialize() const {
  std::vector<uint8_t> out;
  putU32(out, kMagic);
  putU16(out, kFormatVersion);
  putU32(out, static_cast<uint32_t>(pages_.size()));
  for (const auto& pg : pages_) {
    putU32(out, static_cast<uint32_t>(pg.slots.size()));
    for (const auto& s : pg.slots) {
      putU64(out, s.id);
      putU32(out, s.offset);
      putU32(out, s.length);
      out.push_back(s.live ? 1 : 0);
    }
    putU32(out, static_cast<uint32_t>(pg.data.size()));
    out.insert(out.end(), pg.data.begin(), pg.data.end());
  }
  return out;
}

std::optional<RecordStore> RecordStore::deserialize(const uint8_t* data, size_t n,
                                                    std::string* err) {
  const uint8_t* p = data;
  const uint8_t* end = data + n;
  auto fail = [&](const char* why) -> std::optional<RecordStore> {
    if (err) *err = why;
    return std::nullopt;
  };
  uint32_t magic = 0;
  uint16_t ver = 0;
  uint32_t pageCount = 0;
  if (!getU32(p, end, magic) || magic != kMagic) return fail("bad magic");
  if (!getU16(p, end, ver) || ver != kFormatVersion) return fail("unsupported version");
  if (!getU32(p, end, pageCount)) return fail("truncated header");
  if (pageCount > 1'000'000) return fail("absurd page count");  // fail-closed
  RecordStore store;
  for (uint32_t pi = 0; pi < pageCount; ++pi) {
    uint32_t slotCount = 0;
    if (!getU32(p, end, slotCount)) return fail("truncated page");
    if (slotCount > kPageSize) return fail("absurd slot count");
    Page pg;
    pg.slots.reserve(slotCount);
    for (uint32_t si = 0; si < slotCount; ++si) {
      Slot s;
      uint64_t id = 0;
      uint8_t live = 0;
      if (!getU64(p, end, id) || !getU32(p, end, s.offset) || !getU32(p, end, s.length))
        return fail("truncated slot");
      if (end - p < 1) return fail("truncated slot flags");
      live = *p++;
      s.id = id;
      s.live = (live != 0);
      pg.slots.push_back(s);
    }
    uint32_t dataLen = 0;
    if (!getU32(p, end, dataLen)) return fail("truncated data header");
    if (dataLen > kPageSize) return fail("absurd data length");
    if (static_cast<size_t>(end - p) < dataLen) return fail("truncated data");
    pg.data.insert(pg.data.end(), p, p + dataLen);
    p += dataLen;
    // Validate slot bounds before trusting the page.
    for (const auto& s : pg.slots) {
      if (static_cast<size_t>(s.offset) + s.length > pg.data.size())
        return fail("slot out of bounds");
    }
    store.pages_.push_back(std::move(pg));
  }
  store.rebuildIndex();
  return store;
}

bool RecordStore::save(const std::filesystem::path& path, std::string* err) const {
  const std::vector<uint8_t> bytes = serialize();
  // Crash-safe per the file contract: temp -> fsync -> atomic rename, with
  // the default backup rotation (depth 8). Never defrags in place.
  return storage::writeFileAtomic(path, bytes.data(), bytes.size(), err,
                                  storage::FileWriteOptions{});
}

std::optional<RecordStore> RecordStore::load(const std::filesystem::path& path,
                                             std::string* err) {
  // Crash-window recovery (WORLD PERMANENCE): RecordStore::save uses
  // writeFileAtomic with backup rotation, so the same rotation/commit crash
  // window applies — the target can be absent with the previous generation in
  // .bak.1. Promote the newest intact generation BEFORE reading.
  {
    std::string rerr;
    if (!storage::recoverCrashedWrite(path, storeImageIntact, &rerr)) {
      if (!rerr.empty()) {
        if (err) *err = rerr;  // crash footprint, but no intact generation
        return std::nullopt;
      }
      // No crash footprint at all: genuinely absent, fall through.
    }
  }
  // Capped whole-file read: the old slurp ran before any size check, so a
  // hostile multi-GB file OOMed the loader. readFileAll fails closed past
  // kMaxWholeFileRead (callers with legitimately larger stores pass a cap).
  std::vector<uint8_t> bytes;
  if (!storage::readFileAll(path, &bytes, err)) return std::nullopt;
  return deserialize(bytes.data(), bytes.size(), err);
}

// ---------------------------------------------------------------------------
// TextIndexerReindexer
// ---------------------------------------------------------------------------
void TextIndexerReindexer::reindexBegin(size_t /*liveRecords*/) {
  if (indexer_) indexer_->clear();
}
void TextIndexerReindexer::reindexRecord(uint64_t id, std::span<const uint8_t> bytes) {
  if (!indexer_) return;
  indexer_->addDocument(
      id, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}
void TextIndexerReindexer::reindexEnd() {}

// ---------------------------------------------------------------------------
// DefragService
// ---------------------------------------------------------------------------
bool DefragService::registerStore(const std::string& name, RecordStore& store,
                                  const std::filesystem::path& filePath,
                                  IReindexer* reindexerOrNull) {
  if (name.empty() || stores_.count(name)) return false;
  stores_[name] = Entry{&store, filePath, reindexerOrNull};
  return true;
}

bool DefragService::unregisterStore(const std::string& name) {
  if (sessionStore_ == name && running()) return false;  // fail-closed: busy
  return stores_.erase(name) > 0;
}

std::vector<std::string> DefragService::storeNames() const {
  std::vector<std::string> names;
  for (const auto& [n, e] : stores_) names.push_back(n);
  return names;
}

bool DefragService::beginDefrag(const std::string& storeName) {
  if (running()) {
    lastError_ = "a defrag is already running";
    return false;
  }
  auto it = stores_.find(storeName);
  if (it == stores_.end()) {
    lastError_ = "unknown store '" + storeName + "'";
    return false;
  }
  resetSession();
  sessionStore_ = storeName;
  sessionEntry_ = it->second;
  startGeneration_ = sessionEntry_.store->generation();
  phase_ = Phase::Scan;
  return true;
}

void DefragService::cancel() {
  if (!running()) return;
  // A cancel at-or-after reindexBegin() leaves the index cleared-but-partial.
  // If the store did not mutate since the scan, finish feeding the snapshot
  // synchronously: the index ends up complete and consistent with the store,
  // and no dirty flag is needed. If it did mutate, the snapshot is stale —
  // feeding it would bake the divergence in permanently — so mark the index
  // dirty instead; the next session's Reindex phase re-feeds it fully.
  if (reindexBegun_ && sessionEntry_.reindexer) {
    if (sessionEntry_.store->generation() == startGeneration_) {
      IReindexer* rx = sessionEntry_.reindexer;
      while (reindexIdx_ < live_.size()) {
        const auto& [id, bytes] = live_[reindexIdx_];
        rx->reindexRecord(id, std::span<const uint8_t>(bytes.data(), bytes.size()));
        ++reindexIdx_;
      }
      rx->reindexEnd();
      // Re-check: the store may have mutated during the synchronous feed
      // itself. If it did, the snapshot is stale — mark dirty so the next
      // session's Reindex phase re-feeds it fully.
      if (sessionEntry_.store->generation() == startGeneration_) {
        // Fix 2026-10-11 (regression): only heal the dirty flag if it belongs
        // to THIS store. indexDirty_ is per-service but stores are per-map; an
        // unconditional clear could drop a different store's dirty record.
        // Mirrors the store guard in pumpReindex's clear path.
        if (indexDirty_ && indexDirtyStore_ == sessionStore_) {
          indexDirty_ = false;
          indexDirtyStore_.clear();
        }
      } else {
        indexDirty_ = true;
        indexDirtyStore_ = sessionStore_;
      }
    } else {
      indexDirty_ = true;
      indexDirtyStore_ = sessionStore_;
    }
  }
  resetSession();
  phase_ = Phase::Idle;
}

void DefragService::resetSession() {
  phase_ = Phase::Idle;
  sessionStore_.clear();
  sessionEntry_ = Entry{};
  startGeneration_ = 0;
  restarts_ = 0;
  live_.clear();
  scanPage_ = 0;
  copyIdx_ = 0;
  reindexIdx_ = 0;
  reindexBegun_ = false;
  compacted_ = RecordStore();
}

void DefragService::fail(const std::string& why) {
  lastError_ = why;
  // The live file was never touched (commit is atomic): the store object and
  // its file are exactly as before the session. Safe to idle out.
  // If the reindexer already cleared the index, it is now partial: never feed
  // the (possibly stale) snapshot — mark it dirty so the next session's
  // Reindex phase re-feeds it fully.
  if (reindexBegun_) {
    indexDirty_ = true;
    indexDirtyStore_ = sessionStore_;
  }
  resetSession();
  phase_ = Phase::Failed;
  lastError_ = why;
}

const char* DefragService::phaseName(Phase p) {
  switch (p) {
    case Phase::Idle: return "idle";
    case Phase::Scan: return "scan";
    case Phase::Copy: return "copy";
    case Phase::Commit: return "commit";
    case Phase::Reindex: return "reindex";
    case Phase::Done: return "done";
    case Phase::Failed: return "failed";
  }
  return "?";
}

void DefragService::pump() {
  switch (phase_) {
    case Phase::Idle:
    case Phase::Done:
    case Phase::Failed:
      return;
    case Phase::Scan: pumpScan(); break;
    case Phase::Copy: pumpCopy(); break;
    case Phase::Commit: pumpCommit(); break;
    case Phase::Reindex: pumpReindex(); break;
  }
}

void DefragService::pumpScan() {
  RecordStore* store = sessionEntry_.store;
  for (size_t i = 0; i < kScanPagesPerTick; ++i) {
    if (scanPage_ >= store->pageCount()) {
      phase_ = Phase::Copy;
      copyIdx_ = 0;
      compacted_ = RecordStore();
      return;
    }
    store->forEachLiveInPage(scanPage_, [this](uint64_t id, std::span<const uint8_t> b) {
      live_.emplace_back(id, std::vector<uint8_t>(b.begin(), b.end()));
    });
    ++scanPage_;
  }
}

void DefragService::pumpCopy() {
  for (size_t i = 0; i < kCopyRecordsPerTick; ++i) {
    if (copyIdx_ >= live_.size()) {
      phase_ = Phase::Commit;
      return;
    }
    const auto& [id, bytes] = live_[copyIdx_];
    // Cannot fail: every record came from the store, so it fits a page.
    if (!compacted_.put(id, bytes.data(), bytes.size())) {
      fail("internal error: compacted put failed");
      return;
    }
    ++copyIdx_;
  }
}

void DefragService::restartScan() {
  ++restarts_;
  live_.clear();
  scanPage_ = 0;
  copyIdx_ = 0;
  reindexIdx_ = 0;
  reindexBegun_ = false;
  compacted_ = RecordStore();
  startGeneration_ = sessionEntry_.store->generation();
  phase_ = Phase::Scan;
}

void DefragService::pumpCommit() {
  RecordStore* store = sessionEntry_.store;
  // The store mutated mid-defrag: restart the scan, bounded. Past the bound
  // the session fails closed ("store too busy") instead of livelocking.
  if (store->generation() != startGeneration_) {
    if (restarts_ >= kMaxRestarts) {
      fail("store '" + sessionStore_ + "' mutated during defrag too many times; " +
           "aborting (store too busy)");
      return;
    }
    restartScan();
    return;
  }
  const std::vector<uint8_t> image = compacted_.serialize();
  std::string err;
  // Crash-safe: temp -> fsync -> atomic rename. The live file is untouched
  // until the rename lands; a stale .tmp is a crash footprint, never read.
  if (!storage::writeFileAtomic(sessionEntry_.filePath, image.data(), image.size(),
                                &err, storage::FileWriteOptions{})) {
    fail("commit failed: " + err);
    return;
  }
  store->replaceWith(std::move(compacted_));
  compacted_ = RecordStore();
  reindexIdx_ = 0;
  reindexBegun_ = false;
  phase_ = sessionEntry_.reindexer ? Phase::Reindex : Phase::Done;
}

void DefragService::pumpReindex() {
  IReindexer* rx = sessionEntry_.reindexer;
  if (!rx) {
    phase_ = Phase::Done;
    return;
  }
  // A store mutation during Reindex would feed stale snapshot bytes into the
  // freshly cleared index: permanent index/store divergence. Re-check the
  // generation every pump and restart the scan on mismatch (bounded, like
  // the Commit phase).
  if (sessionEntry_.store->generation() != startGeneration_) {
    if (restarts_ >= kMaxRestarts) {
      fail("store '" + sessionStore_ + "' mutated during defrag too many times; " +
           "aborting (store too busy)");
      return;
    }
    restartScan();
    return;
  }
  if (!reindexBegun_) {
    rx->reindexBegin(live_.size());
    reindexBegun_ = true;
  }
  for (size_t i = 0; i < kReindexRecordsPerTick; ++i) {
    if (reindexIdx_ >= live_.size()) {
      rx->reindexEnd();
      // A full reindex just completed: heal the dirty flag for this store.
      if (indexDirty_ && indexDirtyStore_ == sessionStore_) {
        indexDirty_ = false;
        indexDirtyStore_.clear();
      }
      phase_ = Phase::Done;
      return;
    }
    const auto& [id, bytes] = live_[reindexIdx_];
    rx->reindexRecord(id, std::span<const uint8_t>(bytes.data(), bytes.size()));
    ++reindexIdx_;
  }
}

double DefragService::progress() const {
  switch (phase_) {
    case Phase::Idle:
    case Phase::Failed: return 0.0;
    case Phase::Done: return 1.0;
    case Phase::Scan: {
      const size_t pages = sessionEntry_.store ? sessionEntry_.store->pageCount() : 0;
      if (pages == 0) return 0.0;
      return 0.3 * (static_cast<double>(std::min(scanPage_, pages)) / pages);
    }
    case Phase::Copy: {
      if (live_.empty()) return 0.3;
      return 0.3 + 0.4 * (static_cast<double>(std::min(copyIdx_, live_.size())) /
                           live_.size());
    }
    case Phase::Commit: return 0.7;
    case Phase::Reindex: {
      if (live_.empty()) return 0.7;
      return 0.7 + 0.3 * (static_cast<double>(std::min(reindexIdx_, live_.size())) /
                           live_.size());
    }
  }
  return 0.0;
}

std::string DefragService::statusLine() const {
  char buf[256];
  const int pct = static_cast<int>(progress() * 100.0);
  switch (phase_) {
    case Phase::Idle: return "defrag: idle";
    case Phase::Done: return "defrag '" + sessionStore_ + "': complete";
    case Phase::Failed: return "defrag '" + sessionStore_ + "': FAILED: " + lastError_;
    case Phase::Scan: {
      const size_t pages = sessionEntry_.store ? sessionEntry_.store->pageCount() : 0;
      std::snprintf(buf, sizeof(buf), "defrag '%s': scan %zu/%zu pages (%d%%)",
                    sessionStore_.c_str(), std::min(scanPage_, pages), pages, pct);
      return buf;
    }
    case Phase::Copy:
      std::snprintf(buf, sizeof(buf), "defrag '%s': copy %zu/%zu records (%d%%)",
                    sessionStore_.c_str(), std::min(copyIdx_, live_.size()),
                    live_.size(), pct);
      return buf;
    case Phase::Commit:
      std::snprintf(buf, sizeof(buf), "defrag '%s': commit (%d%%)", sessionStore_.c_str(),
                    pct);
      return buf;
    case Phase::Reindex:
      std::snprintf(buf, sizeof(buf), "defrag '%s': reindex %zu/%zu records (%d%%)",
                    sessionStore_.c_str(), std::min(reindexIdx_, live_.size()),
                    live_.size(), pct);
      return buf;
  }
  return "defrag: ?";
}

}  // namespace litholattice::defrag
