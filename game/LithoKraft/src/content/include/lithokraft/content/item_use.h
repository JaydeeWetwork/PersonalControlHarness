// Per-item use dispatch (brief B1a / FRAGMENT-UI-NEEDS item 4, A3 wiring
// 2026-10-13): the general production seam for "use the selected item on
// the targeted cell" — for ALL items, not a Fragment-only verb (the brief
// explicitly forbids a Fragment-only verb that pre-empts this seam).
//
// Items are otherwise registry data + models + icons (items.h). A handler
// is registered per itemId for behavior ALREADY DEFINED in code/docs;
// dispatch for an item with no handler FAILS CLOSED: it logs and returns
// Failed, never a silent fake success. Handlers run on the SIM thread
// (SignalWorldSimulation::command, kCmdItemUse) because they mutate the
// world; Game::handleWorldActions routes the player's Use press there
// through SimThread::edit, exactly like block Use.
#pragma once
#include <cstdint>
#include "litholattice/sim/world_simulation.h"  // IWorldSimulation::DirtyFn
#include "litholattice/world/chunk_map.h"

namespace lithokraft {

enum class ItemUseResult : uint8_t { Handled, Failed };

struct ItemUseContext {
  litholattice::ChunkMap& world;
  int x = 0, y = 0, z = 0;  ///< targeted cell (the looked-at block)
  const litholattice::IWorldSimulation::DirtyFn& dirty;
};

using ItemUseHandler = std::function<ItemUseResult(const ItemUseContext&)>;

/// Registers a use handler for `itemId`. First registration wins (the
/// content-registry rule): a duplicate is logged and ignored.
void registerItemUseHandler(uint32_t itemId, ItemUseHandler handler);
/// True when a handler (including an explicit fail-closed stub) exists.
bool hasItemUseHandler(uint32_t itemId);
/// Dispatches a use of `itemId` at the context's target cell. No handler
/// (or a handler reporting failure) returns Failed; the no-handler case
/// is logged as fail-closed. Never throws, never fakes success.
ItemUseResult dispatchItemUse(uint32_t itemId, const ItemUseContext& ctx);

}  // namespace lithokraft
