// Implements the TOCTOU-hardened sandboxed open (see sandboxed_io.h).
#include "litholattice/storage/sandboxed_io.h"

#include <cstdio>
#include <cstring>
#include <system_error>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace litholattice::storage {
namespace {

// Suffix readlink(2) appends to /proc/self/fd/N when the file was unlinked
// after opening. We fail closed on it: the path on disk no longer matches
// what resolve() vetted.
bool hasDeletedSuffix(const std::string& p) {
  constexpr char kSuffix[] = " (deleted)";
  return p.size() >= sizeof(kSuffix) - 1 &&
         p.compare(p.size() - (sizeof(kSuffix) - 1), sizeof(kSuffix) - 1, kSuffix) == 0;
}

}  // namespace

SandboxedFile::~SandboxedFile() { close(); }

void SandboxedFile::close() noexcept {
#if defined(__linux__)
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
#else
  if (f_) {
    ::fclose(f_);
    f_ = nullptr;
  }
#endif
}

SandboxedFile::SandboxedFile(SandboxedFile&& other) noexcept {
#if defined(__linux__)
  fd_ = other.fd_;
  other.fd_ = -1;
#else
  f_ = other.f_;
  other.f_ = nullptr;
#endif
}

SandboxedFile& SandboxedFile::operator=(SandboxedFile&& other) noexcept {
  if (this != &other) {
    close();  // memberwise move after release: no explicit destructor call
#if defined(__linux__)
    fd_ = other.fd_;
    other.fd_ = -1;
#else
    f_ = other.f_;
    other.f_ = nullptr;
#endif
  }
  return *this;
}

#if defined(__linux__)
SandboxedFile::operator bool() const { return fd_ >= 0; }
int SandboxedFile::fd() const { return fd_; }
#else
SandboxedFile::operator bool() const { return f_ != nullptr; }
int SandboxedFile::fd() const { return -1; }  // no fd abstraction on the fallback path
#endif

SandboxedFile SandboxedFile::openRead(const std::filesystem::path& resolved,
                                      const ModSandbox& sandbox, std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    return SandboxedFile();
  };
  if (resolved.empty() || !resolved.is_absolute())
    return fail("sandboxed open: path must be an absolute resolved path");

#if defined(__linux__)
  // Gate 2: O_NOFOLLOW — a trailing symlink fails closed with ELOOP instead of
  // being followed. This is the check-vs-use race killer for the final component.
  // O_NONBLOCK is set so that opening a FIFO does not block waiting for a
  // writer: the fstat gate below then refuses it as a non-regular file.
  // (O_NONBLOCK is a no-op for regular files.)
  const int fd = ::open(resolved.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ELOOP)
      return fail("sandboxed open refused: trailing symlink not followed: " + resolved.string());
    return fail("sandboxed open failed: " + resolved.string() + ": " + std::strerror(errno));
  }
  SandboxedFile f(fd);

  // Gate 3a: fstat the descriptor — it must be a regular file. Refuses FIFOs,
  // devices, directories, and sockets even if one was swapped in post-resolve.
  struct ::stat st;
  if (::fstat(fd, &st) != 0)
    return fail("sandboxed open: fstat failed: " + std::string(::strerror(errno)));
  if (!S_ISREG(st.st_mode))
    return fail("sandboxed open refused: not a regular file: " + resolved.string());

  // Gate 3b: re-check containment on the fd itself. readlink(/proc/self/fd/N)
  // yields the kernel's true path for this descriptor, so an
  // intermediate-component swap (which O_NOFOLLOW cannot see) is caught here.
  char proc[64];
  std::snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
  char target[4096];
  const ssize_t n = ::readlink(proc, target, sizeof(target) - 1);
  if (n < 0)
    return fail("sandboxed open: cannot re-resolve fd path: " + std::string(::strerror(errno)));
  target[n] = '\0';
  const std::string actual(target);
  if (hasDeletedSuffix(actual))
    return fail("sandboxed open refused: file unlinked after open: " + resolved.string());
  if (!sandbox.contains(std::filesystem::path(actual)))
    return fail("sandboxed open refused: fd path escaped the sandbox root: " + actual);
  return f;
#else
  // Best-effort fallback (non-Linux): resolve() + regular-file check only.
  // The O_NOFOLLOW and fd-path gates are not implemented here — Windows needs
  // CreateFile with FILE_FLAG_OPEN_REPARSE_POINT plus GetFinalPathNameByHandle
  // (see the builder notes in File-Hardening-Report-2026-10-11.md). Documented,
  // not faked.
  std::error_code ec;
  if (!std::filesystem::is_regular_file(resolved, ec) || ec)
    return fail("sandboxed open refused: not a regular file: " + resolved.string());
  std::FILE* fp = std::fopen(resolved.string().c_str(), "rb");
  if (!fp) return fail("sandboxed open failed: " + resolved.string());
  SandboxedFile f;
  f.f_ = fp;
  return f;
#endif
}

bool SandboxedFile::readAll(Bytes* out, std::string* err, uint64_t maxBytes) {
  if (!*this) {
    if (err) *err = "sandboxed read: invalid handle";
    return false;
  }
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    return false;
  };
  Bytes b;
#if defined(__linux__)
  // Fail closed on absurd sizes before reading a byte (the open gates already
  // established this is a regular file, so st_size is meaningful).
  {
    struct ::stat st;
    if (::fstat(fd_, &st) == 0 && S_ISREG(st.st_mode) && uint64_t(st.st_size) > maxBytes)
      return fail("sandboxed read refused: file exceeds the " + std::to_string(maxBytes) +
                  "-byte cap");
  }
  uint8_t buf[65536];
  for (;;) {
    const ssize_t r = ::read(fd_, buf, sizeof(buf));
    if (r < 0) {
      if (errno == EINTR) continue;  // a signal interrupted the read: retry
      return fail(std::string("sandboxed read failed: ") + ::strerror(errno));
    }
    if (r == 0) break;
    if (uint64_t(b.size()) > maxBytes || uint64_t(r) > maxBytes - uint64_t(b.size()))
      return fail("sandboxed read refused: exceeds the " + std::to_string(maxBytes) +
                  "-byte cap while streaming");
    b.insert(b.end(), buf, buf + size_t(r));
  }
#else
  uint8_t buf[65536];
  for (;;) {
    const size_t r = std::fread(buf, 1, sizeof(buf), f_);
    if (uint64_t(b.size()) > maxBytes || uint64_t(r) > maxBytes - uint64_t(b.size()))
      return fail("sandboxed read refused: exceeds the " + std::to_string(maxBytes) +
                  "-byte cap while streaming");
    b.insert(b.end(), buf, buf + r);
    if (r < sizeof(buf)) break;
  }
  if (std::ferror(f_)) return fail("sandboxed read failed");
#endif
  *out = std::move(b);
  return true;
}

std::optional<Bytes> readSandboxedFile(const ModSandbox& sandbox, std::string_view modPath,
                                       std::string* err) {
  std::string rerr;
  auto resolved = sandbox.resolve(modPath, &rerr);
  if (!resolved) {
    if (err) *err = rerr;
    return std::nullopt;
  }
  // No caller code runs between resolve() and openRead(): there is no TOCTOU
  // window left for anyone going through this convenience entry point.
  auto f = SandboxedFile::openRead(*resolved, sandbox, err);
  if (!f) return std::nullopt;
  Bytes out;
  if (!f.readAll(&out, err)) return std::nullopt;
  return out;
}

}  // namespace litholattice::storage
