#include "litholattice/storage/anvil_region.h"

#include <cstdio>
#include <cstring>

#include "litholattice/storage/storage.h"  // crc32 (same namespace)
#include "litholattice/core/log.h"          // C-L3: quarantined-entry logging in readHeader

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace litholattice::storage {

namespace {

constexpr uint64_t kPreambleSize = 16;
constexpr uint64_t kLocTableOff = kPreambleSize;
constexpr uint64_t kTimeTableOff = kPreambleSize + 4096;
constexpr uint64_t kHeaderBytes = uint64_t(AnvilRegion::kHeaderSectors) * AnvilRegion::kSector;

void putU32LE(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
  p[2] = uint8_t(v >> 16);
  p[3] = uint8_t(v >> 24);
}
uint32_t getU32LE(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool seekAbs(std::FILE* f, uint64_t off) {
#if defined(_WIN32)
  return _fseeki64(f, static_cast<__int64>(off), SEEK_SET) == 0;
#else
  return ::fseeko(f, static_cast<off_t>(off), SEEK_SET) == 0;
#endif
}
uint64_t fileSize(std::FILE* f) {
#if defined(_WIN32)
  _fseeki64(f, 0, SEEK_END);
  return uint64_t(_ftelli64(f));
#else
  ::fseeko(f, 0, SEEK_END);
  return uint64_t(::ftello(f));
#endif
}
bool truncateTo(std::FILE* f, uint64_t size, std::string* err) {
#if defined(_WIN32)
  if (_chsize_s(_fileno(f), static_cast<__int64>(size)) != 0) {
    if (err) *err = "cannot truncate region file";
    return false;
  }
#else
  if (::ftruncate(::fileno(f), static_cast<off_t>(size)) != 0) {
    if (err) *err = "cannot truncate region file";
    return false;
  }
#endif
  return true;
}
bool syncFile(std::FILE* f, std::string* err) {
  if (std::fflush(f) != 0) {
    if (err) *err = "fflush failed";
    return false;
  }
#if defined(_WIN32)
  if (_commit(_fileno(f)) != 0) {
    if (err) *err = "fsync failed";
    return false;
  }
#elif defined(__unix__) || defined(__APPLE__)
  if (::fsync(::fileno(f)) != 0) {
    if (err) *err = "fsync failed";
    return false;
  }
#endif
  return true;
}

/// fsync the DIRECTORY containing `filePath`: without this, a crash after file
/// creation can lose the directory entry itself (the file's data is synced but
/// the name never made it to disk). Best-effort on Windows (no dir fsync API).
bool syncDir(const std::filesystem::path& filePath, std::string* err) {
#if defined(__unix__) || defined(__APPLE__)
  std::error_code ec;
  auto dir = filePath.parent_path();
  if (dir.empty()) dir = ".";
  const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (dfd < 0) {
    if (err) *err = "cannot open directory for fsync";
    return false;
  }
  const bool ok = ::fsync(dfd) == 0;
  ::close(dfd);
  if (!ok && err) *err = "directory fsync failed";
  return ok;
#else
  (void)filePath; (void)err;
  return true;
#endif
}

/// Serializes one record: [u32 LE len][u8 codec][u8 flags][payload][u32 LE crc32].
/// The CRC sits at the end so it covers the contiguous prefix len || codec || flags || payload.
std::vector<uint8_t> buildRecord(uint8_t codec, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> rec(AnvilRegion::kRecordOverhead + payload.size());
  putU32LE(rec.data(), uint32_t(payload.size()));
  rec[4] = codec;
  rec[5] = 0;  // flags, reserved
  if (!payload.empty()) std::memcpy(rec.data() + 6, payload.data(), payload.size());
  putU32LE(rec.data() + 6 + payload.size(), crc32(rec.data(), 6 + payload.size()));
  return rec;
}

/// Validates one record image. False + err on truncation, length mismatch, or CRC failure.
bool parseRecord(const uint8_t* data, size_t n, uint8_t* codecOut, std::vector<uint8_t>* payloadOut,
                 std::string* err) {
  if (n < AnvilRegion::kRecordOverhead) {
    if (err) *err = "record image truncated";
    return false;
  }
  const uint32_t len = getU32LE(data);
  if (uint64_t(AnvilRegion::kRecordOverhead) + len > n) {
    if (err) *err = "record length exceeds its sector allocation: corrupt index or torn write";
    return false;
  }
  if (crc32(data, 6 + len) != getU32LE(data + 6 + len)) {
    if (err) *err = "CRC mismatch: record failed integrity check";
    return false;
  }
  *codecOut = data[4];
  payloadOut->assign(data + 6, data + 6 + len);
  return true;
}

}  // namespace

// The count byte keeps only 7 bits for the sector count (0x80 = external
// sidecar): an inline record must never need more than 127 sectors, or the
// count byte collides with kExternalBit and read() misclassifies the record.
static_assert((AnvilRegion::kRecordOverhead + AnvilRegion::kMaxInlineBytes +
               AnvilRegion::kSector - 1) /
                  AnvilRegion::kSector <=
              127,
              "kMaxInlineBytes must fit the 7-bit inline sector count");

AnvilRegion::~AnvilRegion() {
  if (f_) std::fclose(f_);
}

bool AnvilRegion::writeAt(uint64_t off, const uint8_t* data, size_t n, std::string* err) {
  if (!seekAbs(f_, off) || std::fwrite(data, 1, n, f_) != n) {
    if (err) *err = "write failed at offset " + std::to_string(off);
    return false;
  }
  return true;
}

bool AnvilRegion::readAt(uint64_t off, uint8_t* data, size_t n, std::string* err) const {
  if (!seekAbs(f_, off) || std::fread(data, 1, n, f_) != n) {
    if (err) *err = "read failed at offset " + std::to_string(off);
    return false;
  }
  return true;
}

bool AnvilRegion::flushFile(std::string* err) { return syncFile(f_, err); }

bool AnvilRegion::readHeader(std::string* err) {
  quarantinedEntries_ = 0;  // C-L3: the count describes THIS header read
  uint8_t pre[kPreambleSize];
  if (!readAt(0, pre, sizeof(pre), err)) return false;
  if (getU32LE(pre) != kMagic) {
    if (err) *err = "bad magic: not an LLAR region file";
    return false;
  }
  if (getU32LE(pre + 4) != kFormatVersion) {
    if (err) *err = "unsupported region format version";
    return false;
  }
  if (getU32LE(pre + 8) != kSector) {
    if (err) *err = "sector size mismatch";
    return false;
  }
  uint8_t loc[4096], tim[4096];
  if (!readAt(kLocTableOff, loc, sizeof(loc), err) || !readAt(kTimeTableOff, tim, sizeof(tim), err))
    return false;
  for (int i = 0; i < kSpan * kSpan; ++i) {
    const uint32_t off =
        (uint32_t(loc[4 * i]) << 16) | (uint32_t(loc[4 * i + 1]) << 8) | uint32_t(loc[4 * i + 2]);
    const uint8_t cnt = loc[4 * i + 3];
    const bool ext = (cnt & kExternalBit) != 0;
    bool bad = false;
    if (cnt != 0) {
      if (ext) {
        bad = (off != 0);  // external stub: offset field must be zero
      } else {
        bad = (off < kHeaderSectors) || (uint64_t(off) + (cnt & 0x7F) > fileSectors_);
      }
    } else if (off != 0) {
      bad = true;  // absent entry with a nonzero offset: normalize to absent
    }
    if (bad) {
      // C-L3 (P3-2): this normalisation used to be silent — a malformed
      // entry quietly became "absent" and its record was orphaned with no
      // trace. Count every quarantined entry; the total is logged below
      // and stays readable via quarantinedEntryCount().
      ++quarantinedEntries_;
      sectors_[i] = 0;
      counts_[i] = 0;
    } else {
      sectors_[i] = off;
      counts_[i] = cnt;
    }
    gens_[i] = getU32LE(tim + 4 * i);
  }
  if (quarantinedEntries_ > 0)
    logError("storage: AnvilRegion '%s': quarantined %zu malformed index entr%s on open "
             "(normalised to absent; their records are orphaned on disk, not trusted)",
             path_.string().c_str(), quarantinedEntries_, quarantinedEntries_ == 1 ? "y" : "ies");
  return true;
}

void AnvilRegion::rebuildBitmap() {
  freeBits_.assign((fileSectors_ + 63) / 64, ~uint64_t(0));
  const auto markUsed = [&](uint32_t s) { freeBits_[s / 64] &= ~(uint64_t(1) << (s % 64)); };
  for (uint32_t s = 0; s < kHeaderSectors && s < fileSectors_; ++s) markUsed(s);
  for (int i = 0; i < kSpan * kSpan; ++i) {
    const uint8_t c = counts_[i];
    if (c == 0 || (c & kExternalBit)) continue;
    for (uint32_t s = sectors_[i], e = s + (c & 0x7F); s < e; ++s) markUsed(s);
  }
}

bool AnvilRegion::open(std::string* err) {
  const auto fail = [&](const std::string& e) {
    if (err) *err = e;
    if (f_) {
      std::fclose(f_);
      f_ = nullptr;
    }
    opened_ = false;
    return false;
  };
  f_ = std::fopen(path_.c_str(), "r+b");
  if (!f_) {
    // Never truncate a file we could not read: only create when it does not exist.
    std::error_code ec;
    if (std::filesystem::exists(path_, ec)) return fail("cannot open region file for update");
    f_ = std::fopen(path_.c_str(), "w+b");
    if (!f_) return fail("cannot create region file '" + path_.string() + "'");
  }
  uint64_t size = fileSize(f_);
  if (size == 0) {
    // Fresh file: preamble + zeroed tables, padded to a whole number of sectors.
    uint8_t pre[kPreambleSize] = {};
    putU32LE(pre, kMagic);
    putU32LE(pre + 4, kFormatVersion);
    putU32LE(pre + 8, uint32_t(kSector));
    static const uint8_t zero[4096] = {};
    if (!writeAt(0, pre, sizeof(pre), err) || !writeAt(kLocTableOff, zero, sizeof(zero), err) ||
        !writeAt(kTimeTableOff, zero, sizeof(zero), err))
      return fail(err ? *err : std::string("header write failed"));
    // Extend to the full header size (zero-filled tail).
    if (!truncateTo(f_, kHeaderBytes, err)) return fail(err ? *err : std::string("extend failed"));
    if (!flushFile(err)) return fail(err ? *err : std::string("sync failed"));
    // New file: the directory entry itself must be durable too.
    if (!syncDir(path_, err)) return fail(err ? *err : std::string("directory sync failed"));
    size = kHeaderBytes;
  }
  if (size < kHeaderBytes) return fail("region file smaller than its header: damaged, kept for recovery");
  if (size % kSector != 0) {
    // Torn trailing append. By the commit protocol the index entry for a record
    // is written only after its payload is flushed, so a partial trailing sector
    // can never be indexed: truncating it is provably safe recovery.
    if (!truncateTo(f_, size - size % kSector, err)) return fail(err ? *err : std::string("extend failed"));
    size -= size % kSector;
  }
  fileSectors_ = uint32_t(size / kSector);
  if (!readHeader(err)) return fail(err ? *err : std::string("header unreadable"));
  rebuildBitmap();
  uint32_t mx = 0;
  for (uint32_t g : gens_) mx = g > mx ? g : mx;
  nextGen_ = (mx == 0xFFFFFFFFu) ? 1 : mx + 1;  // resume the dirty-generation counter
  opened_ = true;
  return true;
}

bool AnvilRegion::allocRun(uint8_t need, uint32_t* outSector, std::string* err) {
  uint32_t run = 0, runStart = 0;
  for (uint32_t s = uint32_t(kHeaderSectors); s < fileSectors_; ++s) {
    if (freeBits_[s / 64] & (uint64_t(1) << (s % 64))) {
      if (run == 0) runStart = s;
      if (++run == need) {
        for (uint32_t t = runStart; t < runStart + need; ++t)
          freeBits_[t / 64] &= ~(uint64_t(1) << (t % 64));
        *outSector = runStart;
        return true;
      }
    } else {
      run = 0;
    }
  }
  // No run fits: append at end-of-file.
  const uint64_t grown = uint64_t(fileSectors_) + need;
  if (grown > 0xFFFFFFu) {
    if (err) *err = "region file sector address space exhausted";
    return false;
  }
  if (!truncateTo(f_, grown * kSector, err)) return false;
  *outSector = fileSectors_;
  fileSectors_ = uint32_t(grown);
  freeBits_.resize((fileSectors_ + 63) / 64, ~uint64_t(0));
  for (uint32_t t = *outSector; t < *outSector + need; ++t)
    freeBits_[t / 64] &= ~(uint64_t(1) << (t % 64));
  return true;
}

void AnvilRegion::freeRun(uint32_t sector, uint8_t count) {
  for (uint32_t s = sector, e = sector + count; s < e && s < fileSectors_; ++s)
    freeBits_[s / 64] |= (uint64_t(1) << (s % 64));
}

std::filesystem::path AnvilRegion::sidecarPath(const std::filesystem::path& regionPath, int cx, int cz) {
  return regionPath.parent_path() /
         (regionPath.stem().string() + ".c" + std::to_string(cx) + "." + std::to_string(cz) + ".llx");
}

AnvilRegion::Staged AnvilRegion::stage(int lx, int lz, uint8_t codec,
                                       const std::vector<uint8_t>& payload, std::string* err) {
  Staged none{};
  if (!opened_) {
    if (err) *err = "region not open";
    return none;
  }
  if (!validLocal(lx, lz)) {
    if (err) *err = "chunk coordinates out of range";
    return none;
  }
  if (payload.size() > kMaxInlineBytes) {
    // Overflow: the sidecar is written and flushed before the index commit,
    // so the same payload-first ordering protects the external path.
    const auto rec = buildRecord(codec, payload);
    const auto sp = sidecarPath(path_, rx_ * kSpan + lx, rz_ * kSpan + lz);
    std::FILE* sf = std::fopen(sp.c_str(), "wb");
    if (!sf) {
      if (err) *err = "cannot write sidecar '" + sp.string() + "'";
      return none;
    }
    const bool ok = std::fwrite(rec.data(), 1, rec.size(), sf) == rec.size() && syncFile(sf, nullptr);
    std::fclose(sf);
    if (!ok) {
      if (err) *err = "sidecar write failed";
      return none;
    }
    Staged s{};
    s.external = true;
    return s;
  }
  const uint8_t need =
      uint8_t((kRecordOverhead + payload.size() + kSector - 1) / kSector);  // 1..127 (7-bit count)
  const int sl = slot(lx, lz);
  uint32_t sector = 0;
  const uint8_t oldC = counts_[sl];
  if (oldC != 0 && !(oldC & kExternalBit) && (oldC & 0x7F) >= need) {
    sector = sectors_[sl];  // in-place rewrite: the new record still fits the old run
  } else if (!allocRun(need, &sector, err)) {
    return none;
  }
  const auto rec = buildRecord(codec, payload);
  if (!writeAt(uint64_t(sector) * kSector, rec.data(), rec.size(), err)) return none;
  if (!flushFile(err)) return none;
  Staged s{};
  s.sector = sector;
  s.sectors = need;
  return s;
}

bool AnvilRegion::commit(int lx, int lz, uint8_t codec, const Staged& s, uint32_t payloadLen,
                         std::string* err) {
  if (!opened_) {
    if (err) *err = "region not open";
    return false;
  }
  if (!validLocal(lx, lz)) {
    if (err) *err = "chunk coordinates out of range";
    return false;
  }
  if (!s.external && (s.sector == 0 || s.sectors == 0)) {
    if (err) *err = "nothing staged for this slot";
    return false;
  }
  // Verify the staged payload before making it visible: the index must never
  // point at bytes that fail validation (defense in depth for the commit point).
  {
    uint8_t vcodec = 0;
    std::vector<uint8_t> vpayload;
    std::string verr;
    bool ok = false;
    if (s.external) {
      const auto sp = sidecarPath(path_, rx_ * kSpan + lx, rz_ * kSpan + lz);
      std::FILE* sf = std::fopen(sp.c_str(), "rb");
      if (sf) {
        const uint64_t n = fileSize(sf);
        if (n <= 64 * 1024 * 1024 && seekAbs(sf, 0)) {
          std::vector<uint8_t> buf(static_cast<size_t>(n));
          ok = std::fread(buf.data(), 1, buf.size(), sf) == buf.size() &&
               parseRecord(buf.data(), buf.size(), &vcodec, &vpayload, &verr);
          if (!ok && verr.empty()) verr = "sidecar read short";
        } else {
          verr = "sidecar implausibly large";
        }
        std::fclose(sf);
      } else {
        verr = "staged sidecar missing";
      }
    } else {
      std::vector<uint8_t> buf(size_t(s.sectors) * kSector);
      ok = readAt(uint64_t(s.sector) * kSector, buf.data(), buf.size(), &verr) &&
           parseRecord(buf.data(), buf.size(), &vcodec, &vpayload, &verr);
    }
    if (!ok || vcodec != codec || vpayload.size() != payloadLen) {
      if (err) *err = "staged payload failed verification (" + verr + "): index left untouched";
      return false;
    }
  }
  const int sl = slot(lx, lz);
  const uint32_t oldSector = sectors_[sl];
  const uint8_t oldC = counts_[sl];
  const uint32_t gen = nextGen_++;
  if (nextGen_ == 0) nextGen_ = 1;  // generation 0 is reserved for "absent"
  // Timestamp entry first, then the 4-byte location entry LAST: the single commit point.
  uint8_t tb[4];
  putU32LE(tb, gen);
  if (!writeAt(kTimeTableOff + uint64_t(4 * sl), tb, sizeof(tb), err)) return false;
  uint8_t le[4];
  if (s.external) {
    le[0] = le[1] = le[2] = 0;
    le[3] = kExternalBit;
  } else {
    le[0] = uint8_t(s.sector >> 16);
    le[1] = uint8_t(s.sector >> 8);
    le[2] = uint8_t(s.sector);
    le[3] = s.sectors;
  }
  if (!writeAt(kLocTableOff + uint64_t(4 * sl), le, sizeof(le), err)) return false;
  if (!flushFile(err)) return false;
  // Publish in memory; retire the previous record's storage.
  sectors_[sl] = s.external ? 0 : s.sector;
  counts_[sl] = s.external ? kExternalBit : s.sectors;
  gens_[sl] = gen;
  if (oldC != 0 && !(oldC & kExternalBit)) {
    if (s.external || oldSector != s.sector) {
      freeRun(oldSector, oldC & 0x7F);
    } else if (!s.external && (oldC & 0x7F) > s.sectors) {
      // In-place shrink: the tail sectors would otherwise leak (marked used forever).
      freeRun(oldSector + s.sectors, uint8_t((oldC & 0x7F) - s.sectors));
    }
  }
  if ((oldC & kExternalBit) && !s.external) {
    std::error_code ec;
    std::filesystem::remove(sidecarPath(path_, rx_ * kSpan + lx, rz_ * kSpan + lz), ec);
  }
  return true;
}

bool AnvilRegion::put(int lx, int lz, uint8_t codec, const std::vector<uint8_t>& payload,
                      std::string* err) {
  const Staged s = stage(lx, lz, codec, payload, err);
  if (!s.external && s.sector == 0) return false;
  return commit(lx, lz, codec, s, uint32_t(payload.size()), err);
}

bool AnvilRegion::erase(int lx, int lz, std::string* err) {
  if (!opened_) {
    if (err) *err = "region not open";
    return false;
  }
  if (!validLocal(lx, lz)) {
    if (err) *err = "chunk coordinates out of range";
    return false;
  }
  const int sl = slot(lx, lz);
  const uint8_t oldC = counts_[sl];
  if (oldC == 0) return true;
  if (oldC & kExternalBit) {
    std::error_code ec;
    std::filesystem::remove(sidecarPath(path_, rx_ * kSpan + lx, rz_ * kSpan + lz), ec);
  } else {
    freeRun(sectors_[sl], oldC & 0x7F);
  }
  static const uint8_t zero[4] = {};
  if (!writeAt(kLocTableOff + uint64_t(4 * sl), zero, sizeof(zero), err)) return false;
  if (!flushFile(err)) return false;
  sectors_[sl] = 0;
  counts_[sl] = 0;
  return true;
}

std::optional<AnvilRegion::Record> AnvilRegion::readInline(uint32_t sector, uint8_t count,
                                                          uint32_t gen, std::string* err) const {
  if (sector < kHeaderSectors || uint64_t(sector) + count > fileSectors_) {
    if (err) *err = "record location out of bounds: the index entry is corrupt";
    return std::nullopt;
  }
  std::vector<uint8_t> buf(size_t(count) * kSector);
  if (!readAt(uint64_t(sector) * kSector, buf.data(), buf.size(), err)) return std::nullopt;
  uint8_t codec = 0;
  std::vector<uint8_t> payload;
  std::string perr;
  if (!parseRecord(buf.data(), buf.size(), &codec, &payload, &perr)) {
    if (err) *err = perr;
    return std::nullopt;
  }
  Record r;
  r.codec = codec;
  r.payload = std::move(payload);
  r.generation = gen;
  return r;
}

std::optional<AnvilRegion::Record> AnvilRegion::readExternal(int lx, int lz, uint32_t gen,
                                                            std::string* err) const {
  const auto sp = sidecarPath(path_, rx_ * kSpan + lx, rz_ * kSpan + lz);
  std::FILE* sf = std::fopen(sp.c_str(), "rb");
  if (!sf) {
    if (err) *err = "external sidecar missing: '" + sp.string() + "'";
    return std::nullopt;
  }
  const uint64_t n = fileSize(sf);
  std::vector<uint8_t> buf;
  std::string perr;
  bool ok = false;
  if (n >= kRecordOverhead && n <= 64 * 1024 * 1024 && seekAbs(sf, 0)) {
    buf.resize(size_t(n));
    ok = std::fread(buf.data(), 1, buf.size(), sf) == buf.size();
    if (!ok) perr = "sidecar read failed";
  } else {
    perr = "sidecar has an implausible size";
  }
  std::fclose(sf);
  uint8_t codec = 0;
  std::vector<uint8_t> payload;
  if (ok) ok = parseRecord(buf.data(), buf.size(), &codec, &payload, &perr);
  if (!ok) {
    if (err) *err = perr;
    return std::nullopt;
  }
  Record r;
  r.codec = codec;
  r.payload = std::move(payload);
  r.generation = gen;
  return r;
}

std::optional<AnvilRegion::Record> AnvilRegion::read(int lx, int lz, std::string* err) const {
  if (!opened_) {
    if (err) *err = "region not open";
    return std::nullopt;
  }
  if (!validLocal(lx, lz)) {
    if (err) *err = "chunk coordinates out of range";
    return std::nullopt;
  }
  const int sl = slot(lx, lz);
  const uint8_t c = counts_[sl];
  if (c == 0) return std::nullopt;  // absent: err deliberately left untouched
  if (c & kExternalBit) return readExternal(lx, lz, gens_[sl], err);
  return readInline(sectors_[sl], c & 0x7F, gens_[sl], err);
}

bool AnvilRegion::locate(int lx, int lz, uint32_t* sector, uint8_t* count) const {
  if (!opened_ || !validLocal(lx, lz) || counts_[slot(lx, lz)] == 0) return false;
  if (sector) *sector = sectors_[slot(lx, lz)];
  if (count) *count = counts_[slot(lx, lz)];
  return true;
}

uint32_t AnvilRegion::generation(int lx, int lz) const {
  if (!opened_ || !validLocal(lx, lz)) return 0;
  return gens_[slot(lx, lz)];
}

size_t AnvilRegion::count() const {
  size_t n = 0;
  for (uint8_t c : counts_) n += (c != 0);
  return n;
}

size_t AnvilRegion::usedSectors() const {
  size_t n = 0;
  for (int i = 0; i < kSpan * kSpan; ++i)
    if (counts_[i] != 0 && !(counts_[i] & kExternalBit)) n += counts_[i] & 0x7F;
  return n;
}

}  // namespace litholattice::storage
