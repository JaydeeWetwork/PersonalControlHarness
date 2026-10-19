// Engine-side mod signature verification (Worker B).
//
// Trust model (JD 2026-10-12, locked): certificates answer WHO vouches for the
// mod; sandboxing answers WHAT it can do. Tiers:
//   - Verified: signature.json exists, names the baked-in JD public key, and
//     the Ed25519 signature over the canonical mod hash is valid. Full trust.
//   - Unsigned: no signature.json. Loads sandboxed, clearly labeled.
//   - Invalid: signature.json exists but is malformed, names a different key,
//     or the signature doesn't verify. REFUSED in all builds (fail closed).
//
// The baked-in public key below is a THROWAWAY TEST KEY. JD must replace it
// with his real key before beta (see the marker). The private key never ships.
#pragma once
#include <array>

#include <cstdint>
#include <filesystem>
#include <string>

#include "litholattice/assets/mod_manifest.h"  // SignatureStatus

namespace litholattice::assets {

/// Human-readable status for logs.
const char* signatureStatusName(SignatureStatus s);

// ============================================================================
// BAKED-IN PUBLIC KEY
// ============================================================================
// REPLACE BEFORE BETA — this is a throwaway test key, NOT JD's real key.
//
// To install the real key (2-minute operation for JD):
//   1. On JD's OFFLINE machine:  ./litho_sign keygen --out jd_master
//      (jd_master.priv is the crown jewels — never ships, never commits.)
//   2. Copy jd_master.pub to this machine. Get the hex:
//        od -A n -t x1 jd_master.pub | tr -d ' \n'
//   3. Paste the 64 hex chars below, replacing the test key.
//   4. Rebuild. Verify with: ./litho_sign verify --key jd_master.pub --mod <signed mod>
//      then confirm the engine reports "Verified" at load.
//
// Until then, every mod signed with the test key verifies — which is fine for
// development and MUST NOT ship to players.
// ============================================================================
inline constexpr char kBakedPublicKeyHex[] =
    "3bb44b55eabf04186da2ba95b8d93459eadc59d397815487ac45a27d8a9bb787";  // TEST KEY — REPLACE BEFORE BETA

/// Verify the mod in `modDir` against the baked-in public key.
/// Returns the SignatureStatus; on Invalid, *err describes why.
/// Never throws; works identically in debug and release builds.
SignatureStatus verifyModSignature(const std::filesystem::path& modDir, std::string* err = nullptr);

/// Verify the mod in `modDir` against an explicit trusted public key (for tests:
/// generate an ephemeral keypair per run via crypto::generateKeypair — never
/// hardcode a private seed in the tree).
SignatureStatus verifyModSignatureAgainstKey(const std::filesystem::path& modDir,
                                             const std::array<uint8_t, 32>& trustedPub,
                                             std::string* err = nullptr);

/// True when the baked-in key is the retired throwaway test key. Shipping builds
/// must fail closed on this (see main.cpp boot guard). Public-key comparison only.
///
/// GUARD SCOPE (P3-3, Appendix E L3 — read before relying on the guard):
/// the ONLY enforcement point today is the game's main() boot sequence
/// (game/LithoKraft/src/app/main.cpp): it calls bakedKeyIsTestKey() and
/// exit(1)s in release (NDEBUG) builds, warning loudly in debug builds.
/// Nothing in the engine library, the litho_sign tool, or any other entry
/// point performs this check. Any FUTURE executable that links
/// litholattice_assets and calls verifyModSignature() must call
/// bakedKeyIsTestKey() itself at startup, or it will silently trust the
/// test key. The check is deliberately a library FUNCTION, not a library
/// side effect, so tools and tests can verify test-signed mods on purpose.
bool bakedKeyIsTestKey();

}  // namespace litholattice::assets
