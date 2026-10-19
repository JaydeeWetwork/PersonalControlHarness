// lithokraft_items_selftest: headless item-system tests (no window, no GL).
// Items workstream (2026-10-12): the lore hand tools / weapons as FULL items
// -- registry integrity, append-only ids, the tool/weapon split, the ChopOR
// tier-1 rule, .lthm models on disk, and the acceptance check: a fresh
// spawn inventory can hold every lore tool, with the Fragment granted as
// the first-tick starting item. Exit code = failure count. LF endings.
#include <cstdio>
#include <string>
#include <unordered_set>

#include "lithokraft/content/blocks.h"
#include "lithokraft/content/items.h"
#include "lithokraft/game/inventory.h"
#include "model.h"  // lithokraft_model: loadModel for the .lthm check

namespace {
int g_fails = 0;
void check(bool ok, const std::string& name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
  if (!ok) ++g_fails;
}
}  // namespace

int main() {
  using namespace lithokraft;
  using lithokraft::ItemDef;  // the engine also has litholattice::ItemDef (block.h)

  // 1. Registry integrity: unique ids + string ids, every def complete.
  {
    std::unordered_set<uint32_t> ids;
    std::unordered_set<std::string> sids;
    bool ok = true;
    for (const ItemDef& d : itemRegistry()) {
      ok = ok && ids.insert(d.itemId).second && sids.insert(std::string(d.stringId)).second;
      ok = ok && d.itemId != 0 && !d.stringId.empty() && !d.name.empty() && !d.iconName.empty();
      ok = ok && findItem(d.itemId) == &d && findItemByStringId(d.stringId) == &d;
      ok = ok && isItemId(d.itemId);
    }
    check(ok, "registry: ids/string ids unique, defs complete, lookups round-trip");
    check(itemRegistry().size() == 31, "registry: 31 items (seed, Fragment, 6 armory, Firewall, Ping, Callback, DebuggOR, Sniffor, Atlas, Titanium Oxide, 15 JD materials, ChipOR)");
  }

  // 1b. Unified ContentRegistry: every item is also registered under the
  //     engine's item kind (Infinite Registry phase 2), ChipOR places its block.
  {
    const ContentRegistry& content = blockRegistry().content();
    bool ok = content.itemCount() == itemRegistry().size();
    for (const ItemDef& d : itemRegistry()) {
      const auto id = content.findItem(d.stringId);
      ok = ok && id && content.itemDef(*id).name == d.name &&
           content.itemDef(*id).maxStack == uint32_t(d.maxStack);
    }
    const auto chip = content.findItem("lithokraft:chipor");
    ok = ok && chip && content.itemDef(*chip).placesBlock == content.blockStableId(kChipOR) &&
         content.itemDef(*chip).placesBlock != 0;
    check(ok, "unified registry: all 31 items registered as engine items; ChipOR placesBlock = ChipOR block stable id");
  }

  // 2. Append-only ids are exactly the assigned offsets (never renumbered).
  check(kItemSilicaSeed == kFirstItemId + 0 && kItemFragment == kFirstItemId + 1 &&
            kItemBashor == kFirstItemId + 2 && kItemPickor == kFirstItemId + 3 &&
            kItemChopor == kFirstItemId + 4 && kItemHeapor == kFirstItemId + 5 &&
            kItemSeedor == kFirstItemId + 6 && kItemSlicor == kFirstItemId + 7 &&
            kItemFirewall == kFirstItemId + 8 && kItemPing == kFirstItemId + 9 &&
            kItemCallback == kFirstItemId + 10 && kItemDebuggor == kFirstItemId + 11 &&
            kItemSniffor == kFirstItemId + 12 && kItemAtlas == kFirstItemId + 13 &&
            kItemTitaniumOxide == kFirstItemId + 14 && kItemSilicon == kFirstItemId + 15 &&
            kItemCarbide == kFirstItemId + 16 && kItemSiliconCarbide == kFirstItemId + 17 &&
            kItemGallium == kFirstItemId + 18 && kItemSiliconGallium == kFirstItemId + 19 &&
            kItemNitride == kFirstItemId + 20 && kItemSiliconNitride == kFirstItemId + 21 &&
            kItemDioxide == kFirstItemId + 22 && kItemSiliconDioxide == kFirstItemId + 23 &&
            kItemCopper == kFirstItemId + 24 && kItemSiliconCopper == kFirstItemId + 25 &&
            kItemAluminum == kFirstItemId + 26 && kItemSiliconAluminum == kFirstItemId + 27 &&
            kItemPhotoresist == kFirstItemId + 28 && kItemResist == kFirstItemId + 29,
        "ids: append-only offsets stable (seed +0 .. resist +29, all above the BlockId range)");
  check(!isItemId(0) && !isItemId(kFirstItemId + 99) && isItemId(kWaferSilicon),
        "ids: 0 and unregistered offsets rejected; live BlockIds are block items");

  // 2b. JD materials shelf (2026-10-14): every new material resolves by
  //     numeric id AND string id, name round-trips, and carries the plain
  //     material profile (stacks to 512, no tool/weapon role, no model,
  //     not a starting item, places nothing).
  {
    struct Row { uint32_t id; const char* sid; const char* name; };
    const Row rows[] = {
        {kItemSilicon, "lithokraft:silicon_ingot", "Silicon"},
        {kItemCarbide, "lithokraft:carbide", "Carbide"},
        {kItemSiliconCarbide, "lithokraft:silicon_carbide", "Silicon Carbide"},
        {kItemGallium, "lithokraft:gallium", "Gallium"},
        {kItemSiliconGallium, "lithokraft:silicon_gallium", "Silicon Gallium"},
        {kItemNitride, "lithokraft:nitride", "Nitride"},
        {kItemSiliconNitride, "lithokraft:silicon_nitride", "Silicon Nitride"},
        {kItemDioxide, "lithokraft:dioxide", "Dioxide"},
        {kItemSiliconDioxide, "lithokraft:silicon_dioxide", "Silicon Dioxide"},
        {kItemCopper, "lithokraft:copper", "Copper"},
        {kItemSiliconCopper, "lithokraft:silicon_copper", "Silicon Copper"},
        {kItemAluminum, "lithokraft:aluminum", "Aluminum"},
        {kItemSiliconAluminum, "lithokraft:silicon_aluminum", "Silicon Aluminum"},
        {kItemPhotoresist, "lithokraft:photoresist", "Photoresist"},
        {kItemResist, "lithokraft:resist", "Resist"},
    };
    bool ok = true;
    for (const Row& r : rows) {
      const ItemDef* d = findItem(r.id);
      ok = ok && d && d->stringId == r.sid && d->name == r.name &&
           findItemByStringId(r.sid) == d && itemDisplayName(r.id) == r.name &&
           !d->isTool && !d->isWeapon && d->toolAction == ToolAction::None &&
           d->maxStack == 512 && d->modelStem.empty() && !d->startingItem &&
           placedBlockForItem(r.id) == kAir;
      if (!d) std::printf("  (missing material item %s)\n", r.sid);
    }
    check(ok, "materials: all 15 JD materials registered, round-trip by id + string id, plain material profile");
    // Save-resolution guard: resolveStackStringId checks items BEFORE
    // blocks, so a material item whose string id equals a block's would
    // swallow saved stacks of that block on load (the silicon_ingot
    // rename). None of the 15 may collide with a block string id.
    bool noCollision = true;
    for (const Row& r : rows)
      noCollision = noCollision && !blockRegistry().findById(r.sid).has_value();
    check(noCollision, "materials: no material item string id collides with a block string id (save-resolution precedence)");
  }

  // 3. The tool / weapon split (tools interact with blocks, weapons damage
  //    entities, an item can be both via meleeDamage).
  {
    const ItemDef* bash = findItem(kItemBashor);
    const ItemDef* slic = findItem(kItemSlicor);
    const ItemDef* pick = findItem(kItemPickor);
    const ItemDef* seed = findItem(kItemSeedor);
    const ItemDef* wall = findItem(kItemFirewall);
    const ItemDef* ping = findItem(kItemPing);
    const ItemDef* call = findItem(kItemCallback);
    check(bash && bash->isTool && bash->isWeapon && bash->meleeDamage > 0 &&
              bash->toolAction == ToolAction::Bash,
          "split: BashOR is both (hammer tool + mace-role meleeDamage)");
    check(slic && slic->isTool && slic->isWeapon && slic->meleeDamage > 0 &&
              slic->toolAction == ToolAction::Slice,
          "split: SlicOR is both (sword weapon + slice tool)");
    check(pick && pick->isTool && !pick->isWeapon && pick->toolAction == ToolAction::Pick &&
              seed && seed->isTool && seed->toolAction == ToolAction::Till,
          "split: PickOR / SeedOR are pure tools");
    check(wall && wall->isWeapon && !wall->isTool && wall->defense > 0,
          "split: Firewall is a defensive weapon (shield), not a block tool");
    check(ping && ping->isWeapon && ping->projectile == "packet" && !ping->returnsToOwner &&
              call && call->isWeapon && call->projectile == "callback" && call->returnsToOwner,
          "split: Ping fires packets; Callback is thrown and always returns");
  }

  // 4. ChopOR tier rule: tier 1 = trace-trunk (decided); NO tier ladder.
  {
    const ItemDef* chop = findItem(kItemChopor);
    bool ladder = false;
    for (const ItemDef& d : itemRegistry())
      if (d.toolAction == ToolAction::Chop && d.itemId != kItemChopor) ladder = true;
    check(chop && chop->tier == 1 && chop->material == "trace_trunk" && !ladder,
          "tiers: ChopOR tier 1 = trace_trunk is the only ChopOR (no invented tier 2+)");
  }

  // 5. Models: every item with a modelStem has a loadable .lthm on disk.
  {
    bool ok = true;
    for (const ItemDef& d : itemRegistry()) {
      if (d.modelStem.empty()) continue;
      const std::string path =
          std::string(LITHOKRAFT_MODELS_DIR) + "/" + std::string(d.modelStem) + ".lthm";
      const auto lr = lithokraft::model::loadModel(path);
      if (lr.error != lithokraft::model::LoadError::Ok || lr.model.geometry.leafCount() == 0) {
        std::printf("  (model missing/unloadable for %s: %s)\n", d.name.data(), path.c_str());
        ok = false;
      }
    }
    check(ok, "models: every item modelStem resolves to a loadable, non-empty .lthm");
  }

  // 6. ACCEPTANCE: a fresh spawn inventory holds the Fragment (granted,
  //    first tick, idempotent) and can hold every lore tool item.
  {
    Inventory inv;  // fresh spawn: empty
    check(inv.countOf(kItemFragment) == 0, "acceptance: fresh inventory starts empty");
    check(grantStartingItems(inv) && inv.countOf(kItemFragment) == 1,
          "acceptance: fresh spawn is granted the Fragment (starting item)");
    check(!grantStartingItems(inv) && inv.countOf(kItemFragment) == 1,
          "acceptance: Fragment grant is idempotent (soulbound, no duplicates)");
    const ItemDef& start = startingItemDef();
    check(start.itemId == kItemFragment && start.startingItem &&
              inv.hotbar()[0].itemId == kItemFragment,
          "acceptance: the Fragment lands in hotbar slot 0 of a fresh inventory");

    bool all = true;
    for (const ItemDef& d : itemRegistry()) {
      if (d.startingItem) continue;  // already granted above
      const uint16_t left = inv.addItem(d.itemId, 1);
      if (left != 0 || inv.countOf(d.itemId) < 1) {
        std::printf("  (fresh inventory cannot hold %s)\n", d.name.data());
        all = false;
      }
    }
    check(all, "acceptance: fresh inventory can hold every lore tool / item (31 defs)");
    check(inv.addItem(kItemSilicaSeed, 512) == 0 && inv.countOf(kItemSilicaSeed) == 513,
          "acceptance: materials still stack (seed x512 on top of the held one)");
  }

  // 7. P2b-1 bridge: what a held stack places, how it is named, and which
  //    HUD icon represents it. Blocks are their own item form; only the
  //    ChipOR among registered items places a block (via the unified
  //    registry's placesBlock); tools and the Fragment place nothing.
  check(placedBlockForItem(kChipOR) == kChipOR && placedBlockForItem(kWaferSilicon) == kWaferSilicon &&
            placedBlockForItem(kLamp) == kLamp && placedBlockForItem(kItemFragment) == kAir &&
            placedBlockForItem(kItemBashor) == kAir && placedBlockForItem(kItemSilicaSeed) == kAir &&
            placedBlockForItem(0) == kAir && placedBlockForItem(kFirstItemId + 99) == kAir,
        "bridge: block stacks place themselves, ChipOR places its block, tools/Fragment place nothing");
  check(itemDisplayName(kItemFragment) == "The Fragment" && itemDisplayName(kLamp) == "Lamp" &&
            itemDisplayName(0) == "empty",
        "bridge: display names come from the item def / block def");
  check(itemIconRef(kItemFragment) == "item:" + std::to_string(kItemFragment) &&
            itemIconRef(kItemBashor) == "item:" + std::to_string(kItemBashor) &&
            itemIconRef(kLamp) == "block:" + std::to_string(int(kLamp)) && itemIconRef(0).empty(),
        "bridge: icon refs -- non-block items use item icons, block stacks use block icons");

  std::printf("items_selftest: %d failures\n", g_fails);
  return g_fails ? 1 : 0;
}
