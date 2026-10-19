// LithoKraft inventory: 42 carry slots + 8 gear slots.
// 42 = 8 hotbar + 2 offhands (L/R) + 32 main (4x8). 42 = 101010 in binary.
// Gear (worn, not counted in 42): head, chest, legs, feet, arms, face, heart, mind.
// Max stack: 512 (= tick rate).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "lithokraft/content/items.h"  // startingItemDef (the Fragment)

namespace lithokraft {

constexpr int kMaxStack = 512;       ///< max items per stack (= tick rate)
constexpr int kInvHotbarSlots = 8;
constexpr int kMainRows = 4;
constexpr int kMainCols = 8;
constexpr int kMainSlots = kMainRows * kMainCols;  // 32
constexpr int kCarrySlots = kInvHotbarSlots + 2 + kMainSlots;  // 42
constexpr int kGearSlots = 8;
constexpr int kTotalSlots = kCarrySlots + kGearSlots;  // 50: every slot, gear included

/// Gear slot indices.
enum GearSlot : uint8_t {
  kGearHead = 0, kGearChest, kGearLegs, kGearFeet,
  kGearArms, kGearFace, kGearHeart, kGearMind,
};

/// A stack of items. itemId 0 = empty.
///
/// Per-stack data (wiring pass 2026-10-13, B6/B7): a stack may carry a
/// custom name (the EtchOR name field: "the typed name becomes the output
/// chip's name") and applied certificate ids (certificate-table signing).
/// Data-bearing stacks are INDIVIDUALS: plain addItem() never merges into
/// them and they only stack with stacks whose data is identical. The data
/// persists in player.dat's v5 stack-data block (game/world_save.cpp);
/// in the game's tagged binary storage the name is simply the stack's
/// "name" field, per JD's LBT field design.
struct ItemStack {
  uint32_t itemId = 0;  ///< BlockId for blocks; item ids for non-blocks
  uint16_t count = 0;   ///< 1..kMaxStack
  std::string customName;                  ///< EtchOR name field value; empty = the def name
  std::vector<std::string> certificates;   ///< applied certificate ids (content/certificates.h)

  bool empty() const { return itemId == 0 || count == 0; }
  bool hasData() const { return !customName.empty() || !certificates.empty(); }
  /// Identity for stacking: same item AND same per-stack data.
  bool sameIdentity(const ItemStack& o) const {
    return itemId == o.itemId && customName == o.customName && certificates == o.certificates;
  }
  void clear() { itemId = 0; count = 0; customName.clear(); certificates.clear(); }
};

class Inventory {
 public:
  uint64_t version() const { return version_; }  ///< increments on every mutation (UI polls this)
  /// Adds items, stacking with existing and filling empty slots. Returns leftover count.
  uint16_t addItem(uint32_t itemId, uint16_t count) {
    uint16_t left = count;
    // 1. Stack with existing (hotbar first, then main). Plain adds never
    // merge into a data-bearing stack (a named/certified stack is an
    // individual; merging would silently strip its identity).
    for (auto* s : carrySlots()) {
      if (left == 0) break;
      if (s->itemId == itemId && !s->hasData() && s->count < kMaxStack) {
        const uint16_t space = uint16_t(kMaxStack - s->count);
        const uint16_t take = left < space ? left : space;
        s->count += take;
        left -= take;
      }
    }
    // 2. Fill empty slots.
    for (auto* s : carrySlots()) {
      if (left == 0) break;
      if (s->empty()) {
        s->itemId = itemId;
        s->count = left < kMaxStack ? left : kMaxStack;
        left -= s->count;
      }
    }
    if (left != count) ++version_;
    return left;
  }

  /// Removes up to `count` of itemId. Returns how many were actually removed.
  uint16_t removeItem(uint32_t itemId, uint16_t count) {
    uint16_t removed = 0;
    for (auto* s : carrySlots()) {
      if (removed >= count) break;
      if (s->itemId == itemId) {
        const uint16_t take = s->count < (count - removed) ? s->count : (count - removed);
        s->count -= take;
        removed += take;
        if (s->count == 0) s->clear();
      }
    }
    if (removed > 0) ++version_;
    return removed;
  }

  /// Total count of itemId across all carry slots.
  uint16_t countOf(uint32_t itemId) const {
    uint16_t total = 0;
    for (const auto& s : hotbar_) if (s.itemId == itemId) total += s.count;
    if (offhandLeft_.itemId == itemId) total += offhandLeft_.count;
    if (offhandRight_.itemId == itemId) total += offhandRight_.count;
    for (const auto& s : main_) if (s.itemId == itemId) total += s.count;
    return total;
  }

  // Accessors.
  std::array<ItemStack, kInvHotbarSlots>& hotbar() { return hotbar_; }
  const std::array<ItemStack, kInvHotbarSlots>& hotbar() const { return hotbar_; }
  ItemStack& offhandLeft() { return offhandLeft_; }
  const ItemStack& offhandLeft() const { return offhandLeft_; }
  ItemStack& offhandRight() { return offhandRight_; }
  const ItemStack& offhandRight() const { return offhandRight_; }
  std::array<ItemStack, kMainSlots>& main() { return main_; }
  const std::array<ItemStack, kMainSlots>& main() const { return main_; }
  std::array<ItemStack, kGearSlots>& gear() { return gear_; }
  const std::array<ItemStack, kGearSlots>& gear() const { return gear_; }

  /// Every slot in the CANONICAL FLAT ORDER (P2b-1): hotbar 0..7, offhand
  /// L, offhand R, main 0..31, gear 0..7 — 50 slots. This is the order the
  /// player.dat v4 inventory block is written in; never reorder it.
  std::array<ItemStack*, kTotalSlots> allSlots() {
    std::array<ItemStack*, kTotalSlots> out{};
    size_t i = 0;
    for (auto& s : hotbar_) out[i++] = &s;
    out[i++] = &offhandLeft_;
    out[i++] = &offhandRight_;
    for (auto& s : main_) out[i++] = &s;
    for (auto& s : gear_) out[i++] = &s;
    return out;
  }
  std::array<const ItemStack*, kTotalSlots> allSlots() const {
    std::array<const ItemStack*, kTotalSlots> out{};
    size_t i = 0;
    for (const auto& s : hotbar_) out[i++] = &s;
    out[i++] = &offhandLeft_;
    out[i++] = &offhandRight_;
    for (const auto& s : main_) out[i++] = &s;
    for (const auto& s : gear_) out[i++] = &s;
    return out;
  }
  ItemStack& slotAt(int flatIndex) { return *allSlots()[size_t(flatIndex)]; }
  const ItemStack& slotAt(int flatIndex) const { return *allSlots()[size_t(flatIndex)]; }

  /// Places a (possibly data-bearing) stack: merges onto an identical
  /// stack (sameIdentity) with room for the WHOLE stack, else takes the
  /// first empty carry slot. All-or-nothing: returns false and places
  /// nothing when it does not fit (etching/signing check this BEFORE
  /// consuming anything, so a full inventory never eats an input).
  bool placeStack(const ItemStack& st) {
    if (st.empty()) return true;
    for (auto* s : carrySlots())
      if (s->sameIdentity(st) && int(s->count) + int(st.count) <= kMaxStack) {
        s->count = uint16_t(s->count + st.count);
        ++version_;
        return true;
      }
    for (auto* s : carrySlots())
      if (s->empty()) {
        *s = st;
        ++version_;
        return true;
      }
    return false;
  }

  /// True when placeStack(st) would succeed (no mutation). Mirrors
  /// placeStack's carry-slot scope exactly (flat indices 0..kCarrySlots-1;
  /// gear slots never receive placed stacks).
  bool canPlaceStack(const ItemStack& st) const {
    if (st.empty()) return true;
    const auto slots = allSlots();
    for (int i = 0; i < kCarrySlots; ++i) {
      const ItemStack* s = slots[size_t(i)];
      if (s->empty()) return true;
      if (s->sameIdentity(st) && int(s->count) + int(st.count) <= kMaxStack) return true;
    }
    return false;
  }

  void clear() {
    for (auto* s : allSlots()) s->clear();
    ++version_;
  }
  bool isEmpty() const {
    for (const auto* s : allSlots())
      if (!s->empty()) return false;
    return true;
  }

 private:
  // All 42 carry slots in fill order: hotbar, offhands, main.
  std::vector<ItemStack*> carrySlots() {
    std::vector<ItemStack*> out;
    out.reserve(kCarrySlots);
    for (auto& s : hotbar_) out.push_back(&s);
    out.push_back(&offhandLeft_);
    out.push_back(&offhandRight_);
    for (auto& s : main_) out.push_back(&s);
    return out;
  }

  std::array<ItemStack, kInvHotbarSlots> hotbar_{};
  ItemStack offhandLeft_, offhandRight_;
  std::array<ItemStack, kMainSlots> main_{};
  std::array<ItemStack, kGearSlots> gear_{};
  uint64_t version_ = 0;
};

/// Display name of a stack: the EtchOR name field value when the stack
/// carries one (it OVERWRITES the chip's def name, per the locked design),
/// else the item's def name.
inline std::string stackDisplayName(const ItemStack& s) {
  if (!s.customName.empty()) return s.customName;
  return itemDisplayName(s.itemId);
}

/// Fresh-spawn grant (lore: the Fragment is the starting item -- available
/// day one, first tick; open inventory -> open Fragment -> read/write
/// immediately). Adds the starting item iff the inventory does not already
/// hold one, so it is idempotent: safe on first spawn, on respawn, and on
/// loading a save whose owner somehow lost it (Easy/Normal soulbound rule).
/// Returns true when the item was granted by this call.
inline bool grantStartingItems(Inventory& inv) {
  const ItemDef& start = startingItemDef();
  if (inv.countOf(start.itemId) > 0) return false;
  return inv.addItem(start.itemId, 1) == 0;
}

}  // namespace lithokraft
