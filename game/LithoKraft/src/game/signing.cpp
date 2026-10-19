// Signing flow implementation. See signing.h for the design contract
// and content/certificates.h for definitions + the PROPOSED curve.
#include "lithokraft/game/signing.h"

#include <algorithm>

#include "lithokraft/content/certificates.h"

namespace lithokraft {

bool signStack(Inventory& inv, int flatSlot, std::string_view certId,
               int32_t& totalStatic, std::string* err) {
  auto fail = [err](const std::string& msg) {
    if (err) *err = msg;
    return false;
  };
  const CertificateDef* def = findCertificate(certId);
  if (!def) return fail("sign: unknown certificate '" + std::string(certId) + "'");
  if (flatSlot < 0 || flatSlot >= kTotalSlots) return fail("sign: slot out of range");
  ItemStack& slot = inv.slotAt(flatSlot);
  if (slot.empty()) return fail("sign: the selected slot is empty");
  if (std::find(slot.certificates.begin(), slot.certificates.end(), def->id) != slot.certificates.end())
    return fail(std::string("sign: ") + std::string(def->name) + " is already applied to that item");

  const int level = staticLevelForTotal(totalStatic);
  if (level < def->levelCost)
    return fail("sign: " + std::string(def->name) + " costs " + std::to_string(def->levelCost) +
                " levels; you are level " + std::to_string(level) + " (" +
                std::to_string(totalStatic) + " static)");
  const int64_t newTotal = staticSpendLevels(totalStatic, def->levelCost);
  if (newTotal < 0) return fail("sign: insufficient static");  // unreachable after the level check; fail closed anyway

  if (slot.count == 1) {
    slot.certificates.emplace_back(def->id);
    totalStatic = int32_t(newTotal);
    return true;
  }
  // Split one item off the stack and sign the single. Check room BEFORE
  // mutating anything (fail-closed, atomic).
  ItemStack single;
  single.itemId = slot.itemId;
  single.count = 1;
  single.customName = slot.customName;
  single.certificates = slot.certificates;
  single.certificates.emplace_back(def->id);
  if (!inv.canPlaceStack(single)) return fail("sign: no room to split the signed item out of its stack");
  slot.count = uint16_t(slot.count - 1);
  if (!inv.placeStack(single)) {  // cannot happen after canPlaceStack; stay atomic
    slot.count = uint16_t(slot.count + 1);
    return fail("sign: no room to split the signed item out of its stack");
  }
  totalStatic = int32_t(newTotal);
  return true;
}

}  // namespace lithokraft
