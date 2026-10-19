// LithoKraft item registry: the lore hand tools / weapons, the Fragment, the
// silica seed, and the ChipOR as a held item. See items.h for the id layout,
// the tool/weapon split, and the tier rule (ChopOR tier 1 only -- decided;
// no tier ladder is invented here).
//
// Lore sources (Glossary v2, 2026-10-11, "Tool & weapon names" -- LOCKED;
// Lore Bible v0.2 §11 for the Fragment; Lore Addendum 2026-10-12 for the
// ChopOR tier-1 material):
//   BashOR hammer, PickOR pickaxe, ChopOR axe, HeapOR shovel, SeedOR hoe,
//   SlicOR sword, Firewall shield, Ping bow, Callback returning thrown
//   weapon, DebuggOR debug stick, Sniffor handheld receiver (IN DISCUSSION),
//   Atlas map (name LOCKED, crafting TBD), Fragment starting book (LOCKED).
// The EtchOR family (EtchOR / SelectOR / ConnectOR / Laysor / PortOR /
// FabOR) are machine BLOCKS/components in lore, not hand tools -- they are
// deliberately NOT items here (PortOR/Fusor already exist as blocks).
#include "lithokraft/content/items.h"

namespace lithokraft {

namespace {
// The item table. The ChipOR row's itemId is a 0 placeholder here and is
// fixed up in itemRegistry(): kChipOR is itself resolved through the block
// registry, so reading it while this table is first built (which can happen
// inside the registry's own static init) could observe it unassigned.
std::vector<ItemDef> buildItemTable() {
  return {
      // Silica seed (existing id, silica.h): the farmable material drop.
      {kItemSilicaSeed, "lithokraft:silica_seed", "Silica Seed",
       false, false, ToolAction::None, 0, 0, "", false,
       /*maxStack*/ 512, /*tier*/ 0, "", /*model*/ "", "silica_seed", false},
      // The Fragment: starting item, day one, first tick. An editable book
      // (journal / unlocks / recipes / chip archive / images). Soulbound on
      // Easy/Normal in lore; the state model is FragmentState/FragmentGallery.
      {kItemFragment, "lithokraft:fragment", "The Fragment",
       false, false, ToolAction::None, 0, 0, "", false,
       1, 0, "", "fragment", "fragment", /*startingItem*/ true},
      // --- the armory: six tools with checked-in .lthm models --------------
      // BashOR: hammer AND the mace role (lore: "The mace role is already
      // covered by BashOR") -- the canonical tool-with-meleeDamage item.
      {kItemBashor, "lithokraft:bashor", "BashOR",
       true, true, ToolAction::Bash, /*melee*/ 6, 0, "", false,
       1, 0, "", "bashor", "bashor", false},
      {kItemPickor, "lithokraft:pickor", "PickOR",
       true, false, ToolAction::Pick, 0, 0, "", false,
       1, 0, "", "pickor", "pickor", false},
      // ChopOR tier 1 (DECIDED 2026-10-12): surface-tree material -- the
      // Wafer's wood equivalent, trace trunk. Tiers beyond 1 are undecided:
      // this is the only ChopOR row; a future tier appends a new row/id.
      {kItemChopor, "lithokraft:chopor", "ChopOR",
       true, true, ToolAction::Chop, /*melee*/ 4, 0, "", false,
       1, /*tier*/ 1, "trace_trunk", "chopor", "chopor", false},
      {kItemHeapor, "lithokraft:heapor", "HeapOR",
       true, false, ToolAction::Dig, 0, 0, "", false,
       1, 0, "", "heapor", "heapor", false},
      {kItemSeedor, "lithokraft:seedor", "SeedOR",
       true, false, ToolAction::Till, 0, 0, "", false,
       1, 0, "", "seedor", "seedor", false},
      // SlicOR: the sword -- a weapon that also slices blocks/arrays.
      {kItemSlicor, "lithokraft:slicor", "SlicOR",
       true, true, ToolAction::Slice, /*melee*/ 7, 0, "", false,
       1, 0, "", "slicor", "slicor", false},
      // --- the rest of the LOCKED tool & weapon list -----------------------
      // Firewall: the shield. Defense, not a block tool.
      {kItemFirewall, "lithokraft:firewall", "Firewall",
       false, true, ToolAction::None, /*melee*/ 1, /*defense*/ 8, "", false,
       1, 0, "", "firewall", "firewall", false},
      // Ping: the bow. Fires packets (projectiles awaiting responses).
      {kItemPing, "lithokraft:ping", "Ping",
       false, true, ToolAction::None, /*melee*/ 1, 0, "packet", false,
       1, 0, "", "ping", "ping", false},
      // Callback: the returning thrown weapon (trident role).
      {kItemCallback, "lithokraft:callback", "Callback",
       false, true, ToolAction::None, /*melee*/ 3, 0, "callback", /*returns*/ true,
       1, 0, "", "callback", "callback", false},
      // DebuggOR: the debug stick -- rotates blocks freely after placement.
      {kItemDebuggor, "lithokraft:debuggor", "DebuggOR",
       true, false, ToolAction::Rotate, 0, 0, "", false,
       1, 0, "", "debuggor", "debuggor", false},
      // Sniffor: handheld early signal receiver (lore status IN DISCUSSION;
      // registered so the inventory can hold it once JD locks it).
      {kItemSniffor, "lithokraft:sniffor", "Sniffor",
       true, false, ToolAction::Sense, 0, 0, "", false,
       1, 0, "", "sniffor", "sniffor", false},
      // Atlas: the in-game map item (name LOCKED; crafting method TBD).
      {kItemAtlas, "lithokraft:atlas", "Atlas",
       false, false, ToolAction::None, 0, 0, "", false,
       1, 0, "", "atlas", "atlas", false},
      // Titanium Oxide (JD-named 2026-10-14, Deceitor work): a plain
      // material — white pigment powder. Deceitors love it; see the
      // liked-items table in game/deceitor.h. No model (.lthm) yet.
      {kItemTitaniumOxide, "lithokraft:titanium_oxide", "Titanium Oxide",
       false, false, ToolAction::None, 0, 0, "", false,
       /*maxStack*/ 512, /*tier*/ 0, "", /*model*/ "", "titanium_oxide", false},
      // JD materials shelf (2026-10-14): plain + silicon-compound pairs,
      // in JD's list order (see items.h). All plain materials: stack 512,
      // no tool/weapon role, no .lthm model yet.
      {kItemSilicon, "lithokraft:silicon_ingot", "Silicon",
       false, false, ToolAction::None, 0, 0, "", false,
       /*maxStack*/ 512, /*tier*/ 0, "", /*model*/ "", "silicon_ingot", false},
      {kItemCarbide, "lithokraft:carbide", "Carbide",
       false, false, ToolAction::None, 0, 0, "", false,
       /*maxStack*/ 512, /*tier*/ 0, "", /*model*/ "", "carbide", false},
      {kItemSiliconCarbide, "lithokraft:silicon_carbide", "Silicon Carbide",
       false, false, ToolAction::None, 0, 0, "", false,
       /*maxStack*/ 512, /*tier*/ 0, "", /*model*/ "", "silicon_carbide", false},
      {kItemGallium, "lithokraft:gallium", "Gallium",
       false, false, ToolAction::None, 0, 0, "", false,
       /*maxStack*/ 512, /*tier*/ 0, "", /*model*/ "", "gallium", false},
      {kItemSiliconGallium, "lithokraft:silicon_gallium", "Silicon Gallium",
       false, false, ToolAction::None, 0, 0, "", false,
       /*maxStack*/ 512, /*tier*/ 0, "", /*model*/ "", "silicon_gallium", false},
      {kItemNitride, "lithokraft:nitride", "Nitride",
       false, false, ToolAction::None, 0, 0, "", false,
       /*maxStack*/ 512, /*tier*/ 0, "", /*model*/ "", "nitride", false},
      {kItemSiliconNitride, "lithokraft:silicon_nitride", "Silicon Nitride",
       false, false, ToolAction::None, 0, 0, "", false,
       /*maxStack*/ 512, /*tier*/ 0, "", /*model*/ "", "silicon_nitride", false},
      {kItemDioxide, "lithokraft:dioxide", "Dioxide",
       false, false, ToolAction::None, 0, 0, "", false,
       /*maxStack*/ 512, /*tier*/ 0, "", /*model*/ "", "dioxide", false},
      {kItemSiliconDioxide, "lithokraft:silicon_dioxide", "Silicon Dioxide",
       false, false, ToolAction::None, 0, 0, "", false,
       /*maxStack*/ 512, /*tier*/ 0, "", /*model*/ "", "silicon_dioxide", false},
      {kItemCopper, "lithokraft:copper", "Copper",
       false, false, ToolAction::None, 0, 0, "", false,
       /*maxStack*/ 512, /*tier*/ 0, "", /*model*/ "", "copper", false},
      {kItemSiliconCopper, "lithokraft:silicon_copper", "Silicon Copper",
       false, false, ToolAction::None, 0, 0, "", false,
       /*maxStack*/ 512, /*tier*/ 0, "", /*model*/ "", "silicon_copper", false},
      {kItemAluminum, "lithokraft:aluminum", "Aluminum",
       false, false, ToolAction::None, 0, 0, "", false,
       /*maxStack*/ 512, /*tier*/ 0, "", /*model*/ "", "aluminum", false},
      {kItemSiliconAluminum, "lithokraft:silicon_aluminum", "Silicon Aluminum",
       false, false, ToolAction::None, 0, 0, "", false,
       /*maxStack*/ 512, /*tier*/ 0, "", /*model*/ "", "silicon_aluminum", false},
      // Photoresist / Resist: JD's two coating entries, as written.
      {kItemPhotoresist, "lithokraft:photoresist", "Photoresist",
       false, false, ToolAction::None, 0, 0, "", false,
       /*maxStack*/ 512, /*tier*/ 0, "", /*model*/ "", "photoresist", false},
      {kItemResist, "lithokraft:resist", "Resist",
       false, false, ToolAction::None, 0, 0, "", false,
       /*maxStack*/ 512, /*tier*/ 0, "", /*model*/ "", "resist", false},
      // ChipOR as an item: the printed chip. Block-backed -- the itemId IS
      // the ChipOR BlockId, so a held ChipOR and a placed ChipOR are the
      // same id (the EtchOR's name field names the individual chip in its
      // per-stack data, not in this def). Model: chipor-demo.lthm (the
      // viewport's miniaturized-circuit demo model).
      {0, "lithokraft:chipor", "ChipOR",
       false, false, ToolAction::None, 0, 0, "", false,
       /*maxStack*/ 512, 0, "", "chipor-demo", "chipor", false},
  };
}
}  // namespace

const std::vector<ItemDef>& itemRegistry() {
  static std::vector<ItemDef> kItems = buildItemTable();
  // ChipOR fix-up (see buildItemTable): resolve the row's itemId from the
  // block registry's string id once the registry exists. Idempotent; runs
  // until resolved, then never again (the id is stable, append-only).
  ItemDef& chipor = kItems.back();
  if (chipor.itemId == 0) {
    if (const auto id = blockRegistry().findById("lithokraft:chipor"))
      chipor.itemId = uint32_t(*id);
  }
  return kItems;
}

const ItemDef* findItem(uint32_t itemId) {
  for (const ItemDef& d : itemRegistry())
    if (d.itemId == itemId) return &d;
  return nullptr;
}

const ItemDef* findItemByStringId(std::string_view stringId) {
  for (const ItemDef& d : itemRegistry())
    if (d.stringId == stringId) return &d;
  return nullptr;
}

bool isItemId(uint32_t itemId) {
  if (findItem(itemId)) return true;
  return itemId > 0 && itemId < blockRegistry().size();  // any live block
}

BlockId placedBlockForItem(uint32_t itemId) {
  if (itemId == 0) return kAir;
  if (const ItemDef* d = findItem(itemId)) {
    // Registered item: the unified registry is the authority on whether it
    // places a block (today only the ChipOR does, via its stable id).
    const ContentRegistry& content = blockRegistry().content();
    if (const auto engineId = content.findItem(d->stringId)) {
      const StableId sb = content.itemDef(*engineId).placesBlock;
      if (sb != 0)
        if (const auto b = content.findBlockByStableId(sb)) return *b;
    }
    return kAir;  // tools / weapons / books place nothing
  }
  // Unregistered id: a live BlockId is its own item form (items.h id layout).
  return itemId < blockRegistry().size() ? BlockId(itemId) : kAir;
}

std::string itemDisplayName(uint32_t itemId) {
  if (itemId == 0) return "empty";
  if (const ItemDef* d = findItem(itemId)) return std::string(d->name);
  if (itemId < blockRegistry().size()) return std::string(blockRegistry().def(BlockId(itemId)).name);
  return "unknown item";
}

std::string itemIconRef(uint32_t itemId) {
  if (itemId == 0) return {};
  // Non-block items render from their real item icon; block-backed stacks
  // (including the ChipOR, whose itemId IS its BlockId) render as blocks.
  if (const ItemDef* d = findItem(itemId); d && d->itemId >= kFirstItemId)
    return "item:" + std::to_string(itemId);
  const BlockId b = placedBlockForItem(itemId);
  return b == kAir ? std::string() : "block:" + std::to_string(int(b));
}

const ItemDef& startingItemDef() {
  for (const ItemDef& d : itemRegistry())
    if (d.startingItem) return d;
  return itemRegistry()[1];  // the Fragment row (defensive; unreachable)
}

void registerGameItems(ContentRegistry& content) {
  for (const ItemDef& d : itemRegistry()) {
    litholattice::ItemDef ed;
    ed.name = d.name;
    ed.maxStack = d.maxStack;
    ed.id = d.stringId;
    // The engine never reads `behavior`; the game uses it as the item's
    // behaviour stem (the suffix of its string id, e.g. "bashor").
    const size_t colon = d.stringId.find(':');
    ed.behavior = colon == std::string_view::npos ? d.stringId : d.stringId.substr(colon + 1);
    // Block-backed items place their block (ChipOR item -> ChipOR block).
    ed.placesBlock =
        d.itemId < kFirstItemId ? content.blockStableId(BlockId(d.itemId)) : StableId(0);
    content.addItem(ed);  // first-wins on duplicates; throws only when sealed
  }
}

}  // namespace lithokraft
