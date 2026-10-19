// litholattice::storage::SandboxedFile — TOCTOU-hardened mod file opens.
//
// The TOCTOU problem: ModSandbox::resolve() guarantees containment at resolve()
// time, but a hostile mod racing the filesystem could swap a symlink between
// resolve() and open(). This header closes that window. Every open goes through
// three gates, in order:
//   1. resolve() the mod path (lexical + symlink containment, as before).
//   2. open() the resolved path with O_NOFOLLOW (Linux): a trailing symlink
//      fails closed with ELOOP instead of being followed into attacker land.
//   3. fstat the open fd: it must be a regular file (no FIFOs, devices,
//      directories, sockets), and its true path — read back from
//      /proc/self/fd/N (Linux) — must still be inside the sandbox root. This
//      catches intermediate-component swaps that O_NOFOLLOW cannot see.
// Any failure at any gate -> invalid handle / nullopt + err. Fail closed, always.
//
// Platform notes (Linux is the primary target):
//   - O_NOFOLLOW and /proc/self/fd exist on Linux. On other platforms the
//     resolve() + regular-file gates still run, but the trailing-symlink and
//     fd-path gates are best-effort and the TOCTOU window is documented, not
//     closed. Windows needs CreateFile with FILE_FLAG_OPEN_REPARSE_POINT
//     (opens the reparse point itself instead of following it) plus
//     GetFinalPathNameByHandle for the fd re-check — see the builder notes in
//     File-Hardening-Report-2026-10-11.md. That work is explicitly not faked here.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "litholattice/storage/file_contract.h"  // Bytes
#include "litholattice/storage/mod_sandbox.h"

namespace litholattice::storage {

/// RAII handle for a hardened sandboxed open. Owns the fd; closes on
/// destruction. Move-only. Invalid when the fd is -1 (check operator bool).
class SandboxedFile {
 public:
  /// Open `resolved` — an absolute path already vetted by ModSandbox::resolve —
  /// for reading, with O_NOFOLLOW + fstat + fd containment re-check.
  /// On any failure returns an invalid handle and sets err.
  static SandboxedFile openRead(const std::filesystem::path& resolved,
                                const ModSandbox& sandbox, std::string* err);

  SandboxedFile() = default;
  ~SandboxedFile();
  SandboxedFile(SandboxedFile&& other) noexcept;
  SandboxedFile& operator=(SandboxedFile&& other) noexcept;
  SandboxedFile(const SandboxedFile&) = delete;
  SandboxedFile& operator=(const SandboxedFile&) = delete;

  explicit operator bool() const;
  /// The open fd (a real OS descriptor on Linux; -1 when invalid or on the
  /// fallback path, which has no fd abstraction).
  int fd() const;
  /// Read the whole file through the validated fd. Refuses files larger than
  /// `maxBytes` (fail closed): pre-checked with fstat on Linux, with a
  /// running cap while streaming on the fallback path.
  bool readAll(Bytes* out, std::string* err, uint64_t maxBytes = kMaxWholeFileRead);

 private:
  /// Release the handle (idempotent). The destructor and operator= go through
  /// this instead of open-coding (or explicitly invoking the destructor on
  /// *this, which is formally UB).
  void close() noexcept;
  explicit SandboxedFile(int fd) : fd_(fd) {}
#if defined(__linux__)
  int fd_ = -1;
#else
  // Best-effort fallback (non-Linux): no fd abstraction; the TOCTOU gates are
  // Linux-only until the builder wires the Windows equivalents.
  int fd_ = -1;  // unused on the fallback path
  std::FILE* f_ = nullptr;
#endif
};

/// Convenience: resolve + hardened open + read, with no caller code between
/// resolve() and open() — no TOCTOU window at all. Returns nullopt + err on
/// any failure (rejected path, refused open, read error).
std::optional<Bytes> readSandboxedFile(const ModSandbox& sandbox,
                                       std::string_view modPath, std::string* err);

}  // namespace litholattice::storage
