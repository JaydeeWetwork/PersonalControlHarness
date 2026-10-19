// Per-item use dispatch: registry + the handlers whose behavior is
// already defined in code/docs (A3 wiring, 2026-10-13). See item_use.h.
#include "lithokraft/content/item_use.h"
#include <mutex>
#include <unordered_map>
#include "litholattice/core/log.h"
#include "lithokraft/content/items.h"
#include "lithokraft/content/silica.h"

namespace lithokraft {
namespace {

std::unordered_map<uint32_t, ItemUseHandler>& handlers() {
  static std::unordered_map<uint32_t, ItemUseHandler> m;
  return m;
}

void registerDefaults() {
  // NOTE: inserts DIRECTLY into the map. Going through the public
  // registerItemUseHandler() here would re-enter ensureDefaults()'s
  // std::call_once on the same flag — a self-deadlock on first dispatch
  // (found the hard way: froze the selftest suite, 2026-10-13).
  auto& m = handlers();
  // SeedOR Till (items.h ToolAction::Till; silica.h silicaTill): tills
  // silicon soil into farmland. Dead silicon refuses (silicaTill's own
  // rule) and the use simply fails — a normal gameplay miss, no log.
  m[kItemSeedor] = [](const ItemUseContext& c) {
    return silicaTill(c.world, c.x, c.y, c.z, c.dirty) ? ItemUseResult::Handled : ItemUseResult::Failed;
  };
  // Silica seed planting (silica.h silicaCanPlant/silicaDoPlant): using
  // the seed on tilled soil plants in the cell above it.
  m[kItemSilicaSeed] = [](const ItemUseContext& c) {
    if (!silicaCanPlant(c.world, c.x, c.y + 1, c.z)) return ItemUseResult::Failed;
    silicaDoPlant(c.world, c.x, c.y + 1, c.z, c.dirty);
    return ItemUseResult::Handled;
  };
  // The Fragment: its use behavior IS the book's open verb, which is JD
  // decision C3 #1 (dedicated key / use selected item / both) — and the
  // book dialog itself does not exist yet. Explicit fail-closed stub:
  // logged as decision-pending, never an invented behavior.
  m[kItemFragment] = [](const ItemUseContext&) {
    litholattice::logWarn("item-use: the Fragment's use is decision-pending (JD C3: open verb not chosen); failing closed");
    return ItemUseResult::Failed;
  };
  // Every other registered item (BashOR, PickOR, ChopOR, HeapOR, SlicOR,
  // DebuggOR, Sniffor, ...) intentionally has NO handler: no world-
  // interaction behavior is defined for them in code/docs yet (brief
  // B1a), so dispatch fails closed below instead of inventing one.
}

void ensureDefaults() {
  static std::once_flag once;
  std::call_once(once, registerDefaults);
}

}  // namespace

void registerItemUseHandler(uint32_t itemId, ItemUseHandler handler) {
  ensureDefaults();
  if (!handlers().emplace(itemId, std::move(handler)).second)
    litholattice::logWarn("item-use: duplicate handler for item %u ignored (first wins)", itemId);
}

bool hasItemUseHandler(uint32_t itemId) {
  ensureDefaults();
  return handlers().count(itemId) != 0;
}

ItemUseResult dispatchItemUse(uint32_t itemId, const ItemUseContext& ctx) {
  ensureDefaults();
  const auto it = handlers().find(itemId);
  if (it == handlers().end()) {
    litholattice::logWarn("item-use: no handler registered for item %u; failing closed", itemId);
    return ItemUseResult::Failed;
  }
  return it->second(ctx);
}

}  // namespace lithokraft
