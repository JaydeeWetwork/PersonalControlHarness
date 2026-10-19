// EXPERIMENTAL (P3-2 C-L3): AnvilRegion has NO production callers. The
// production region path is RegionFile / WorldStore (storage.h); this
// layer is a design experiment kept for evaluation and its own tests.
// Do not wire it into a save path without a fresh review.
//
// litholattice::storage::AnvilRegion: an Anvil-inverted region file layer.
// 32x32 chunk columns per file, an 8 KiB index header (Anvil-compatible 3-byte
// big-endian sector offset + 1-byte count entries), 512-byte sector allocation
// with the free bitmap rebuilt from the index on every open, a per-record
// opaque codec byte, sidecar overflow for records past the 255-sector cap, and
// — the inversion — per-record CRC32 plus a payload-first / header-second
// commit protocol, so a torn write is always detectable and never garbage.
// Payloads are opaque bytes: the layer never interprets chunk contents and
// carries no game block types. See REGION_DESIGN.md.
#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace litholattice::storage {

using Bytes = std::vector<uint8_t>;

class AnvilRegion {
 public:
  static constexpr int kSpan = 32;                 ///< chunk columns per side
  static constexpr size_t kSector = 512;           ///< allocation quantum (Anvil: 4096)
  static constexpr uint32_t kMagic = 0x52414C4Cu;  // "LLAR" (little-endian u32)
  static constexpr uint16_t kFormatVersion = 1;
  static constexpr size_t kHeaderSectors = 17;     ///< 16-byte preamble + 8 KiB tables, padded to whole sectors
  static constexpr uint8_t kExternalBit = 0x80;    ///< set in the count byte: record is in a sidecar
  /// Record framing overhead: [u32 len][u8 codec][u8 flags][payload][u32 crc32].
  static constexpr size_t kRecordOverhead = 10;
  /// Largest inline payload (bytes). The count byte keeps only 7 bits for the
  /// sector count (0x80 is the external-sidecar flag), so a record needs
  /// (kRecordOverhead + payload + kSector - 1) / kSector <= 127 sectors.
  static constexpr size_t kMaxInlineBytes = 127 * kSector - kRecordOverhead;
  // Codec tag numbering follows Anvil's convention; the layer treats it as opaque.
  static constexpr uint8_t kCodecNone = 3;
  static constexpr uint8_t kCodecLZ4 = 4;
  static constexpr uint8_t kCodecCustom = 127;

  /// A chunk record as returned by read().
  struct Record {
    uint8_t codec = kCodecNone;
    Bytes payload;            ///< opaque chunk bytes (as handed to put())
    uint32_t generation = 0;  ///< dirty-generation counter at commit time
  };

  /// A staged (payload-written, not yet committed) record. Exposed so tests —
  /// and only tests — can simulate a crash between payload write and the index
  /// commit. A staged record that is never committed is invisible after reopen.
  struct Staged {
    uint32_t sector = 0;   ///< first sector of the staged payload (0 = invalid)
    uint8_t sectors = 0;   ///< sector run length
    bool external = false; ///< staged into a sidecar instead of sectors
  };

  explicit AnvilRegion(std::filesystem::path p, int rx = 0, int rz = 0)
      : path_(std::move(p)), rx_(rx), rz_(rz) {}
  ~AnvilRegion();

  /// Opens the region, validating the preamble and rebuilding the free-sector
  /// bitmap from the location table. Creates the file (header only) if absent.
  /// False + err on a bad preamble or unreadable header; a damaged file is
  /// never repaired in place.
  bool open(std::string* err);

  /// Reads one chunk record. Returns nullopt with empty err when the chunk is
  /// absent; nullopt with err set when the record fails validation (CRC, bounds,
  /// length) — corrupt is reported, never returned as garbage.
  std::optional<Record> read(int lx, int lz, std::string* err) const;

  /// Full write: stage() then commit(), each flushed. Atomic per record.
  bool put(int lx, int lz, uint8_t codec, const Bytes& payload, std::string* err);

  /// Phase 1 of the commit protocol: allocate + write the payload bytes and
  /// flush, without touching the index. Crash here => record stays invisible.
  Staged stage(int lx, int lz, uint8_t codec, const Bytes& payload, std::string* err);

  /// Phase 2: the single commit point. Writes the timestamp entry, then the
  /// 4-byte location entry last, and flushes. A staged write that is never
  /// committed leaves the old record (or absence) in place.
  bool commit(int lx, int lz, uint8_t codec, const Staged& s, uint32_t payloadLen, std::string* err);

  /// Clears the index entry and frees its sectors (and sidecar, if external).
  bool erase(int lx, int lz, std::string* err);

  uint32_t generation(int lx, int lz) const;  ///< dirty-generation vector read
  /// Index entries readHeader() quarantined (normalised to absent) on the
  /// last open: out-of-range / malformed entries are dropped, never trusted
  /// (P3-2 C-L3 — the normalisation is logged and counted, not silent).
  size_t quarantinedEntryCount() const { return quarantinedEntries_; }
  size_t count() const;                       ///< indexed (non-absent) slots
  size_t usedSectors() const;                 ///< sectors referenced by the index
  size_t fileSectors() const { return fileSectors_; }  ///< total sectors in the file
  /// Introspection for repair/defrag tooling (and crash-simulation tests):
  /// reports the index entry for a slot. False when absent or not open.
  bool locate(int lx, int lz, uint32_t* sector, uint8_t* count) const;

  /// Sidecar path for an external record (absolute chunk coords, same dir).
  static std::filesystem::path sidecarPath(const std::filesystem::path& regionPath, int cx, int cz);

 private:
  static int slot(int lx, int lz) { return lz * kSpan + lx; }
  bool validLocal(int lx, int lz) const { return lx >= 0 && lx < kSpan && lz >= 0 && lz < kSpan; }
  bool readHeader(std::string* err);
  void rebuildBitmap();
  bool allocRun(uint8_t need, uint32_t* outSector, std::string* err);
  void freeRun(uint32_t sector, uint8_t count);
  bool writeAt(uint64_t off, const uint8_t* data, size_t n, std::string* err);
  bool readAt(uint64_t off, uint8_t* data, size_t n, std::string* err) const;
  bool flushFile(std::string* err);
  std::optional<Record> readInline(uint32_t sector, uint8_t count, uint32_t gen, std::string* err) const;
  std::optional<Record> readExternal(int lx, int lz, uint32_t gen, std::string* err) const;

  std::filesystem::path path_;
  int rx_ = 0, rz_ = 0;  ///< region coords (for absolute chunk coords in sidecar names)
  mutable std::FILE* f_ = nullptr;  // opened "r+b"/"w+b" for the region's lifetime
  uint32_t fileSectors_ = 0;
  // Index state (the ground truth; the bitmap is derived from it).
  std::vector<uint32_t> sectors_ = std::vector<uint32_t>(kSpan * kSpan);  // BE 24-bit offset
  std::vector<uint8_t> counts_ = std::vector<uint8_t>(kSpan * kSpan);      // 0 = absent
  std::vector<uint32_t> gens_ = std::vector<uint32_t>(kSpan * kSpan);      // dirty generations
  std::vector<uint64_t> freeBits_;  // rebuilt from the index on open()
  size_t quarantinedEntries_ = 0;   ///< entries normalised to absent by readHeader() (C-L3)
  uint32_t nextGen_ = 1;
  bool opened_ = false;
};

}  // namespace litholattice::storage
