// Implements the engine-wide file contract (see file_contract.h).
#include "litholattice/storage/file_contract.h"

#include <cstdio>
#include <cstring>
#include <system_error>

#include "litholattice/core/log.h"
#include "litholattice/storage/storage.h"  // crc32

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace litholattice::storage {
namespace {

// Little-endian u32 helpers (the contract is LE throughout, like the save format).
void putU32(Bytes& b, uint32_t v) {
  b.push_back(uint8_t(v));
  b.push_back(uint8_t(v >> 8));
  b.push_back(uint8_t(v >> 16));
  b.push_back(uint8_t(v >> 24));
}
uint32_t getU32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool fsyncFile(FILE* f, const char* what, std::string* err) {
#if defined(__linux__)
  if (::fflush(f) != 0 || ::fsync(::fileno(f)) != 0) {
    if (err) *err = std::string("fsync failed (") + what + "): " + std::strerror(errno);
    return false;
  }
#else
  if (::fflush(f) != 0) {
    if (err) *err = std::string("flush failed (") + what + ")";
    return false;
  }
#endif
  return true;
}

// Best-effort durability for the rename itself: fsync the parent directory so the
// directory entry lands on disk too. Linux-only; elsewhere a no-op (documented).
void fsyncParentDir(const std::filesystem::path& path) {
#if defined(__linux__)
  const std::filesystem::path parent = path.parent_path();
  const std::string dir = parent.empty() ? "." : parent.string();
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
#else
  (void)path;
#endif
}

std::filesystem::path backupPath(const std::filesystem::path& path, unsigned i) {
  return std::filesystem::path(path.string() + kBackupSuffix + "." + std::to_string(i));
}

}  // namespace

bool writeFileAtomic(const std::filesystem::path& path, const uint8_t* data, size_t n,
                     std::string* err, const FileWriteOptions& opts) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    return false;
  };
  if (path.empty()) return fail("writeFileAtomic: empty path");

  // The temp file must live in the same directory (same volume) for the rename
  // to be atomic.
  const std::filesystem::path parent = path.parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) return fail("cannot create directory " + parent.string() + ": " + ec.message());
  }
  const std::filesystem::path tmp = std::filesystem::path(path.string() + kAtomicTmpSuffix);

  // Phase 1: payload. The target is not touched until the rename.
  {
    FILE* f = ::fopen(tmp.string().c_str(), "wb");
    if (!f) return fail("cannot open temp file " + tmp.string() + ": " + std::strerror(errno));
    bool ok = true;
    std::string why;
    if (n > 0 && ::fwrite(data, 1, n, f) != n) {
      ok = false;
      why = "short write to " + tmp.string() + ": " + std::strerror(errno);
    }
    if (ok && opts.fsync && !fsyncFile(f, tmp.string().c_str(), &why)) ok = false;
    // fclose flushes once more; check its status too (a deferred write error
    // surfaces here, e.g. disk full).
    if (::fclose(f) != 0 && ok) {
      ok = false;
      why = "close failed on " + tmp.string() + ": " + std::strerror(errno);
    }
    if (!ok) {
      std::error_code ec;
      std::filesystem::remove(tmp, ec);  // never leave a half-written temp behind
      return fail(why);
    }
  }

  // Phase 2: backup rotation (old target -> .bak.1, shifting older backups up).
  // This runs BEFORE the rename so the previous version is preserved.
  if (opts.keepBackups > 0) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
      for (unsigned i = opts.keepBackups; i >= 2; --i) {
        std::filesystem::rename(backupPath(path, i - 1), backupPath(path, i), ec);
        ec.clear();  // a missing backup is fine; anything else is best-effort too
      }
      std::filesystem::rename(path, backupPath(path, 1), ec);
      if (ec) {
        std::filesystem::remove(tmp, ec);
        ec.clear();
        return fail("backup rotation failed for " + path.string() + ": " + ec.message());
      }
    }
  }

  // Phase 3: the single commit point — atomic rename over the target.
  {
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
      std::filesystem::remove(tmp, ec);
      return fail("rename " + tmp.string() + " -> " + path.string() + " failed: " + ec.message());
    }
  }
  if (opts.fsync && !opts.deferDirFsync) fsyncParentDir(path);
  return true;
}

void syncDirContaining(const std::filesystem::path& path) { fsyncParentDir(path); }

bool recoverCrashedWrite(const std::filesystem::path& path,
                         std::function<bool(const std::filesystem::path&, std::string*)> validate,
                         std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    return false;
  };
  if (path.empty() || !validate) return fail("recoverCrashedWrite: empty path");
  std::error_code ec;

  // Step 1: the primary exists and validates — nothing to do (fast path).
  const bool primaryExists = std::filesystem::exists(path, ec);
  ec.clear();
  bool sawFootprint = primaryExists;
  if (primaryExists) {
    std::string verr;
    if (validate(path, &verr)) return true;
    // Present but torn: fall through to the backup scan. The torn primary is
    // moved aside (never silently destroyed) before any promotion below.
  }

  // Step 2: scan the backup chain newest-first for an intact generation.
  std::filesystem::path good;
  std::string lastWhy;
  for (unsigned i = 1; i <= kBackupDepthMax; ++i) {
    const auto cand = backupPath(path, i);
    if (!std::filesystem::exists(cand, ec)) {
      ec.clear();
      continue;
    }
    sawFootprint = true;
    std::string verr;
    if (validate(cand, &verr)) {
      good = cand;
      break;
    }
    if (lastWhy.empty() && !verr.empty()) lastWhy = cand.string() + ": " + verr;
  }
  if (!good.empty()) {
    if (primaryExists) {
      // Forensics first: a torn primary is evidence, not garbage. C-L2
      // (P3-2): never overwrite an earlier .torn — the FIRST torn copy is
      // the original crash evidence, so rotate to the first free suffix
      // (.torn, .torn.1, .torn.2, ...) instead of destroying it.
      std::filesystem::path torn(path.string() + ".torn");
      for (unsigned i = 1; std::filesystem::exists(torn, ec); ++i) {
        ec.clear();
        torn = std::filesystem::path(path.string() + ".torn." + std::to_string(i));
      }
      ec.clear();
      std::filesystem::rename(path, torn, ec);
      ec.clear();
    }
    // COPY, never move: the backup chain keeps its last good copy even if
    // this process crashes mid-copy. Then fsync the restored file and its
    // directory so the recovery itself is durable.
    std::filesystem::copy_file(good, path, std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec)
      return fail("recoverCrashedWrite: cannot restore " + good.string() + " over " +
                  path.string() + ": " + ec.message());
    FILE* f = ::fopen(path.string().c_str(), "rb");
    if (f) {
      std::string why;
      const bool ok = fsyncFile(f, path.string().c_str(), &why);
      ::fclose(f);
      if (!ok) return fail("recoverCrashedWrite: fsync after restore failed: " + why);
    }
    fsyncParentDir(path);
    logWarn("storage: recovered '%s' from backup '%s' (crash during atomic write)",
            path.string().c_str(), good.string().c_str());
    return true;
  }

  // Step 3: the temp file is a crash footprint (never read otherwise). If it
  // validates, it was fsync'd before the crash: atomically promote it.
  const auto tmp = std::filesystem::path(path.string() + kAtomicTmpSuffix);
  if (std::filesystem::exists(tmp, ec)) {
    ec.clear();
    sawFootprint = true;
    std::string verr;
    if (validate(tmp, &verr)) {
      std::filesystem::rename(tmp, path, ec);
      if (ec) return fail("recoverCrashedWrite: cannot promote temp file: " + ec.message());
      fsyncParentDir(path);
      logWarn("storage: recovered '%s' by promoting its temp file (crash before commit)",
              path.string().c_str());
      return true;
    }
    if (lastWhy.empty() && !verr.empty()) lastWhy = tmp.string() + ": " + verr;
  } else {
    ec.clear();
  }

  // Step 4: nothing recoverable. No footprint at all => err stays empty and
  // the caller treats the path as genuinely absent/new. A footprint with no
  // intact generation => fail closed.
  if (!sawFootprint) return false;
  std::string why = "recoverCrashedWrite: no intact generation for " + path.string();
  if (!lastWhy.empty()) why += " (" + lastWhy + ")";
  return fail(why);
}

bool readFileAll(const std::filesystem::path& path, Bytes* out, std::string* err,
                 uint64_t maxBytes) {
  FILE* f = ::fopen(path.string().c_str(), "rb");
  if (!f) {
    if (err) *err = "cannot open " + path.string() + ": " + std::strerror(errno);
    return false;
  }
  auto fail = [&](const std::string& m) {
    ::fclose(f);
    if (err) *err = m;
    return false;
  };
  // Fail closed on absurd sizes BEFORE reading a byte.
  Bytes b;
  {
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(path, ec);
    if (!ec) {
      if (size > maxBytes)
        return fail("refusing to read " + path.string() + " (" + std::to_string(size) +
                    " bytes exceeds the " + std::to_string(maxBytes) + "-byte cap)");
      // Size known: single allocation, no reallocation churn below.
      if (size <= uint64_t(SIZE_MAX)) b.reserve(size_t(size));
    }
  }
  uint8_t buf[65536];
  for (;;) {
    const size_t r = ::fread(buf, 1, sizeof(buf), f);
    // Running cap for the no-pre-check case (non-regular files, races): the
    // file cannot grow past the cap under us.
    if (b.size() > maxBytes || r > maxBytes - b.size())
      return fail("refusing to read " + path.string() + ": exceeds the " +
                  std::to_string(maxBytes) + "-byte cap while streaming");
    b.insert(b.end(), buf, buf + r);
    if (r < sizeof(buf)) break;
  }
  const bool ok = ::ferror(f) == 0;
  ::fclose(f);
  if (!ok) {
    if (err) *err = "read failed: " + path.string();
    return false;
  }
  *out = std::move(b);
  return true;
}

Bytes frameRecord(const Bytes& payload) {
  Bytes b;
  b.reserve(12 + payload.size());
  putU32(b, kFrameMagic);
  putU32(b, uint32_t(payload.size()));
  b.insert(b.end(), payload.begin(), payload.end());
  putU32(b, crc32(b.data(), b.size()));  // covers magic || len || payload
  return b;
}

std::optional<Bytes> unframeRecord(const uint8_t* data, size_t n, std::string* err) {
  auto fail = [&](const std::string& m) -> std::optional<Bytes> {
    if (err) *err = m;
    return std::nullopt;
  };
  if (n < 12) return fail("framed record truncated (" + std::to_string(n) + " bytes)");
  if (getU32(data) != kFrameMagic) return fail("framed record has a bad magic word");
  const uint32_t len = getU32(data + 4);
  if (12 + size_t(len) != n) return fail("framed record length mismatch");
  if (crc32(data, 8 + len) != getU32(data + 8 + len)) return fail("framed record CRC mismatch");
  return Bytes(data + 8, data + 8 + len);
}

void Quarantine::quarantine(const std::filesystem::path& path, const std::string& reason) {
  entries_.push_back(path.string() + ": " + reason);
  // "Logged once": the entry is always recorded (count() is the audit trail),
  // but the log line fires only the first time per path.
  if (loggedPaths_.insert(path.string()).second)
    logError("storage: quarantined %s (%s); skipped, left on disk for recovery",
             path.string().c_str(), reason.c_str());
}

std::optional<Bytes> readValidatedRecord(const std::filesystem::path& path, Quarantine* q,
                                          std::string* err) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return std::nullopt;  // absent, not corrupt
  Bytes all;
  if (!readFileAll(path, &all, err)) return std::nullopt;  // I/O failure, not corruption
  std::string why;
  auto payload = unframeRecord(all.data(), all.size(), &why);
  if (!payload) {
    if (q) q->quarantine(path, why);
    if (err) *err = why;
    return std::nullopt;
  }
  return payload;
}

}  // namespace litholattice::storage
