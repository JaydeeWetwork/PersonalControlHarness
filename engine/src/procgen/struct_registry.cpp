// StructureRegistry implementation: validation + identity + signing.
//
// Identity: every entry gets a SHA-256 content fingerprint at registration
// (see computeFingerprint for the byte-exact spec). Signing: optional Ed25519
// over the raw fingerprint bytes, mirroring the mod-signing trust model —
// Trusted means the signature verifies against JD's baked-in public key,
// which the game installs via setTrustedPublicKeyHex() at boot.
#include "litholattice/procgen/struct_registry.h"

#include <algorithm>
#include <cstring>

#include "litholattice/core/log.h"
#include "litholattice/crypto/ed25519.h"
#include "litholattice/crypto/sha256.h"
#include "litholattice/crypto/signing.h"  // toHex / fromHex

namespace litholattice::procgen {
namespace {

void putU16LE(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(uint8_t(v));
  out.push_back(uint8_t(v >> 8));
}
void putU32LE(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v));
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v >> 16));
  out.push_back(uint8_t(v >> 24));
}
void putI32LE(std::vector<uint8_t>& out, int32_t v) {
  putU32LE(out, static_cast<uint32_t>(v));
}
uint32_t floatBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}

bool validSegment(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

}  // namespace

const char* structureTrustName(StructureTrust t) {
  switch (t) {
    case StructureTrust::Unsigned: return "Unsigned";
    case StructureTrust::Trusted: return "Trusted";
    case StructureTrust::SignedByUnknownKey: return "SignedByUnknownKey";
    case StructureTrust::Invalid: return "Invalid";
  }
  return "?";
}

StructureRegistry& StructureRegistry::instance() {
  static StructureRegistry reg;
  return reg;
}

bool StructureRegistry::isValidName(std::string_view name) {
  const size_t colon = name.find(':');
  if (colon == std::string_view::npos) return false;
  if (name.find(':', colon + 1) != std::string_view::npos) return false;  // exactly one
  return validSegment(name.substr(0, colon)) && validSegment(name.substr(colon + 1));
}

std::string StructureRegistry::computeFingerprint(const std::vector<StructureVariant>& variants) {
  struct Cell {
    int32_t dx, dy, dz;
    uint16_t material;
    uint32_t decayBits;
  };
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < variants.size(); ++i) {
    const auto& v = variants[i];
    putU32LE(bytes, static_cast<uint32_t>(i));
    putU32LE(bytes, floatBits(v.weight));
    std::vector<Cell> cells;
    cells.reserve(v.cells.size());
    for (const auto& c : v.cells) {
      cells.push_back({c.dx, c.dy, c.dz, c.material, floatBits(c.decay)});
    }
    std::sort(cells.begin(), cells.end(), [](const Cell& a, const Cell& b) {
      if (a.dx != b.dx) return a.dx < b.dx;
      if (a.dy != b.dy) return a.dy < b.dy;
      if (a.dz != b.dz) return a.dz < b.dz;
      if (a.material != b.material) return a.material < b.material;
      return a.decayBits < b.decayBits;
    });
    for (const auto& c : cells) {
      putI32LE(bytes, c.dx);
      putI32LE(bytes, c.dy);
      putI32LE(bytes, c.dz);
      putU16LE(bytes, c.material);
      putU32LE(bytes, c.decayBits);
    }
  }
  crypto::Sha256 s;
  if (!bytes.empty()) s.update(bytes.data(), bytes.size());
  return crypto::toHex(s.final().data(), 32);
}

bool StructureRegistry::verifySignatureBytes(const StructureEntry::Signature& sig,
                                             const std::string& fingerprintHex) {
  std::array<uint8_t, 32> pub{};
  std::array<uint8_t, 64> signature{};
  std::array<uint8_t, 32> fpBytes{};
  if (!crypto::fromHex(sig.publicKeyHex, pub.data(), pub.size())) return false;
  if (!crypto::fromHex(sig.signatureHex, signature.data(), signature.size())) return false;
  if (!crypto::fromHex(fingerprintHex, fpBytes.data(), fpBytes.size())) return false;
  crypto::ed25519::PublicKey pubKey;
  crypto::ed25519::Signature edSig;
  std::copy(pub.begin(), pub.end(), pubKey.begin());
  std::copy(signature.begin(), signature.end(), edSig.begin());
  return crypto::ed25519::verify(edSig, fpBytes.data(), fpBytes.size(), pubKey);
}

bool StructureRegistry::registerEntry(StructureEntry e, const StructureEntry::Signature* sig,
                                      std::string* err) {
  return registerEntryImpl(std::move(e), sig, err, false);
}
bool StructureRegistry::registerInGame(StructureEntry e, std::string* err) {
  return registerEntryImpl(std::move(e), nullptr, err, true);
}
bool StructureRegistry::registerEntryImpl(StructureEntry e, const StructureEntry::Signature* sig,
                                          std::string* err, bool bypassSeal) {
  auto fail = [&](const char* reason) {
    if (err) *err = reason;
    logError("structreg: refused '%s': %s", e.name.c_str(), reason);
    return false;
  };
  if (!bypassSeal && sealed_) return fail("registry is sealed");
  if (!isValidName(e.name)) return fail("malformed name (want namespace:name, lowercase a-z0-9_)");
  if (e.variants.empty()) return fail("no variants");
  for (const auto& v : e.variants) {
    if (v.cells.empty() || !(v.weight > 0.f)) return fail("variant has no cells or bad weight");
    for (const auto& c : v.cells) {
      // Bound coordinates (same limits as the JSON loader): unbounded values
      // overflow reach_ computation (std::abs(INT_MIN) is UB).
      if (c.dx < -512 || c.dx > 512 || c.dz < -512 || c.dz > 512 || c.dy < -1024 || c.dy > 1024)
        return fail("variant cell coordinates out of bounds (±512, dy ±1024)");
    }
  }
  if (index_.find(e.name) != index_.end()) return fail("name already registered");

  e.fingerprintHex = computeFingerprint(e.variants);
  if (sig) {
    e.signature = *sig;
    if (!verifySignatureBytes(*sig, e.fingerprintHex)) {
      return fail("signature invalid for this blueprint (fail closed)");
    }
  }

  const size_t i = entries_.size();
  index_.emplace(e.name, i);
  if (fingerprintIndex_.find(e.fingerprintHex) == fingerprintIndex_.end()) {
    fingerprintIndex_.emplace(e.fingerprintHex, i);  // first name wins per fingerprint
  }
  const bool wasUnsigned = !e.signature.has_value();
  entries_.push_back(std::move(e));
  if (wasUnsigned) {
    logInfo("structreg: '%s' registered unsigned (fingerprint %s)", entries_[i].name.c_str(),
                  entries_[i].fingerprintHex.c_str());
  } else {
    logInfo("structreg: '%s' registered with signature (fingerprint %s)",
                  entries_[i].name.c_str(), entries_[i].fingerprintHex.c_str());
  }
  return true;
}

const StructureEntry* StructureRegistry::find(std::string_view name) const {
  const auto it = index_.find(std::string(name));
  return it == index_.end() ? nullptr : &entries_[it->second];
}

const StructureEntry* StructureRegistry::findByFingerprint(std::string_view fingerprintHex) const {
  const auto it = fingerprintIndex_.find(std::string(fingerprintHex));
  return it == fingerprintIndex_.end() ? nullptr : &entries_[it->second];
}

std::vector<std::string> StructureRegistry::list() const {
  std::vector<std::string> out;
  out.reserve(entries_.size());
  for (const auto& e : entries_) out.push_back(e.name);
  return out;
}

std::vector<const StructureEntry*> StructureRegistry::findByNamespace(std::string_view ns) const {
  std::vector<const StructureEntry*> out;
  for (const auto& e : entries_) {
    const size_t colon = e.name.find(':');
    if (colon != std::string::npos && e.name.compare(0, colon, ns) == 0) out.push_back(&e);
  }
  return out;
}

std::vector<const StructureEntry*> StructureRegistry::findByPrefix(std::string_view prefix) const {
  std::vector<const StructureEntry*> out;
  for (const auto& e : entries_) {
    if (e.name.compare(0, prefix.size(), prefix) == 0) out.push_back(&e);
  }
  return out;
}

StructureTrust StructureRegistry::verifyEntry(std::string_view name) const {
  const StructureEntry* e = find(name);
  if (!e || !e->signature.has_value()) return StructureTrust::Unsigned;
  if (!verifySignatureBytes(*e->signature, e->fingerprintHex)) return StructureTrust::Invalid;
  if (!hasTrustedKey_) return StructureTrust::SignedByUnknownKey;
  std::array<uint8_t, 32> pub{};
  if (!crypto::fromHex(e->signature->publicKeyHex, pub.data(), pub.size())) {
    return StructureTrust::Invalid;  // unreachable (verified above), defensive
  }
  return pub == trustedKey_ ? StructureTrust::Trusted : StructureTrust::SignedByUnknownKey;
}

void StructureRegistry::setTrustedPublicKeyHex(std::string hex) {
  std::array<uint8_t, 32> key{};
  if (crypto::fromHex(hex, key.data(), key.size())) {
    trustedKey_ = key;
    hasTrustedKey_ = true;
  } else {
    logWarn("structreg: ignored malformed trusted-key hex");
  }
}

void StructureRegistry::resetForTests() {
  index_.clear();
  fingerprintIndex_.clear();
  entries_.clear();
  trustedKey_.fill(0);
  hasTrustedKey_ = false;
  sealed_ = false;
}

}  // namespace litholattice::procgen
