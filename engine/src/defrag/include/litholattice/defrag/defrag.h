#pragma once
// Engine defrag/restructure service (JD 2026-10-11).
//
// A general engine service — not Fragment-specific — that compacts, reorders,
// and reclaims dead space for large persistent stores. The Fragment is the
// first client (a forever-world Fragment grows enormous and fragmented), but
// any paged record store can register.
//
// LAYOUT ASSUMPTION (for the builder): the Fragment's persistence is assumed
// to be a RecordStore below — slotted 4 KiB pages, tombstoned deletes,
// generation counter. If the real Fragment persistence evolves, it must either
// adopt this layout or provide its own defrag target behind the same
// DefragService session protocol (begin/pump/cancel + reindex hook).
//
// Crash safety: the compacted image is written with storage::writeFileAtomic
// (temp -> fsync -> atomic rename). The live file is never touched until the
// atomic commit; a stale .tmp is a crash footprint per the file contract.
// Defrag is chunked across ticks: pump() does bounded work per call, so a
// ten-year Fragment never stalls the sim.
//
// Threading: NOT thread-safe. The owner (the tick pump) must serialize access,
// same rule as index::TextIndexer.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace litholattice::index {
class TextIndexer;  // engine text indexer (the reindex target)
}  // namespace litholattice::index

namespace litholattice::defrag {

// ---------------------------------------------------------------------------
// RecordStore: a paged, slotted record store with tombstones.
// ---------------------------------------------------------------------------
class RecordStore {
 public:
  // Powers of two throughout (project motif). A record must fit one page
  // with its slot: fail-closed on oversize puts.
  static constexpr size_t kPageSize = 4096;
  static constexpr size_t kPageHeaderSize = 32;
  static constexpr size_t kSlotSize = 24;
  static constexpr size_t kMaxRecordSize = kPageSize - kPageHeaderSize - kSlotSize;

  RecordStore() = default;

  // Insert or replace. Replace tombstones the old record (dead space grows;
  // that is what defrag reclaims). False + no mutation when oversize.
  bool put(uint64_t id, const uint8_t* data, size_t n);
  bool put(uint64_t id, std::span<const uint8_t> bytes) {
    return put(id, bytes.data(), bytes.size());
  }
  bool put(uint64_t id, std::string_view text) {
    return put(id, reinterpret_cast<const uint8_t*>(text.data()), text.size());
  }

  std::optional<std::vector<uint8_t>> get(uint64_t id) const;
  // Tombstones the record (dead space). False when the id is not live.
  bool erase(uint64_t id);

  // Bumped on every mutation (put/erase/replaceWith). The defrag service
  // snapshots it at begin; a change mid-defrag forces a bounded restart.
  uint64_t generation() const { return generation_; }

  size_t pageCount() const { return pages_.size(); }
  size_t recordCount() const;  // live records
  size_t liveBytes() const;
  size_t deadBytes() const;  // tombstoned bytes awaiting reclaim

  // Crash-safe persistence (storage::writeFileAtomic: temp -> fsync -> rename).
  bool save(const std::filesystem::path& path, std::string* err) const;
  static std::optional<RecordStore> load(const std::filesystem::path& path,
                                         std::string* err);

  // Chunkable live-record iteration for the defrag scan phase.
  // Templated on the callback (not std::function): the scan feeds hundreds of
  // records per tick on the tick thread, and must not heap-allocate per page
  // per tick.
  template <typename Fn>
  void forEachLiveInPage(size_t pageIdx, Fn&& fn) const {
    if (pageIdx >= pages_.size()) return;
    const Page& pg = pages_[pageIdx];
    for (const auto& s : pg.slots) {
      if (!s.live) continue;
      if (static_cast<size_t>(s.offset) + s.length > pg.data.size()) continue;
      fn(s.id, std::span<const uint8_t>(pg.data.data() + s.offset, s.length));
    }
  }

  // Defrag commit: replace this store's pages with the compacted image.
  // Content-identical; the generation is preserved (no spurious restart).
  void replaceWith(RecordStore&& compacted);

  // The defrag service serializes the compacted image for the atomic commit.
  friend class DefragService;

 private:
  struct Slot {
    uint64_t id = 0;
    uint32_t offset = 0;  // into the page's data buffer
    uint32_t length = 0;
    bool live = false;
  };
  struct Page {
    std::vector<Slot> slots;
    std::vector<uint8_t> data;  // record bytes, appended
  };
  static size_t pageUsed(const Page& p);

  // id -> (page index, slot index), live records only.
  std::unordered_map<uint64_t, std::pair<size_t, size_t>> index_;
  std::vector<Page> pages_;
  uint64_t generation_ = 0;

  void rebuildIndex();
  std::vector<uint8_t> serialize() const;
  static std::optional<RecordStore> deserialize(const uint8_t* data, size_t n,
                                                std::string* err);
};

// ---------------------------------------------------------------------------
// IReindexer: the reindex hook. Defrag drives it; the indexer never knows.
// ---------------------------------------------------------------------------
class IReindexer {
 public:
  virtual ~IReindexer() = default;
  virtual void reindexBegin(size_t liveRecords) = 0;
  virtual void reindexRecord(uint64_t id, std::span<const uint8_t> bytes) = 0;
  virtual void reindexEnd() = 0;
};

// Adapts the engine TextIndexer: record bytes are interpreted as UTF-8 text
// and re-fed as documents keyed by record id. Keeps index/ untouched.
class TextIndexerReindexer : public IReindexer {
 public:
  explicit TextIndexerReindexer(::litholattice::index::TextIndexer* indexer)
      : indexer_(indexer) {}
  void reindexBegin(size_t liveRecords) override;
  void reindexRecord(uint64_t id, std::span<const uint8_t> bytes) override;
  void reindexEnd() override;

 private:
  ::litholattice::index::TextIndexer* indexer_ = nullptr;
};

// ---------------------------------------------------------------------------
// DefragService: chunked, crash-safe defrag sessions over registered stores.
// ---------------------------------------------------------------------------
class DefragService {
 public:
  enum class Phase { Idle, Scan, Copy, Commit, Reindex, Done, Failed };

  // Per-tick work bounds (powers of two). A pump() call never does more than
  // this much, so the sim never stalls.
  static constexpr size_t kScanPagesPerTick = 64;
  static constexpr size_t kCopyRecordsPerTick = 256;
  static constexpr size_t kReindexRecordsPerTick = 256;
  // A store mutated mid-defrag forces a scan restart; past this many restarts
  // the session fails closed ("store too busy") rather than livelocking.
  static constexpr int kMaxRestarts = 3;

  DefragService() = default;

  // Registers a defraggable store. The game registers the Fragment store at
  // boot with its save path (the seam the real Fragment persistence fills).
  // The service never owns the store or the reindexer: they must outlive it.
  // False when the name is already registered.
  bool registerStore(const std::string& name, RecordStore& store,
                     const std::filesystem::path& filePath,
                     IReindexer* reindexerOrNull = nullptr);
  bool unregisterStore(const std::string& name);
  std::vector<std::string> storeNames() const;

  // Begins a defrag session on the named store. Only one session runs at a
  // time (fail-closed). False + lastError() when unknown/already running.
  bool beginDefrag(const std::string& storeName);
  // Bounded work for this tick. Drives Scan -> Copy -> Commit -> Reindex.
  void pump();
  // Cancels the session and idles. If the reindexer already cleared the index
  // (reindex begun), cancel() finishes feeding the scanned snapshot
  // synchronously — the index ends up complete and consistent with the store —
  // unless the store mutated mid-reindex, in which case the snapshot is stale
  // and the index is marked dirty instead (see indexDirty()).
  void cancel();

  bool running() const { return phase_ != Phase::Idle && phase_ != Phase::Done &&
                                phase_ != Phase::Failed; }
  Phase phase() const { return phase_; }
  /// True when a cancel/fail landed mid-reindex with a stale snapshot: the
  /// index is cleared-but-partial. The next defrag session's Reindex phase
  /// re-feeds it fully (clearing this flag); until then, searches may miss
  /// documents. Cleared automatically when a reindex runs to completion.
  /// indexDirtyStore() names the store whose index is dirty.
  bool indexDirty() const { return indexDirty_; }
  const std::string& indexDirtyStore() const { return indexDirtyStore_; }
  double progress() const;           // 0..1 across the whole session
  std::string statusLine() const;    // one-liner for the console
  std::string lastError() const { return lastError_; }
  static const char* phaseName(Phase p);

 private:
  struct Entry {
    RecordStore* store = nullptr;
    std::filesystem::path filePath;
    IReindexer* reindexer = nullptr;
  };
  std::unordered_map<std::string, Entry> stores_;

  // Session state.
  Phase phase_ = Phase::Idle;
  std::string sessionStore_;
  Entry sessionEntry_;
  uint64_t startGeneration_ = 0;
  int restarts_ = 0;
  // Scan/Copy workspace: live records in scan order.
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> live_;
  size_t scanPage_ = 0;
  size_t copyIdx_ = 0;
  size_t reindexIdx_ = 0;
  bool reindexBegun_ = false;
  RecordStore compacted_;
  std::string lastError_;
  // Cross-session: set when a cancel/fail lands mid-reindex with a stale
  // snapshot. Survives resetSession(); cleared when a reindex completes.
  bool indexDirty_ = false;
  std::string indexDirtyStore_;

  void pumpScan();
  void pumpCopy();
  void pumpCommit();
  void pumpReindex();
  /// Restart the scan after a mid-defrag store mutation (bounded by
  /// kMaxRestarts by the caller). Resets scan/copy AND reindex cursors.
  void restartScan();
  void fail(const std::string& why);
  void resetSession();
};

}  // namespace litholattice::defrag
