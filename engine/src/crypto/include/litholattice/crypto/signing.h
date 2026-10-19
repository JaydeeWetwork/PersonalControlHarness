// LithoKraft mod signing: the crude in-house trust layer (JD 2026-10-12).
//
// This is phase 1 of the signing roadmap: real Ed25519 signatures with
// self-managed keys. JD holds the private key offline; the public key is baked
// into the engine binary (see Worker B: engine verification). When funding
// permits, real PKI certificates replace this — the verify() call site is the
// seam.
//
// CANONICAL MOD HASH (byte-exact spec — the engine verifier MUST reproduce it):
//   1. Walk modDir recursively. Consider every REGULAR file. Symlinks are never
//      followed and never hashed. Anything not a regular file is skipped.
//   2. EXCLUDE <modDir>/signature.json itself (the signature signs everything else).
//   3. Sort by relative path using '/' separators, byte-wise lexicographic
//      order on the UTF-8 bytes.
//   4. H = SHA-256(); first H.update(u64le(file_count)); then for each file
//      in order:
//        H.update(u64le(len(relative_path_bytes))); H.update(relative_path_bytes);
//        H.update(u64le(len(file_bytes)));          H.update(file_bytes);
//      (u64le = unsigned 64-bit little-endian length prefix.)
//   5. The digest is H.final(). An empty file list hashes u64le(0) — eight
//      zero bytes — NOT the empty string.
//
// FRAMING HISTORY: v1 (2026-10-12) hashed rel ‖ 0x00 ‖ bytes with no
// lengths and no count; that framing was ambiguous — different mods could
// produce the identical byte stream (mod A {a="", b=""} vs mod B
// {a="b\0"}), so a signature could be transplanted between them
// (Appendix E H1, found 2026-10-13). v2 (P1b, 2026-10-13) length-prefixes
// both fields and hashes the file count. The algorithm string stays
// "ed25519-sha256-v1" (the signature scheme is unchanged; only the hashed
// byte stream changed).
// RE-ISSUE NOTE: the framing change changes every canonical hash, so ALL
// existing signatures MUST be re-issued against v2 hashes. As of
// 2026-10-13 there are no signatures in the wild (signing landed
// 2026-10-12, the baked key is still the retired test key, and no mod has
// shipped signed) — this is the cheap moment to make the change.
//
// signature.json (written into the mod dir):
//   {"algorithm": "ed25519-sha256-v1",
//    "public_key": "<64 hex chars, the 32-byte Ed25519 public key>",
//    "signature": "<128 hex chars, Ed25519 signature over the canonical hash>"}
//
// Key files: raw 32-byte Ed25519 keys. <prefix>.priv = seed (JD guards this
// with his life; it must NEVER ship). <prefix>.pub = public key.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace litholattice::crypto {

inline constexpr char kSignatureAlgorithm[] = "ed25519-sha256-v1";
inline constexpr char kSignatureFileName[] = "signature.json";

/// SHA-256 over the mod dir per the canonical spec above.
/// Returns nullopt and sets *err on I/O failure.
std::optional<std::array<uint8_t, 32>> canonicalModHash(const std::filesystem::path& modDir,
                                                        std::string* err = nullptr);

/// True when ANY entry in the mod tree is a symlink (P3-3, Appendix E M4).
/// canonicalModHash skips symlinks, so symlinked content contributes
/// nothing to the signed hash while the resource loader happily reads
/// through in-root symlinks — a Verified mod could carry unsigned content
/// behind a link. Verification therefore REFUSES any signed mod whose
/// tree contains a symlink (see verifyMod and the engine verifier).
/// Signing and unsigned (dev) mods are unaffected: the check runs at
/// verify time only.
bool modTreeHasSymlink(const std::filesystem::path& modDir, std::string* err = nullptr);

/// Generate a fresh Ed25519 keypair from the OS RNG.
bool generateKeypair(std::array<uint8_t, 32>& seedOut, std::array<uint8_t, 32>& pubOut);

/// Raw key file I/O (32 bytes). Returns false on I/O error or wrong size.
bool writeKeyFile(const std::filesystem::path& path, const uint8_t key[32]);
/// Private-key variant: 0600 at creation (no umask window); on POSIX it
/// also refuses symlinks (O_NOFOLLOW) and fchmod()s an existing file to
/// 0600 before writing (P3-3, Appendix E L2).
bool writePrivateKeyFile(const std::filesystem::path& path, const uint8_t key[32]);
bool readKeyFile(const std::filesystem::path& path, std::array<uint8_t, 32>& keyOut, std::string* err = nullptr);

/// Hex helpers (lowercase).
std::string toHex(const uint8_t* data, size_t len);
bool fromHex(const std::string& hex, uint8_t* out, size_t outLen);

/// Sign the mod dir's canonical hash with the 32-byte seed; writes signature.json.
/// Returns false and sets *err on failure.
bool signMod(const std::filesystem::path& modDir, const uint8_t seed[32], std::string* err = nullptr);

struct ModSignature {
  std::array<uint8_t, 32> publicKey;
  std::array<uint8_t, 64> signature;
};

/// Read and parse signature.json. Returns nullopt on missing/malformed file.
std::optional<ModSignature> readModSignature(const std::filesystem::path& modDir, std::string* err = nullptr);

/// Verify a mod dir against a public key: recompute the canonical hash and
/// check the Ed25519 signature. Returns false (with *err set) when the
/// signature is missing, malformed, or invalid.
/// NOTE for Worker B: the engine verifier should ALSO check that the
/// signature's public key matches the baked-in JD key — this function verifies
/// against whatever key the signature file names (self-contained, for the CLI).
bool verifyMod(const std::filesystem::path& modDir, const uint8_t pubKey[32], std::string* err = nullptr);

/// Self-test entry point. Returns failure count.
int runCryptoSelfTest();

}  // namespace litholattice::crypto
