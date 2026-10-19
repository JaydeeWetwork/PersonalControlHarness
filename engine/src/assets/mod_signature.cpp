// Engine-side mod signature verification. Uses Worker A's tested crypto
// (litholattice_crypto): canonicalModHash + Ed25519 verify.
#include "litholattice/assets/mod_signature.h"

#include <array>
#include <cstring>
#include <fstream>

#include "litholattice/crypto/ed25519.h"
#include "litholattice/crypto/sha256.h"
#include "litholattice/crypto/signing.h"

namespace litholattice::assets {

const char* signatureStatusName(SignatureStatus s) {
  switch (s) {
    case SignatureStatus::Verified: return "verified";
    case SignatureStatus::Unsigned: return "unsigned";
    case SignatureStatus::Invalid: return "invalid";
  }
  return "unknown";
}

namespace {

// The baked key, decoded from hex once at startup.
bool bakedKey(std::array<uint8_t, 32>& out) {
  return crypto::fromHex(kBakedPublicKeyHex, out.data(), out.size());
}

}  // namespace

/// Core verifier: checks signature.json against an explicit trusted public key.
/// `keyDesc` names the key for error messages ("baked-in key", "test key", ...).
static SignatureStatus verifyAgainstKey(const std::filesystem::path& modDir,
                                        const std::array<uint8_t, 32>& trustedPub,
                                        const char* keyDesc, std::string* err) {
  auto setErr = [&](const std::string& m) {
    if (err) *err = m;
  };

  // No signature.json → unsigned (not an error; sandboxed path handles it).
  // A filesystem ERROR here is fail-closed (Invalid, not Unsigned): a transient
  // I/O failure must not downgrade a signed mod to the sandboxed path.
  const std::filesystem::path sigPath = modDir / crypto::kSignatureFileName;
  std::error_code ec;
  const bool exists = std::filesystem::exists(sigPath, ec);
  if (ec) {
    setErr(std::string("cannot stat signature.json: ") + ec.message());
    return SignatureStatus::Invalid;
  }
  if (!exists) {
    return SignatureStatus::Unsigned;
  }

  // Parse signature.json.
  std::string parseErr;
  auto sig = crypto::readModSignature(modDir, &parseErr);
  if (!sig) {
    setErr(std::string("signature.json malformed: ") + parseErr);
    return SignatureStatus::Invalid;
  }

  // The signature must name JD's baked-in key. A valid signature under any
  // OTHER key is still Invalid — we only trust JD (first-party) and keys JD
  // signs for (second-party, via JD's signature on their mod).
  if (std::memcmp(sig->publicKey.data(), trustedPub.data(), 32) != 0) {
    setErr(std::string("signature.json names an untrusted public key (not ") + keyDesc + ")");
    return SignatureStatus::Invalid;
  }

  // Symlink refusal at verify time (P3-3, Appendix E M4): the canonical
  // hash excludes symlinked content that the resource loader would read
  // through in-root links, so any signed mod whose tree contains a
  // symlink is Invalid. Runs only when a signature exists — unsigned dev
  // mods (returned above) are unaffected.
  {
    std::string symErr;
    if (crypto::modTreeHasSymlink(modDir, &symErr)) {
      setErr("signed mod contains a symlink (refused at verify time)");
      return SignatureStatus::Invalid;
    }
  }

  // Recompute the canonical hash and verify the Ed25519 signature.
  std::string hashErr;
  auto hash = crypto::canonicalModHash(modDir, &hashErr);
  if (!hash) {
    setErr(std::string("cannot hash mod dir: ") + hashErr);
    return SignatureStatus::Invalid;
  }
  crypto::ed25519::Signature edSig;
  crypto::ed25519::PublicKey edPub;
  std::memcpy(edSig.data(), sig->signature.data(), 64);
  std::memcpy(edPub.data(), trustedPub.data(), 32);
  if (!crypto::ed25519::verify(edSig, hash->data(), hash->size(), edPub)) {
    setErr("Ed25519 signature verification failed (mod tampered or mis-signed)");
    return SignatureStatus::Invalid;
  }

  return SignatureStatus::Verified;
}

SignatureStatus verifyModSignatureAgainstKey(const std::filesystem::path& modDir,
                                             const std::array<uint8_t, 32>& trustedPub,
                                             std::string* err) {
  return verifyAgainstKey(modDir, trustedPub, "the expected key", err);
}

SignatureStatus verifyModSignature(const std::filesystem::path& modDir, std::string* err) {
  std::array<uint8_t, 32> jdPub;
  if (!bakedKey(jdPub)) {
    if (err) *err = "baked-in public key is malformed (not 64 hex chars) — build misconfigured";
    return SignatureStatus::Invalid;
  }
  return verifyAgainstKey(modDir, jdPub, "JD's baked-in key", err);
}

// The throwaway test key's public half (hex). Anyone holding the matching private
// seed from the old selftest could mint "Verified" mods, so a shipping build must
// never carry this key. The comparison is against the PUBLIC key (not secret).
bool bakedKeyIsTestKey() {
  std::array<uint8_t, 32> jdPub;
  if (!bakedKey(jdPub)) return false;
  std::array<uint8_t, 32> testPub{};
  // This is the public half of the retired test keypair. Public by design.
  crypto::fromHex("3bb44b55eabf04186da2ba95b8d93459eadc59d397815487ac45a27d8a9bb787",
                  testPub.data(), testPub.size());
  return std::memcmp(jdPub.data(), testPub.data(), 32) == 0;
}

}  // namespace litholattice::assets
