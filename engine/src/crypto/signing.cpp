// LithoKraft mod signing implementation.
#include "litholattice/crypto/signing.h"

#include <algorithm>
#include <array>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

#include "litholattice/core/json.h"
#include "litholattice/crypto/ed25519.h"
#include "litholattice/crypto/sha256.h"

namespace litholattice::crypto {
namespace {

std::string relPosix(const std::filesystem::path& modDir, const std::filesystem::path& p) {
  std::string rel = std::filesystem::relative(p, modDir).string();
  std::replace(rel.begin(), rel.end(), '\\', '/');
  return rel;
}

}  // namespace

bool modTreeHasSymlink(const std::filesystem::path& modDir, std::string* err) {
  std::error_code ec;
  if (!std::filesystem::is_directory(modDir, ec)) {
    if (err) *err = "not a directory: " + modDir.string();
    return false;
  }
  // The walk itself never follows symlinks (no follow_directory_symlink),
  // so a symlinked directory is reported as an entry, not descended into.
  for (auto it = std::filesystem::recursive_directory_iterator(
           modDir, std::filesystem::directory_options::skip_permission_denied, ec);
       it != std::filesystem::recursive_directory_iterator(); ++it) {
    if (ec) {
      if (err) *err = "directory walk failed: " + ec.message();
      return false;
    }
    std::error_code ec2;
    if (it->is_symlink(ec2)) return true;
  }
  return false;
}

std::optional<std::array<uint8_t, 32>> canonicalModHash(const std::filesystem::path& modDir,
                                                        std::string* err) {
  auto fail = [&](const std::string& why) -> std::optional<std::array<uint8_t, 32>> {
    if (err) *err = why;
    return std::nullopt;
  };
  std::error_code ec;
  if (!std::filesystem::is_directory(modDir, ec)) return fail("not a directory: " + modDir.string());

  std::vector<std::string> relPaths;
  for (auto it = std::filesystem::recursive_directory_iterator(
           modDir, std::filesystem::directory_options::skip_permission_denied, ec);
       it != std::filesystem::recursive_directory_iterator(); ++it) {
    if (ec) return fail("directory walk failed: " + ec.message());
    const auto& p = it->path();
    // Never follow symlinks; skip anything that isn't a regular file.
    std::error_code ec2;
    if (it->is_symlink(ec2)) continue;
    if (!it->is_regular_file(ec2)) continue;
    std::string rel = relPosix(modDir, p);
    if (rel == kSignatureFileName) continue;  // the signature signs everything else
    relPaths.push_back(rel);
  }
  std::sort(relPaths.begin(), relPaths.end());  // byte-wise lexicographic on UTF-8

  // Framing v2 (P1b, 2026-10-13, Appendix E H1): every field is
  // length-prefixed, so the byte stream parses back into exactly one
  // (path, bytes) sequence — the v1 framing (rel ‖ 0x00 ‖ bytes, no
  // lengths, no count) let a signature be transplanted between different
  // mods whose streams collided (A {a="", b=""} vs B {a="b\0"}). The file
  // count is hashed first so truncating/appending a file also changes the
  // digest even before any framing is considered.
  Sha256 h;
  auto putU64le = [&h](uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = uint8_t((v >> (8 * i)) & 0xFF);
    h.update(b, 8);
  };
  putU64le(static_cast<uint64_t>(relPaths.size()));
  for (const std::string& rel : relPaths) {
    std::ifstream in(modDir / rel, std::ios::binary);
    if (!in) return fail("cannot read: " + rel);
    // The length prefix needs the size BEFORE the bytes: stat it, then
    // stream the file into the hash in chunks (P3-3, Appendix E M3 — the
    // old code buffered every file whole, so hashing a hostile "mod" of
    // huge files was memory-proportional). A size that disagrees with
    // the bytes actually read fails closed.
    std::error_code fec;
    const uint64_t fileSize = std::filesystem::file_size(modDir / rel, fec);
    if (fec) return fail("cannot stat: " + rel);
    putU64le(static_cast<uint64_t>(rel.size()));
    h.update(reinterpret_cast<const uint8_t*>(rel.data()), rel.size());
    putU64le(fileSize);
    std::array<char, 64 << 10> chunk;
    uint64_t read = 0;
    while (in) {
      in.read(chunk.data(), std::streamsize(chunk.size()));
      const std::streamsize n = in.gcount();
      if (n > 0) {
        h.update(reinterpret_cast<const uint8_t*>(chunk.data()), size_t(n));
        read += uint64_t(n);
      }
    }
    if (in.bad() || read != fileSize) return fail("read error: " + rel);
  }
  return h.final();
}

bool generateKeypair(std::array<uint8_t, 32>& seedOut, std::array<uint8_t, 32>& pubOut) {
  try {
    std::random_device rd;
    for (int i = 0; i < 32; i += 4) {
      const uint32_t v = rd();
      seedOut[size_t(i)] = uint8_t(v);
      seedOut[size_t(i) + 1] = uint8_t(v >> 8);
      seedOut[size_t(i) + 2] = uint8_t(v >> 16);
      seedOut[size_t(i) + 3] = uint8_t(v >> 24);
    }
  } catch (...) {
    return false;
  }
  ed25519::Seed seed;
  std::memcpy(seed.data(), seedOut.data(), 32);
  ed25519::PublicKey pub;
  ed25519::SecretKey sec;
  ed25519::keypair_from_seed(seed, pub, sec);
  std::memcpy(pubOut.data(), pub.data(), 32);
  return true;
}

bool writeKeyFile(const std::filesystem::path& path, const uint8_t key[32]) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(key), 32);
  return !!out;
}

/// Private-key variant: creates the file with mode 0600 (no umask window
/// where another user could read it). P3-3 hardening (Appendix E L2):
/// O_NOFOLLOW refuses to write the seed THROUGH a symlink, and fchmod
/// forces 0600 even when the file already existed with looser permissions
/// (the open() mode only applies at creation). Overwriting an existing
/// REGULAR key file stays allowed — `litho_sign keygen` re-issue is a
/// documented flow — but its permissions are tightened to 0600 first.
bool writePrivateKeyFile(const std::filesystem::path& path, const uint8_t key[32]) {
#if defined(__unix__) || defined(__APPLE__)
  const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);
  if (fd < 0) return false;
  if (::fchmod(fd, 0600) != 0) { ::close(fd); return false; }
  const char* p = reinterpret_cast<const char*>(key);
  size_t left = 32;
  while (left > 0) {
    const ssize_t n = ::write(fd, p, left);
    if (n <= 0) { ::close(fd); return false; }
    p += n;
    left -= size_t(n);
  }
  return ::close(fd) == 0;
#else
  if (!writeKeyFile(path, key)) return false;
  std::error_code ec;
  std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, ec);
  return !ec;
#endif
}

bool readKeyFile(const std::filesystem::path& path, std::array<uint8_t, 32>& keyOut, std::string* err) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    if (err) *err = "cannot open: " + path.string();
    return false;
  }
  if (in.tellg() != 32) {
    if (err) *err = "bad key size (want 32 bytes): " + path.string();
    return false;
  }
  in.seekg(0);
  in.read(reinterpret_cast<char*>(keyOut.data()), 32);
  return !!in;
}

std::string toHex(const uint8_t* data, size_t len) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(kDigits[data[i] >> 4]);
    out.push_back(kDigits[data[i] & 15]);
  }
  return out;
}

bool fromHex(const std::string& hex, uint8_t* out, size_t outLen) {
  if (hex.size() != outLen * 2) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < outLen; ++i) {
    const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = uint8_t((hi << 4) | lo);
  }
  return true;
}

bool signMod(const std::filesystem::path& modDir, const uint8_t seed[32], std::string* err) {
  auto fail = [&](const std::string& why) {
    if (err) *err = why;
    return false;
  };
  auto hash = canonicalModHash(modDir, err);
  if (!hash) return false;
  ed25519::Seed s;
  std::memcpy(s.data(), seed, 32);
  ed25519::PublicKey pub;
  ed25519::SecretKey sec;
  ed25519::keypair_from_seed(s, pub, sec);
  ed25519::Signature sig = ed25519::sign(hash->data(), hash->size(), sec);

  // Minimal JSON writer (avoid depending on a JSON emitter).
  std::ostringstream json;
  json << "{\"algorithm\": \"" << kSignatureAlgorithm << "\", \"public_key\": \"" << toHex(pub.data(), 32)
       << "\", \"signature\": \"" << toHex(sig.data(), 64) << "\"}";
  std::ofstream out(modDir / kSignatureFileName, std::ios::binary | std::ios::trunc);
  if (!out) return fail("cannot write signature.json");
  out << json.str();
  if (!out) return fail("write error on signature.json");
  return true;
}

std::optional<ModSignature> readModSignature(const std::filesystem::path& modDir, std::string* err) {
  auto fail = [&](const std::string& why) -> std::optional<ModSignature> {
    if (err) *err = why;
    return std::nullopt;
  };
  std::ifstream in(modDir / kSignatureFileName, std::ios::binary);
  if (!in) return fail("no signature.json");
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  Json j;
  std::string jerr;
  if (!Json::parse(text, j, &jerr)) return fail("signature.json malformed: " + jerr);
  if (j.getStr("algorithm") != kSignatureAlgorithm)
    return fail("unsupported algorithm: " + j.getStr("algorithm"));
  ModSignature ms;
  if (!fromHex(j.getStr("public_key"), ms.publicKey.data(), 32)) return fail("bad public_key hex");
  if (!fromHex(j.getStr("signature"), ms.signature.data(), 64)) return fail("bad signature hex");
  return ms;
}

bool verifyMod(const std::filesystem::path& modDir, const uint8_t pubKey[32], std::string* err) {
  auto fail = [&](const std::string& why) {
    if (err) *err = why;
    return false;
  };
  auto ms = readModSignature(modDir, err);
  if (!ms) return false;
  // Symlink refusal at verify time (P3-3, Appendix E M4): the canonical
  // hash excludes symlinked content that the loader would read, so a
  // signed mod containing any symlink cannot be Verified.
  std::string symErr;
  if (modTreeHasSymlink(modDir, &symErr))
    return fail("signed mod contains a symlink (refused at verify time): " + symErr);
  // The signature must have been made by THIS key, not just any key.
  if (std::memcmp(ms->publicKey.data(), pubKey, 32) != 0)
    return fail("signature key does not match the expected public key");
  auto hash = canonicalModHash(modDir, err);
  if (!hash) return false;
  ed25519::PublicKey pub;
  std::memcpy(pub.data(), pubKey, 32);
  ed25519::Signature sig;
  std::memcpy(sig.data(), ms->signature.data(), 64);
  if (!ed25519::verify(sig, hash->data(), hash->size(), pub)) return fail("signature INVALID");
  return true;
}

}  // namespace litholattice::crypto
