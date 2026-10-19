// Certificate definitions + the static progression curve (wiring pass
// 2026-10-13, Team B item B7).
//
// Design sources (do not re-lock anything here):
//  - Certificate signing is LOCKED as a design (Lore Addendum: Certificate
//    Signing & Systerian, 2026-10-12): LithoKraft's enchanting analogue,
//    performed at the certificate table; output is the item with the
//    certificate applied. Terminology fence: certificate table (this) is
//    NOT the mod signature and NOT the Litho asset-provenance certificate.
//  - "Static" is the XP analogue: a player STAT (not a vital), earned from
//    mobs/activities, spent at the table on signing. The stat stores TOTAL
//    static; level is DERIVED through the curve; signing spends LEVELS.
//  - The curve is PROPOSED (Elfie 2026-10-12, NOT locked): per-level cost
//    is a power of two that doubles every 4 levels, base 8. Levels 1-4
//    cost 8, 5-8 cost 16, ..., 29-32 cost 1024. Total to level 16 = 480,
//    total to level 32 = 8160. The helpers below implement exactly that
//    proposal; the knobs (base 8, band width 4) are JD's to change.
//  - Certificate of Clarity: DECIDED (JD) -- crafted at the Certificate
//    Table; fully reveals Demensors. Registered here as a signing
//    definition as well because the table is its home; its crafting
//    recipe is a separate, still-open path (see the lore addendum).
//  - Certificate of Knowledge: DECIDED to exist (JD, 2026-10-13);
//    mechanics unspecified -- level cost below is a conservative
//    placeholder, flagged as such.
//  - Certificate of Efficiency / Mining Haste: the glossary's own "e.g."
//    examples of the Certificate-of-X family; registered so the family
//    is real, with placeholder costs, flagged for JD.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lithokraft {

/// One signable certificate. `id` is the stable string stored on item
/// stacks (player.dat v5 stack-data block); never rename a shipped id.
struct CertificateDef {
  std::string_view id;          ///< e.g. "certificate_of_clarity"
  std::string_view name;        ///< "Certificate of Clarity"
  int levelCost = 1;            ///< static LEVELS spent on signing (curve below)
  std::string_view effectTag;   ///< machine tag for the future effect consumer
  std::string_view note;        ///< provenance / status, shown in listings
};

/// The built-in certificate registry (append-only; ids are save data).
const std::vector<CertificateDef>& certificateRegistry();
const CertificateDef* findCertificate(std::string_view id);

// --- The static progression curve (PROPOSED values, see header) ----------

/// Cost in total-static points of going from level (level-1) to `level`
/// (level >= 1). Band b = (level-1)/4; cost = 8 << b.
int32_t staticLevelCost(int level);
/// Total static required to REACH `level` (sum of levels 1..level).
/// staticTotalForLevel(0) = 0, (16) = 480, (32) = 8160.
int64_t staticTotalForLevel(int level);
/// Level derived from a total: the largest L with totalForLevel(L) <= total.
int staticLevelForTotal(int64_t total);
/// Enchanting-style level spend: returns the new total after paying
/// `levels` levels from `total` (progress inside the current level is
/// preserved), or -1 when the derived level is insufficient.
int64_t staticSpendLevels(int64_t total, int levels);

}  // namespace lithokraft
