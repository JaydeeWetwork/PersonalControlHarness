// Certificate registry + static curve math. See certificates.h for the
// design provenance of every value here (LOCKED vs PROPOSED vs placeholder
// is recorded per entry; nothing in this file re-locks a proposal).
#include "lithokraft/content/certificates.h"

namespace lithokraft {

const std::vector<CertificateDef>& certificateRegistry() {
  static const std::vector<CertificateDef> kCerts = {
      // DECIDED (JD): the Certificate Table crafts the Certificate of
      // Clarity; it fully reveals Demensors. Cost 3 levels is the
      // conservative signing cost until JD sets real numbers.
      {"certificate_of_clarity", "Certificate of Clarity", 3, "reveals_demensors",
       "DECIDED (JD 2026-10-12): table-crafted; fully reveals Demensors. Signing cost is a placeholder."},
      // DECIDED to exist (JD 2026-10-13); mechanics/cost unspecified --
      // 5 levels is a conservative placeholder, flagged for JD.
      {"certificate_of_knowledge", "Certificate of Knowledge", 5, "knowledge",
       "DECIDED to exist (JD 2026-10-13); cost/effect unspecified -- placeholder."},
      // The glossary's own examples of the Certificate-of-X family
      // (LOCKED signing design, 2026-10-12). Placeholder costs.
      {"certificate_of_efficiency", "Certificate of Efficiency", 2, "efficiency",
       "Glossary example of the family; placeholder cost."},
      {"certificate_of_mining_haste", "Certificate of Mining Haste", 2, "mining_haste",
       "Glossary example of the family; placeholder cost."},
  };
  return kCerts;
}

const CertificateDef* findCertificate(std::string_view id) {
  for (const CertificateDef& d : certificateRegistry())
    if (d.id == id) return &d;
  return nullptr;
}

int32_t staticLevelCost(int level) {
  if (level < 1) level = 1;
  const int band = (level - 1) / 4;  // doubles every 4 levels, base 8
  // 8 << band overflows int32 at band 28 (level 113); static itself is an
  // int32 stat, so totals can never reach such levels -- clamp the shift
  // to the largest band whose cost still fits comfortably in int64 math.
  if (band > 24) return INT32_MAX;
  return int32_t(8) << band;
}

int64_t staticTotalForLevel(int level) {
  if (level <= 0) return 0;
  int64_t total = 0;
  for (int l = 1; l <= level; ++l) total += staticLevelCost(l);
  return total;
}

int staticLevelForTotal(int64_t total) {
  if (total <= 0) return 0;
  int level = 0;
  // Totals are bounded by the int32 stat range, so this loop runs at
  // most ~90 iterations; it is called on spend/list, never per tick.
  while (staticTotalForLevel(level + 1) <= total) ++level;
  return level;
}

int64_t staticSpendLevels(int64_t total, int levels) {
  if (levels < 0) return -1;
  if (levels == 0) return total;
  const int level = staticLevelForTotal(total);
  if (level < levels) return -1;
  const int64_t levelFloor = staticTotalForLevel(level);
  const int64_t progressInLevel = total - levelFloor;  // kept (enchanting-style)
  return staticTotalForLevel(level - levels) + progressInLevel;
}

}  // namespace lithokraft
