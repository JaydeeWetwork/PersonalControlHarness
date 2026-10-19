// Etch flow implementation. See etching.h for the design contract.
#include "lithokraft/game/etching.h"

#include "lithokraft/content/blocks.h"  // kWaferSilicon, kChipOR
#include "lithokraft/content/items.h"   // findItemByStringId

namespace lithokraft {

uint32_t etchInputItemId() { return uint32_t(kWaferSilicon); }

uint32_t etchedChipItemId() {
  // The ChipOR item is block-backed: its itemId IS the kChipOR BlockId
  // (items.cpp). Resolve through the item registry so a future split of
  // item/block ids is absorbed here, not at call sites.
  if (const ItemDef* d = findItemByStringId("lithokraft:chipor")) return d->itemId;
  return uint32_t(kChipOR);
}

bool etchChip(Inventory& inv, std::string_view nameField, std::string* err) {
  auto fail = [err](const std::string& msg) {
    if (err) *err = msg;
    return false;
  };
  // The name field is one line of identity text: refuse control bytes
  // and over-cap names outright (never truncate a chip's name).
  if (nameField.size() > kEtchNameMaxBytes)
    return fail("etch: name field is " + std::to_string(nameField.size()) + " bytes (max " +
                std::to_string(kEtchNameMaxBytes) + "); refused, not truncated");
  for (const char c : nameField)
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
      return fail("etch: name field carries control characters; refused");

  const uint32_t inId = etchInputItemId();
  if (inv.countOf(inId) == 0) return fail("etch: no blank wafer (Wafer Silicon) in inventory");

  ItemStack out;
  out.itemId = etchedChipItemId();
  out.count = 1;
  out.customName = std::string(nameField);  // empty name field -> plain chip

  if (!inv.canPlaceStack(out)) {
    // Edge: a full inventory whose only wafer stack is a single wafer --
    // consuming it frees the slot the chip needs. Try the consume first,
    // and restore the wafer if the chip still cannot be placed.
    bool singleWafer = false;
    for (const auto* s : inv.allSlots())
      if (s->itemId == inId && s->count == 1) { singleWafer = true; break; }
    if (!singleWafer) return fail("etch: no room for the etched chip");
    inv.removeItem(inId, 1);
    if (inv.placeStack(out)) return true;
    inv.addItem(inId, 1);  // restore: the wafer is fungible, the etch did not happen
    return fail("etch: no room for the etched chip");
  }
  if (inv.removeItem(inId, 1) != 1) return fail("etch: blank wafer vanished mid-etch; nothing consumed");
  if (!inv.placeStack(out)) {  // cannot happen after canPlaceStack, but stay atomic if it does
    inv.addItem(inId, 1);
    return fail("etch: no room for the etched chip");
  }
  return true;
}

}  // namespace lithokraft
