// litholattice::storage: the engine-wide file contract.
//
// Every file the engine writes must survive a crash mid-write. This header is the
// standard, generalized from the .llar region layer's lesson ("the Anvil inversion"):
//   1. Atomic writes: payload -> temp file -> fsync -> backup rotation ->
//      atomic rename over the target. A crash leaves either the old or the new
//      version, never a torn one — with one documented window: when backups are
//      enabled, a crash between the rotation and the commit rename leaves the
//      target ABSENT, the previous generation safe in "<path>.bak.1". That
//      window self-heals: recoverCrashedWrite() (called by RegionFile::open and
//      RecordStore::load) promotes the newest intact generation on the next
//      open. The target is never touched until the replacement is fully on disk.
//   2. Per-record CRC32: framed records (see frameRecord) are checksum-verified on
//      load; corruption is detected, never misread.
//   3. Payload before header: inside multi-part files, data bytes are written and
//      flushed before any index/header that points at them is committed, so a torn
//      write is old-or-absent, never garbage. (AnvilRegion implements this per
//      record; RegionFile implements it per whole-file swap.)
//
// Future file-writing code gets safety by default by going through writeFileAtomic
// (and frameRecord/unframeRecord for checksummed payloads). It is deliberately hard
// to misuse: there is no partial-write API; failures before backup rotation leave
// the target untouched; backup rotation runs after the replacement is fsync'd but
// before the commit rename, and recoverCrashedWrite() heals that crash window.
//
// Concurrency: not safe for two writers to the same path at once. Serialize
// per-path writers with IoScheduler (async_io.h), which strands same-path ops in
// FIFO order.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace litholattice::storage {

using Bytes = std::vector<uint8_t>;

/// Suffix of the temp file an atomic write stages into ("<target>.tmp"). A stale
/// temp file is a crash footprint: it is never read, and the next writeFileAtomic
/// to that target simply overwrites it.
inline constexpr const char* kAtomicTmpSuffix = ".tmp";
/// Suffix pattern for rotated backups: "<target>.bak.1" .. "<target>.bak.N".
inline constexpr const char* kBackupSuffix = ".bak";

/// Default cap for whole-file reads (64 MiB). readFileAll(), SandboxedFile::readAll()
/// and RecordStore::load() refuse larger inputs (fail closed) instead of OOMing on
/// hostile multi-GB files. The size is pre-checked (file_size/fstat) before a byte
/// is read, with a running cap while streaming for non-regular files. Callers with
/// legitimately larger files pass an explicit cap.
inline constexpr uint64_t kMaxWholeFileRead = 64ULL * 1024 * 1024;

/// Backup rotation depth policy (JD 2026-10-11): the number of previous versions
/// kept is a power of two from 2 to 32, default 8. (64 was considered and
/// rejected as too heavy.) Depth 0 is not one of the options — it disables
/// backups entirely, which remains a valid programmatic opt-out.
inline constexpr unsigned kBackupDepthMin = 2;
inline constexpr unsigned kBackupDepthMax = 32;
inline constexpr unsigned kBackupDepthDefault = 8;

/// True iff `depth` is one of the offered backup-depth options (2, 4, 8, 16, 32).
/// Depth 0 (disabled) is intentionally not "valid" here: it is the documented
/// opt-out, not a position on the dial.
inline bool isValidBackupDepth(unsigned depth) {
  return depth >= kBackupDepthMin && depth <= kBackupDepthMax && (depth & (depth - 1)) == 0;
}

struct FileWriteOptions {
  /// fsync the temp file (and, on Linux, the parent directory) before the rename,
  /// so the new version is durable, not just visible. Disable only for scratch
  /// files where durability does not matter.
  bool fsync = true;
  /// Keep this many previous versions as "<path>.bak.1" (newest) .. "<path>.bak.N".
  /// Rotation runs after the new payload is fsync'd but BEFORE the commit rename:
  /// a crash in that window leaves the target absent (the previous generation is
  /// safe in .bak.1); recoverCrashedWrite() promotes the newest intact generation
  /// on the next open, so the window self-heals. The old target becomes .bak.1,
  /// older backups shift up, the oldest is dropped. 0 = no backups (opt-out).
  /// The offered depths are the powers of two 2..32 (see isValidBackupDepth);
  /// the default is kBackupDepthDefault (8).
  unsigned keepBackups = kBackupDepthDefault;
  /// Skip the parent-directory fsync after the commit rename. For batch flushers
  /// (WorldStore): they pass deferDirFsync=true for every file in the batch and
  /// then call syncDirContaining() once — one directory fsync for the whole
  /// batch instead of one per file, with the same batch durability. The file
  /// itself is still fsync'd before the rename. A crash between the renames and
  /// the batch dir fsync can only revert a rename (the old version stays
  /// visible), never tear a file.
  bool deferDirFsync = false;
};

/// fsync the directory containing `path` (Linux; no-op elsewhere). Batch
/// flushers call this once after a run of writeFileAtomic(..., deferDirFsync)
/// instead of paying one directory fsync per file.
void syncDirContaining(const std::filesystem::path& path);

/// Crash-recovery for writeFileAtomic's backup-rotation window. Called by
/// RegionFile::open and RecordStore::load before they read the target.
///
/// RECOVERY ORDER (do not reorder without updating the callers):
///   1. `path` exists and validate(path) passes: nothing to do, return true.
///   2. Otherwise, scan the backup chain "<path>.bak.1" (newest) .. ".bak.32"
///      for the first generation that exists AND passes validate(): promote it
///      by COPYING (never moving) it over `path`, then fsync the copy and its
///      directory. Copying keeps the backup chain's last good copy even if
///      this process crashes mid-copy. If `path` exists but failed validation,
///      it is first moved aside to "<path>.torn" (best-effort, for forensics —
///      a torn primary is never silently destroyed; if a ".torn" already
///      exists, the first free suffix ".torn.1", ".torn.2", ... is used, so
///      the first torn evidence is never overwritten — C-L2). Return true.
///   3. Otherwise, if "<path>.tmp" exists and passes validate(): it is a crash
///      footprint (never read otherwise) that was fsync'd before the crash —
///      atomically rename it over `path`. Return true.
///   4. Otherwise return false. `err` is left EMPTY when there was no crash
///      footprint at all (no primary, no backups, no temp: the caller treats
///      the path as genuinely absent/new); `err` is SET when a footprint was
///      found but no generation validated (fail closed — the caller must not
///      treat the path as a new/empty store).
///
/// NEVER DELETES THE LAST GOOD COPY: backups are only ever copied by this
/// function, never moved or removed (only writeFileAtomic's own rotation
/// retires backup generations). `validate` must be fail-closed: true only for
/// a fully intact image (magic + version + structural checks). Promotion uses
/// header-level integrity; the loader's full validation (record CRCs,
/// deserialization) remains the final gate on the promoted file.
bool recoverCrashedWrite(const std::filesystem::path& path,
                         std::function<bool(const std::filesystem::path&, std::string*)> validate,
                         std::string* err);

/// Crash-safe write: `data` -> "<path>.tmp" (+fsync) -> [backup rotation] ->
/// atomic rename over `path`. True on success. On ANY failure returns false +
/// err, and the temp file is removed. Creates missing parent directories.
/// A failure before backup rotation leaves the target untouched. With
/// opts.keepBackups > 0 the previous target is rotated to "<path>.bak.1"
/// (shifting older backups) after the new payload is fsync'd but before the
/// commit rename: if THAT rename fails, the target is absent and the previous
/// version waits in "<path>.bak.1" — recoverCrashedWrite() restores it on the
/// next open (see its header for the recovery order).
bool writeFileAtomic(const std::filesystem::path& path, const uint8_t* data, size_t n,
                     std::string* err, const FileWriteOptions& opts = {});
inline bool writeFileAtomic(const std::filesystem::path& path, const Bytes& data, std::string* err,
                            const FileWriteOptions& opts = {}) {
  return writeFileAtomic(path, data.data(), data.size(), err, opts);
}

/// Read a whole file into memory. False + err when the file cannot be opened or
/// read (a missing file is an error here — see readValidatedRecord for the
/// absent-vs-corrupt distinction). Refuses files larger than `maxBytes`
/// (fail closed): the size is pre-checked before reading, with a running cap
/// while streaming.
bool readFileAll(const std::filesystem::path& path, Bytes* out, std::string* err,
                 uint64_t maxBytes = kMaxWholeFileRead);

// ---- CRC32-framed records -------------------------------------------------------
// One checksummed payload per file: [u32 LE magic "LFRM"][u32 LE payloadLen]
// [payload][u32 LE crc32]. The CRC covers magic || payloadLen || payload, so a
// truncated, torn, or bit-rotted file fails closed. For multi-record containers
// (region files) use the container's own per-record CRC; this framing is for
// single-payload files (metadata, configs, small snapshots).

/// Magic word at the start of every framed record: ASCII "LFRM" (little-endian).
inline constexpr uint32_t kFrameMagic = 0x4D52464Cu;

/// Wrap `payload` in the framed layout above.
Bytes frameRecord(const Bytes& payload);
/// Verify magic, length, and CRC; return the payload. nullopt + err on any
/// mismatch — never returns garbage. A short buffer is "truncated", not "empty".
std::optional<Bytes> unframeRecord(const uint8_t* data, size_t n, std::string* err);

// ---- quarantine -----------------------------------------------------------------
/// A corrupt file is quarantined, not repaired: it is skipped (reads report
/// absent/corrupt), the reason is logged once, and the count is kept. The file
/// itself is left on disk for recovery tooling — the engine never overwrites or
/// deletes a file it quarantined.
class Quarantine {
 public:
  /// Record that `path` failed validation and was skipped. Logs once via logError.
  void quarantine(const std::filesystem::path& path, const std::string& reason);
  size_t count() const { return entries_.size(); }
  const std::vector<std::string>& entries() const { return entries_; }
  void clear() {
    entries_.clear();
    loggedPaths_.clear();
  }

 private:
  std::vector<std::string> entries_;  // "path: reason", oldest first
  // The "logged once" contract: a corrupt file re-read every frame must not
  // spam the log unboundedly. entries_ still records every call (count() is
  // the audit trail); only the logError line is deduplicated per path.
  std::unordered_set<std::string> loggedPaths_;
};

/// Read `path` and require it to be exactly one framed record (frameRecord).
/// Returns the payload. Corrupt (bad magic / length / CRC) => quarantine it when
/// `q` is non-null, return nullopt + err. A missing file => nullopt with err
/// left empty (absent, not corrupt). An unreadable-but-present file => nullopt +
/// err, not quarantined (that is an I/O failure, not corruption).
std::optional<Bytes> readValidatedRecord(const std::filesystem::path& path, Quarantine* q,
                                          std::string* err);

}  // namespace litholattice::storage
