// Engine-level structure registry: named, reusable structure blueprints.
//
// JD's design tool loop: build a structure in-game, capture it with the structure
// block (SAVE mode), register it under a name ("jd:watchtower"). Worldgen JSON
// references it by name ("structure": "jd:watchtower") instead of pasting inline
// blueprint cells. Mods register their own via registerStructure() during mod init.
//
// Two-door registration contract:
//   - Mods register via registerStructure() during mod init, before the game
//     seals the registry at boot (same seal contract as ContentRegistry).
//   - JD's in-game build loop (structure block SAVE) uses registerInGame(),
//     the intentional post-boot door. Worldgen resolves names at pipeline
//     build time and copies variants, so a SAVE never mutates a live pipeline.
#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "litholattice/procgen/stages.h"  // BlueprintCell, AnchorMode

namespace litholattice::procgen {

/// One placement variant: a weighted blueprint. A registry entry holds a pool;
/// the placer rolls which variant per cell (deterministic, position-keyed).
struct StructureVariant {
  std::vector<BlueprintCell> cells;
  float weight = 1.f;  ///< selection weight within the entry's pool; must be > 0
};

/// Data-driven placement rules. From worldgen JSON or the mod API; the
/// StructureFeature merges these with its per-feature StructureSettings
/// (explicit JSON fields win over the registry entry's rules).
struct StructurePlacementRules {
  float spacing = 0.f;  ///< min distance between placed bases (separation)
  AnchorMode anchor = AnchorMode::Surface;
  int fixedY = 0;       ///< base Y when anchor == FixedY
  int hoverHeight = 0;  ///< extra lift above the surface when anchor == Floating
  float minSlope = 0.f;          ///< reject when terrain gradient < minSlope
  float maxSlope = 1e9f;         ///< reject when terrain gradient > maxSlope
  int minHeight = std::numeric_limits<int>::min();  ///< reject when surface Y below
  int maxHeight = std::numeric_limits<int>::max();  ///< reject when surface Y above
  bool nearWater = false;  ///< require water within waterRadius of the base
  int waterRadius = 8;     ///< search radius (blocks) for nearWater
  int seaLevel = std::numeric_limits<int>::min();  ///< water surface Y; min() = no water
};

/// A named structure: variant pool + placement rules.
struct StructureEntry {
  std::string name;  ///< namespaced, unique: "jd:watchtower" (validated: ^[a-z0-9_]+:[a-z0-9_]+$)
  std::vector<StructureVariant> variants;
  StructurePlacementRules rules;
  /// SHA-256 (64 lowercase hex chars) of the canonical blueprint. Computed by
  /// registerEntry() via computeFingerprint(); same content -> same fingerprint,
  /// so content dedup comes free. Never set by hand.
  std::string fingerprintHex;
  /// Optional Ed25519 signature over the raw 32 fingerprint bytes. Unsigned
  /// entries are fine (personal/JD-unsigned use); see StructureTrust.
  struct Signature {
    std::string publicKeyHex;  ///< 64 hex chars: the 32-byte Ed25519 public key
    std::string signatureHex;  ///< 128 hex chars: Ed25519 over the fingerprint bytes
  };
  std::optional<Signature> signature;
};

/// Trust outcome for a registered structure. Mirrors the mod-signing model:
/// the signature answers *who vouches for this structure*, not what it can do.
enum class StructureTrust {
  Unsigned,               ///< no signature; loads, labeled as such in logs
  Trusted,                ///< signature valid AND key == the trusted (JD) public key
  SignedByUnknownKey,     ///< signature valid but the key isn't JD's (self-signed etc.)
  Invalid,                ///< signature present but malformed or fails verification
};

/// Human-readable name for logs.
const char* structureTrustName(StructureTrust t);

/// Engine-level structure registry (process-wide singleton).
class StructureRegistry {
 public:
  static StructureRegistry& instance();

  /// Register an entry. Computes the fingerprint, validates the name
  /// (^[a-z0-9_]+:[a-z0-9_]+$), and — when `sig` is given — verifies the
  /// signature against the fingerprint before accepting.
  ///
  /// Returns false (with *err set when non-null) when sealed, the name is
  /// malformed/empty/taken, there are no variants (or a variant has no cells /
  /// non-positive weight), or the signature is invalid. First registration
  /// wins — never a silent overwrite. Invalid signature -> refused, fail closed.
  bool registerEntry(StructureEntry e, const StructureEntry::Signature* sig = nullptr,
                     std::string* err = nullptr);
  /// Lookup by name; nullptr when unknown.
  const StructureEntry* find(std::string_view name) const;
  /// Lookup by content fingerprint (64 hex chars); nullptr when unknown.
  /// First-registered entry wins when several names share a fingerprint.
  const StructureEntry* findByFingerprint(std::string_view fingerprintHex) const;
  /// All registered names, in registration order. Powers the structure block's
  /// LOAD UI and any future structure browser.
  std::vector<std::string> list() const;
  /// Entries whose namespace (before the ':') equals `ns`.
  std::vector<const StructureEntry*> findByNamespace(std::string_view ns) const;
  /// Entries whose full name starts with `prefix`.
  std::vector<const StructureEntry*> findByPrefix(std::string_view prefix) const;
  /// Trust check for a registered entry. Unsigned -> Unsigned; bad signature ->
  /// Invalid; good signature + key == trusted key -> Trusted; good signature +
  /// other key -> SignedByUnknownKey. Unknown name -> Unsigned.
  StructureTrust verifyEntry(std::string_view name) const;
  /// Install the trusted public key (64 hex chars). The game calls this at boot
  /// with the baked-in JD key (see assets/mod_signature.h kBakedPublicKeyHex);
  /// procgen stays decoupled from the assets module. Until set, nothing can be
  /// Trusted (fail closed). Malformed hex is ignored.
  void setTrustedPublicKeyHex(std::string hex);
  /// Name rule: exactly one ':', lowercase a-z0-9_ on both sides, non-empty.
  static bool isValidName(std::string_view name);
  /// Canonical content fingerprint (64 lowercase hex chars). Byte-exact spec:
  /// for each variant i in order: u32LE(i), u32LE(bit pattern of weight as
  /// float); then that variant's cells sorted by (dx, dy, dz, material, decay
  /// bits), each as i32LE(dx), i32LE(dy), i32LE(dz), u16LE(material),
  /// u32LE(bit pattern of decay as float). SHA-256 over the byte stream.
  static std::string computeFingerprint(const std::vector<StructureVariant>& variants);
  /// In-game door for the structure block SAVE path: same validation as
  /// registerEntry, but bypasses the boot seal (JD's build loop runs post-boot).
  /// Call only from the game thread between ticks.
  bool registerInGame(StructureEntry e, std::string* err = nullptr);
  /// Seal: mod-init registrations refuse afterwards. Irreversible. The game
  /// seals after mod init, before any world generates. registerInGame() is
  /// unaffected (that's the documented carve-out, not a hole).
  void seal() { sealed_ = true; }
  bool isSealed() const { return sealed_; }
  size_t size() const { return entries_.size(); }
  /// Test hook: clears everything and unseals. Tests only — never production.
  void resetForTests();

 private:
  StructureRegistry() = default;
  bool registerEntryImpl(StructureEntry e, const StructureEntry::Signature* sig, std::string* err,
                         bool bypassSeal);
  static bool verifySignatureBytes(const StructureEntry::Signature& sig,
                                   const std::string& fingerprintHex);
  std::unordered_map<std::string, size_t> index_;
  std::unordered_map<std::string, size_t> fingerprintIndex_;  ///< fp hex -> first entry
  std::vector<StructureEntry> entries_;
  std::array<uint8_t, 32> trustedKey_{};  ///< JD's public key; all-zero = unset
  bool hasTrustedKey_ = false;
  bool sealed_ = false;
};

/// Mod API: register a named structure during mod init (before the registry seals).
/// Returns false when sealed or the name is taken.
inline bool registerStructure(StructureEntry e) {
  return StructureRegistry::instance().registerEntry(std::move(e));
}

}  // namespace litholattice::procgen
