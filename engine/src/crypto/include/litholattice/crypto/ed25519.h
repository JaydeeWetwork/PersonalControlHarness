// Ed25519 (RFC 8032), clean-room implementation for LithoLattice.
// Used for mod signatures. No third-party code; verified against the RFC 8032
// test vectors and differential-tested against libsodium (see crypto_selftest).
//
// This is NOT constant-time and is NOT for high-threat contexts: the private
// key lives offline with JD and signing happens on his machine. Verification
// (the hot path) uses only public inputs.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace litholattice::crypto::ed25519 {

inline constexpr size_t kSeedBytes = 32;
inline constexpr size_t kPublicKeyBytes = 32;
inline constexpr size_t kSecretKeyBytes = 64;  // seed || public key (libsodium layout)
inline constexpr size_t kSignatureBytes = 64;

using Seed = std::array<uint8_t, kSeedBytes>;
using PublicKey = std::array<uint8_t, kPublicKeyBytes>;
using SecretKey = std::array<uint8_t, kSecretKeyBytes>;
using Signature = std::array<uint8_t, kSignatureBytes>;

/// Derive (public key, secret key) from a 32-byte seed.
/// secret = seed || public_key.
void keypair_from_seed(const Seed& seed, PublicKey& pub, SecretKey& sec);

/// Sign a message. Deterministic (RFC 8032).
Signature sign(const uint8_t* msg, size_t msglen, const SecretKey& sec);

/// Verify. Returns true iff the signature is valid for (msg, pub).
/// Checks S < L, that R and A decompress, and the group equation
/// S*G == R + k*A (with a degenerate identity-LHS acceptance).
///
/// KNOWN LIMITATION (P3-3, Appendix E L9 — DOCUMENTED, deliberately not
/// changed): verify does NOT reject small-order R or A encodings and does
/// not enforce canonical field encodings for R/A, so multiple encodings
/// of the same signature may verify (signature MALLEABILITY, not
/// forgery: the trusted key A is fixed by the caller and no forgery was
/// constructed). Acceptable for the current use — a mod-trust anchor
/// where the verifier compares against one baked-in key. MUST be
/// revisited before signatures are ever used for byte-equality,
/// transcript, or deduplication schemes, where malleability bites.
bool verify(const Signature& sig, const uint8_t* msg, size_t msglen, const PublicKey& pub);

}  // namespace litholattice::crypto::ed25519
