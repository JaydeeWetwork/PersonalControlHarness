// litholattice::storage: the engine's world save container (see docs/SAVE_FORMAT.md).
// Generic and game-agnostic: region files with an offset index, a palette + bit-packed sub-chunk
// codec, CRC-checked chunk records, versioned payloads with migration chains that a game registers
// (SDK style), and an async store that loads / saves off the calling thread.
#pragma once
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "litholattice/storage/file_contract.h"
#include "litholattice/world/chunk.h"

namespace litholattice { class JobSystem; }  // optional async back-end, see WorldStore

namespace litholattice::storage {

using Bytes = std::vector<uint8_t>;

uint32_t crc32(const uint8_t* data, size_t n);

/// Little-endian byte writer / bounds-checked reader.
class ByteWriter {
 public:
  void u8(uint8_t v) { out.push_back(v); }
  void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
  void u32(uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); }
  void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
  void bytes(const Bytes& b) { u32(uint32_t(b.size())); out.insert(out.end(), b.begin(), b.end()); }
  Bytes out;
};
class ByteReader {
 public:
  ByteReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  bool ok() const { return ok_; }
  bool done() const { return i_ == n_; }
  uint8_t u8() { if (i_ >= n_) { ok_ = false; return 0; } return p_[i_++]; }
  uint16_t u16() { const uint16_t a = u8(); return uint16_t(a | (u8() << 8)); }
  uint32_t u32() { const uint32_t a = u16(); return a | (uint32_t(u16()) << 16); }
  uint64_t u64() { const uint64_t a = u32(); return a | (uint64_t(u32()) << 32); }
  Bytes bytes() {
    const uint32_t n = u32();
    if (!ok_ || n > n_ - i_) { ok_ = false; return {}; }
    Bytes b(p_ + i_, p_ + i_ + n); i_ += n; return b;
  }
 private:
  const uint8_t* p_; size_t n_, i_ = 0; bool ok_ = true;
};

/// Palette + bit-packed array of 4096 uint16 values (blocks or cell states). A uniform array is
/// 1 palette entry and no index bits.
void encodePalette(const uint16_t* v, ByteWriter& w);
bool decodePalette(ByteReader& r, uint16_t* v);

/// One versioned payload kind a game (or the engine) stores per chunk column, e.g. LithoKraft's
/// signal state. `migrate[n]` upgrades bytes of version n to n + 1; loads walk the chain up to
/// `version`, so old saves keep opening as the format evolves.
struct PayloadType {
  uint32_t tag = 0;       ///< four-character code, e.g. fourcc("SGNL")
  uint16_t version = 1;   ///< current version written
  std::function<Bytes(const ChunkColumn&)> save;
  std::function<bool(const Bytes&, ChunkColumn&)> load;
  std::map<uint16_t, std::function<std::optional<Bytes>(const Bytes&)>> migrate;
};
constexpr uint32_t fourcc(const char (&s)[5]) { return uint32_t(s[0]) | uint32_t(s[1]) << 8 | uint32_t(s[2]) << 16 | uint32_t(s[3]) << 24; }

/// Engine payload "BLKS": the column's sub-chunks (block ids + cell states), palette coded.
constexpr uint32_t kBlocksTag = fourcc("BLKS");
constexpr uint16_t kBlocksVersion = 1;
constexpr uint16_t kRecordVersion = 1;  ///< chunk record layout

class PayloadRegistry {
 public:
  PayloadRegistry();  ///< registers the engine's BLKS payload
  void add(PayloadType t) { types_[t.tag] = std::move(t); }
  /// Registry used by the BLKS payload to translate between the StableId voxel
  /// storage and the BlockId disk codec (the on-disk BLKS format is unchanged).
  /// Installed once at startup (the registry outlives the stores; it is only
  /// ever read after boot, so worker-thread saves are safe).
  void setRegistry(const BlockRegistry* registry) { registry_ = registry; }
  const BlockRegistry* registry() const { return registry_; }
  const PayloadType* find(uint32_t tag) const { auto it = types_.find(tag); return it == types_.end() ? nullptr : &it->second; }
  /// Column -> chunk record (all registered payloads, each with its version).
  Bytes encode(const ChunkColumn& c) const;
  /// Chunk record -> column. Unknown payload tags are skipped (kept by no one), old versions migrated.
  std::unique_ptr<ChunkColumn> decode(const Bytes& record, std::string* err) const;
 private:
  std::map<uint32_t, PayloadType> types_;
  const BlockRegistry* registry_ = nullptr;
};

/// One region file: 32 x 32 columns, a fixed index of {offset, size, crc} and appended records.
/// Saving rewrites the region into "<file>.tmp" and renames it over the old one (atomic swap), so
/// a crash leaves either the old or the new region, never a torn one — with the one documented
/// backup-rotation window (see file_contract.h): a crash between rotation and the commit rename
/// leaves the target absent, and open() heals it via recoverCrashedWrite().
class RegionFile {
 public:
  static constexpr int kSpan = 32;
  static constexpr uint32_t kMagic = 0x52474C4C;  // "LLGR"
  static constexpr uint16_t kVersion = 1;
  /// Whole-file read cap for open() (P3-2 C-M3): aligned with the file
  /// contract's kMaxWholeFileRead discipline (64 MiB). A region holds
  /// 1024 columns; a typical column record is a few KiB, so a healthy
  /// region is single-digit MiB — 64 MiB is ~10x headroom, and anything
  /// past it is corruption or a hostile file, refused before a byte is
  /// read into memory (open() fails closed: broken_, never overwritten).
  static constexpr uint64_t kMaxFileBytes = kMaxWholeFileRead;
  /// Last-known-good copies kept per flush ("<file>.bak.1" .. ".bak.N"), rotated
  /// on each successful write via the file contract (file_contract.h), which
  /// defaults to kBackupDepthDefault (8) previous versions.
  explicit RegionFile(std::filesystem::path p) : path_(std::move(p)) {}
  /// Reads the index (and keeps the file's records); false + err on a bad / corrupt header.
  bool open(std::string* err);
  std::optional<Bytes> read(int lx, int lz, std::string* err) const;  ///< nullopt: absent or corrupt (err set)
  void put(int lx, int lz, Bytes record) {
    if (lx < 0 || lx >= kSpan || lz < 0 || lz >= kSpan) {
      assert(false && "RegionFile::put: chunk coordinates out of range");
      return;  // fail closed: never index the vectors out of bounds
    }
    pending_[slot(lx, lz)] = std::move(record);
  }
  bool flush(std::string* err);  ///< write all records (old + pending) via the file contract
  /// Same, with explicit write options. WorldStore's batch flush passes
  /// deferDirFsync=true and fsyncs the directory once afterwards.
  bool flush(std::string* err, const FileWriteOptions& opts);
                                 ///< (temp + fsync + atomic rename, one backup kept); refused if open() failed
  size_t count() const;
 private:
  static int slot(int lx, int lz) { return lz * kSpan + lx; }
  std::filesystem::path path_;
  std::vector<Bytes> records_ = std::vector<Bytes>(kSpan * kSpan);  // as stored (crc checked on read)
  std::vector<uint32_t> crcs_ = std::vector<uint32_t>(kSpan * kSpan);
  std::unordered_map<int, Bytes> pending_;
  bool broken_ = false;  ///< open() failed: never overwrite the damaged file (kept for recovery)
};

/// A world directory of sparse region files "r.<rx>.<rz>.llr" (int32 region coordinates, so the
/// addressable world is about 2^36 blocks a side; only touched regions exist). Load and save run
/// on the store's own I/O thread; callbacks run on that thread.
class WorldStore {
 public:
  /// `jobs`: when non-null, saves/loads/region writes run on the JobSystem via
  /// submitIo() (tail-chained, strict FIFO, one op per submitIo so other I/O work
  /// can interleave); when null, the store keeps its own private I/O thread.
  /// Either way the sim/tick threads never block on disk. The JobSystem (if any)
  /// must outlive the store.
  explicit WorldStore(std::filesystem::path dir, const PayloadRegistry& payloads,
                      JobSystem* jobs = nullptr);
  ~WorldStore();  ///< finishes queued work
  WorldStore(const WorldStore&) = delete;
  WorldStore& operator=(const WorldStore&) = delete;

  void saveAsync(const ChunkColumn& column);  ///< encodes now (caller's thread), writes on the I/O thread
  void saveAsync(std::unique_ptr<ChunkColumn> column);  ///< takes a snapshot copy: encodes and writes on the I/O thread
  /// Runs `job` on the I/O thread, in order with the queued saves and loads.
  void enqueue(std::function<void()> job) { post(std::move(job)); }
  void loadAsync(int cx, int cz, std::function<void(std::unique_ptr<ChunkColumn>, std::string err)> done);
  /// Blocks until every queued save is on disk (regions swapped in). Returns
  /// FALSE when any region flush in the batch it waited for failed (SV-H1:
  /// failures are reported, never swallowed; a failed region keeps its
  /// pending records, so a later flush retries the same data).
  bool flush();
  size_t quarantinedCount() const { return quarantined_; }  ///< regions skipped as corrupt since construction
  /// Queues the region writes and returns at once (autosave). Async by nature,
  /// so the outcome cannot be a return value: the flush job records it in
  /// lastFlushOk()/lastFlushFailureCount(), readable once the queue drains
  /// (flush() is the synchronous reporting surface built on the same job).
  void flushAsync();
  bool lastFlushOk() const { return lastFlushOk_.load(std::memory_order_relaxed); }
  /// Regions whose write failed in the most recent flush job (0 = clean).
  int lastFlushFailureCount() const { return lastFlushFailures_.load(std::memory_order_relaxed); }
  // Synchronous forms (used by tests and tools).
  std::unique_ptr<ChunkColumn> load(int cx, int cz, std::string* err);
  static std::filesystem::path regionPath(const std::filesystem::path& dir, int rx, int rz);

 private:
  void post(std::function<void()> job);
  void run();   ///< private-I/O-thread mode only
  void pumpOne();  ///< JobSystem mode: run one queued job per submitIo, tail-chained
  RegionFile& region(int rx, int rz);
  std::filesystem::path dir_;
  const PayloadRegistry& payloads_;
  std::map<std::pair<int, int>, std::unique_ptr<RegionFile>> regions_;  // I/O thread only
  std::mutex m_;
  std::condition_variable cv_, idle_;
  std::deque<std::function<void()>> jobs_;
  bool stop_ = false, busy_ = false, pumping_ = false;
  std::thread thread_;
  JobSystem* jobsys_ = nullptr;  // not owned
  size_t quarantined_ = 0;
  // Flush-job outcome (SV-H1): written by the I/O job, read after the queue
  // drains. Atomics because the JobSystem back-end runs jobs off this thread.
  std::atomic<bool> lastFlushOk_{true};
  std::atomic<int> lastFlushFailures_{0};
};

}  // namespace litholattice::storage
