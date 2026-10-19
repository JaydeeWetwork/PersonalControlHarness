// Certificate-table signing flow (wiring pass 2026-10-13, Team B item B7).
//
// Locked design (Lore Addendum: Certificate Signing & Systerian,
// 2026-10-12): signing is LithoKraft's enchanting analogue, performed at
// the certificate table -- spend the "static" stat, apply a "Certificate
// of X" to an item. The stat stores TOTAL static; the level is derived
// through the PROPOSED progression curve and signing spends LEVELS
// (content/certificates.h carries the curve and the definitions).
//
// This module is the mechanic core the design's game-side entry point
// (working name in components/certificate_table.cpp: openSigning) binds
// to: the console `sign` command calls it today; JD's table UI will call
// it when designed. Table-presence gating binds with that UI -- the
// sim-side use() path cannot reach the player inventory (see
// certificate_table.cpp for the seam analysis), so nothing here fakes a
// proximity check the architecture cannot yet honour.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include "lithokraft/game/inventory.h"

namespace lithokraft {

/// Sign ONE item of the stack in flat slot `flatSlot` (Inventory::allSlots
/// order) with certificate `certId`, spending the certificate's level
/// cost from `totalStatic` (updated in place on success). A stack of
/// more than one splits: the signed item becomes its own 1-count stack.
/// Fail-closed -- unknown certificate, empty/out-of-range slot, the
/// certificate already applied to that stack, insufficient derived
/// level, or no room for the split: nothing changes, `err` says why.
bool signStack(Inventory& inv, int flatSlot, std::string_view certId,
               int32_t& totalStatic, std::string* err);

}  // namespace lithokraft
