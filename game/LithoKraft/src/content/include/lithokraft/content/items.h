// LithoKraft items: the registry for everything an ItemStack can hold that is
// not a plain block -- the lore hand tools / weapons, the Fragment (the
// first-tick starting item), the silica seed, and the ChipOR as a held item.
//
// Ids (ItemStack::itemId, see game/inventory.h):
//   - Block items use their BlockId directly (0 < id < blockRegistry size):
//     holding a block IS holding its item form. The ChipOR's entry below is
//     the canonical example (itemId == kChipOR).
//   - Non-block items live at kFirstItemId (1<<16) and up, append-only, so
//     the two namespaces never collide (the silica seed took +0 first; see
//     content/silica.h). NEVER renumber or reuse an offset: item ids persist
//     in inventories and saves exactly like block ids.
//
// Tool / weapon split (JD-locked): tools interact with blocks, weapons deal
// damage to entities; an item can be both (a tool with meleeDamage > 0 --
// BashOR is the hammer AND the mace role; SlicOR is the sword that also
// slices blocks/arrays). isTool / isWeapon are independent flags for that
// reason; neither implies the other.
//
// Tiers: only ONE tier is decided in lore today -- ChopOR tier 1 is the
// surface-tree material (trace trunk; Lore Addendum 2026-10-12: "logs ->
// planks -> sticks -> make a ChopOR. The first ChopOR tier is the tree
// material"). Tiers beyond tier 1 are UNDECIDED. ItemDef::tier is the clean
// extension point: a future tier is a new append-only ItemDef row with
// tier = 2+ and its own string id. Do NOT invent a tier ladder here.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "lithokraft/content/blocks.h"  // kFirstItemId lives in silica.h; blocks for kChipOR
#include "lithokraft/content/silica.h"  // kFirstItemId, kItemSilicaSeed

namespace lithokraft {

// --- append-only non-block item ids (offset from kFirstItemId) ------------
// +0 is the silica seed (kItemSilicaSeed in silica.h). Append below; never
// renumber.
constexpr uint32_t kItemFragment = kFirstItemId + 1;   ///< the Fragment (starting book)
constexpr uint32_t kItemBashor = kFirstItemId + 2;     ///< BashOR (hammer)
constexpr uint32_t kItemPickor = kFirstItemId + 3;     ///< PickOR (pickaxe)
constexpr uint32_t kItemChopor = kFirstItemId + 4;     ///< ChopOR tier 1 (axe, trace-trunk)
constexpr uint32_t kItemHeapor = kFirstItemId + 5;     ///< HeapOR (shovel)
constexpr uint32_t kItemSeedor = kFirstItemId + 6;     ///< SeedOR (hoe)
constexpr uint32_t kItemSlicor = kFirstItemId + 7;     ///< SlicOR (sword)
constexpr uint32_t kItemFirewall = kFirstItemId + 8;   ///< Firewall (shield)
constexpr uint32_t kItemPing = kFirstItemId + 9;       ///< Ping (bow)
constexpr uint32_t kItemCallback = kFirstItemId + 10;  ///< Callback (returning thrown weapon)
constexpr uint32_t kItemDebuggor = kFirstItemId + 11;  ///< DebuggOR (debug stick)
constexpr uint32_t kItemSniffor = kFirstItemId + 12;   ///< Sniffor (handheld signal receiver)
constexpr uint32_t kItemAtlas = kFirstItemId + 13;     ///< Atlas (in-game map)
/// Titanium Oxide (JD-named 2026-10-14): a plain material item — the one
/// JD-sanctioned new item of the Deceitor work; Deceitors love it (see
/// game/deceitor.h liked-items table). No tool/weapon role, no model.
constexpr uint32_t kItemTitaniumOxide = kFirstItemId + 14;  ///< Titanium Oxide (material)
// --- JD materials shelf (2026-10-14, New-Blocks-Materials list) ----------
// The semiconductor materials, in JD's pairing: EVERY material ships in
// two forms -- the plain substance AND its silicon compound ("and on down
// the list"). Plain material items in the Titanium Oxide pattern
// (maxStack 512, no tool/weapon role, no model). Silicon is the base:
// plain silicon itself; its compound form IS the family below, so there
// is no separate "silicon silicon" row. The Silicon item's string id is
// "lithokraft:silicon_ingot", NOT the Wafer Silicon block's
// "lithokraft:silicon": player-save stack resolution
// (resolveStackStringId) checks items before blocks, so an item claiming
// the block's string id would swallow every saved Wafer Silicon stack on
// load. (The ChipOR share works because that item IS the block's item
// form; this item is the loose material, distinct from the placed
// wafer.) iconName is likewise "silicon_ingot" --
// textures/ui/icons/silicon.png is the block's icon.
constexpr uint32_t kItemSilicon = kFirstItemId + 15;          ///< Silicon (material)
constexpr uint32_t kItemCarbide = kFirstItemId + 16;          ///< Carbide (material)
constexpr uint32_t kItemSiliconCarbide = kFirstItemId + 17;   ///< Silicon Carbide (material)
constexpr uint32_t kItemGallium = kFirstItemId + 18;          ///< Gallium (material)
constexpr uint32_t kItemSiliconGallium = kFirstItemId + 19;   ///< Silicon Gallium (material)
constexpr uint32_t kItemNitride = kFirstItemId + 20;          ///< Nitride (material)
constexpr uint32_t kItemSiliconNitride = kFirstItemId + 21;   ///< Silicon Nitride (material)
constexpr uint32_t kItemDioxide = kFirstItemId + 22;          ///< Dioxide (material)
constexpr uint32_t kItemSiliconDioxide = kFirstItemId + 23;   ///< Silicon Dioxide (material)
constexpr uint32_t kItemCopper = kFirstItemId + 24;           ///< Copper (material)
constexpr uint32_t kItemSiliconCopper = kFirstItemId + 25;    ///< Silicon Copper (material)
constexpr uint32_t kItemAluminum = kFirstItemId + 26;         ///< Aluminum (material)
constexpr uint32_t kItemSiliconAluminum = kFirstItemId + 27;  ///< Silicon Aluminum (material)
// Photoresist / Resist (JD list, 2026-10-14): the lithography coating
// materials, registered as JD wrote them -- two entries. Their roles in
// the etch flow are JD-to-define; plain materials here.
constexpr uint32_t kItemPhotoresist = kFirstItemId + 28;      ///< Photoresist (material)
constexpr uint32_t kItemResist = kFirstItemId + 29;           ///< Resist (material)

/// What using the item on a block does (the tool half of the split).
enum class ToolAction : uint8_t {
  None,     ///< not a tool
  Bash,     ///< BashOR: hammer / mace-role bashing
  Pick,     ///< PickOR: pickaxe (decompiles blocks into source)
  Chop,     ///< ChopOR: axe (fells silicon / trace trees)
  Dig,      ///< HeapOR: shovel (digs the heap)
  Till,     ///< SeedOR: hoe -- tills soil (silicaTill) and plants seeds
  Slice,    ///< SlicOR: slices blocks / arrays
  Rotate,   ///< DebuggOR: rotates a placed block freely
  Sense,    ///< Sniffor: transcodes ambient Signal around the player
};

struct ItemDef {
  uint32_t itemId = 0;              ///< ItemStack::itemId (BlockId or kFirstItemId+offset)
  std::string_view stringId;        ///< stable save id, e.g. "lithokraft:bashor"
  std::string_view name;            ///< display name, e.g. "BashOR"
  bool isTool = false;              ///< interacts with blocks (see toolAction)
  bool isWeapon = false;            ///< deals damage to entities (see meleeDamage / projectile)
  ToolAction toolAction = ToolAction::None;
  int meleeDamage = 0;              ///< >0: usable as a melee weapon (the "both" case)
  int defense = 0;                  ///< >0: blocks damage while held (Firewall)
  std::string_view projectile;      ///< ranged/thrown projectile id; "" = not ranged/thrown
  bool returnsToOwner = false;      ///< thrown weapon always comes back (Callback)
  uint16_t maxStack = 1;            ///< tools/weapons/books: 1; materials: kMaxStack-class
  int tier = 0;                     ///< 0 = lore has no tier for this item; ChopOR = 1 (decided)
  std::string_view material;        ///< tier material where lore decides it (ChopOR: "trace_trunk")
  std::string_view modelStem;       ///< assets/models/<stem>.lthm; "" = no .lthm model
  std::string_view iconName;        ///< textures/ui/icons/<iconName>.png in the packs
  bool startingItem = false;        ///< granted on fresh spawn (the Fragment, first tick)
};

/// Every registered item (non-block items in id order, then the block-backed
/// ChipOR entry). The registry is static data: no registration call exists --
/// adding an item is appending one row in items.cpp plus one kItem* constant
/// above (append-only, like blocks.cpp).
const std::vector<ItemDef>& itemRegistry();
/// The def for an ItemStack::itemId, or nullptr when unregistered (a raw
/// BlockId with no ItemDef is still a valid block item -- see isItemId).
const ItemDef* findItem(uint32_t itemId);
/// The def for a stable string id ("lithokraft:bashor"), or nullptr.
const ItemDef* findItemByStringId(std::string_view stringId);
/// True when `itemId` can sit in an inventory: a registered item id, or any
/// live BlockId (blocks are their own item form).
bool isItemId(uint32_t itemId);
/// The item granted on fresh spawn (lore: the Fragment, day one, first tick).
const ItemDef& startingItemDef();

/// The block a held stack places when used, or kAir when it places nothing
/// (P2b-1 bridge between ItemStacks and block placement). Resolution:
/// a registered item defers to its unified ContentRegistry entry's
/// placesBlock (the ChipOR item -> the ChipOR block); an unregistered id
/// that is a live BlockId IS its own item form and places itself; tools,
/// weapons and the Fragment place nothing.
BlockId placedBlockForItem(uint32_t itemId);
/// Display name of a stack's contents ("empty" for an empty stack id 0).
std::string itemDisplayName(uint32_t itemId);
/// HUD icon reference for a stack: "block:<BlockId>" for block-backed
/// stacks (rendered from block materials / block icons), "item:<itemId>"
/// for registered non-block items (rendered from the item icon atlas),
/// "" for an empty stack.
std::string itemIconRef(uint32_t itemId);

/// Registers every item above into the engine's unified ContentRegistry (the
/// Infinite Registry's item kind: litholattice::ItemDef with name / maxStack
/// / id / behavior, plus placesBlock for the block-backed ChipOR), so the
/// canonical catalog, stable ids and content CRCs cover items as well as
/// blocks. Called once from blocks.cpp while the block registry is built --
/// single-threaded, before the boot seal. The engine ItemId space is dense
/// and separate; ItemStack ids keep the game's convention above until the
/// planned StableId inventory migration (a builder follow-up).
void registerGameItems(ContentRegistry& content);

}  // namespace lithokraft
