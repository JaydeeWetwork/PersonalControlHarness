// Block types are data: one BlockDef row per block in a BlockRegistry instance that the game builds
// at startup and passes to the engine systems that need it (worldgen, sim thread, mesher,
// materials). The engine knows exactly one block, kAir (id 0, always present); every other block,
// its id, its game-specific role and its flags above kFirstGameFlag belong to the game.
#pragma once
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "litholattice/core/log.h"
#include "litholattice/world/hash.h"

namespace litholattice {

using BlockId = uint16_t;
constexpr BlockId kAir = 0;

/// Infinite Registry (phase 2): the compact machine identity of a content entry
/// (block, item, or a future IMod/IExtensible kind). 64 bits: small enough for
/// LEB128 varints on the wire, 2^64 large enough to never exhaust.
/// Default-derivation is FNV-1a-64 of `kindName + ":" + namespacedId` (see
/// stableIdFor() in hash.h) — deterministic across sessions; mods/IExtensible/the
/// engine may supply their own instead (duplicates rejected). The human-readable
/// string id stays canonical.
///
/// MIGRATION (phase 1 -> 2): phase 1 derived block ids as FNV-1a-64(stringId) with
/// no kind prefix. Phase 2 prefixes the kind, so every stable id changed (air:
/// 0x82ca3801816336c9 -> fnv1a64("block:litholattice:air")). Stable ids are never
/// persisted — saves store string ids via the LLBN name table and re-derive per
/// session; the wire re-derives per session too — so no save/world migration was
/// needed. But any hardcoded phase-1 stable-id constant is now invalid.
using StableId = uint64_t;
inline constexpr StableId kAirStableId = fnv1a64("block:litholattice:air");
inline constexpr bool isAirStableId(StableId id) { return id == kAirStableId; }

/// Content kinds in the unified registry (Infinite Registry, phase 2).
///
/// The registry is one table keyed by (kind, namespaced string id); the stable id
/// folds the kind name in, so `lithokraft:copper` can be both a block and an item
/// with no collision. Engine kinds are fixed below; mods and IExtensible nested
/// extensions add kinds at runtime via ContentRegistry::registerKind() — the same
/// door, no new plumbing (see "Adding a content kind" on ContentRegistry).
///
/// Numeric kind ids are SESSION-LOCAL; only the kind NAME is stable (it feeds
/// stable-id derivation). Never persist the number.
enum class EntryKind : uint16_t {
  Block = 0,
  Item = 1,
  // Values 2..65535: future engine kinds and extension-registered kinds.
};

/// Engine-interpreted flags (bits 0..15). Bits 16..31 are free for the game (kFirstGameFlag << n).
enum BlockFlags : uint32_t {
  kSolid       = 1u << 0,  // occupies the cell (raycasts stop on it, placement needs air)
  kOpaque      = 1u << 1,  // hides neighbour faces (face culling, ambient occlusion)
  kTransparent = 1u << 2,  // light/vision passes through
  kDirectional = 1u << 5,  // aimed on placement (facing = player's horizontal look direction)
  kInteractive = 1u << 6,  // right-click uses it (Shift + right-click still places)
  kRandomTicks = 1u << 7,  // opts in to random ticks (IWorldSimulation::onRandomTick)
  kAttachable  = 1u << 8,
  kFlatPick    = 1u << 9,  // picked (raycast / selection) as a 1/16 plate: on the floor, or under the ceiling when hanging  // mounts on a neighbour's face (attach face in the cell state); pops off with it
  kFirstGameFlag = 1u << 16,
};

/// What the mesher builds. Cube / Slab / Cross are generic engine geometry; Custom blocks are
/// meshed by the game's IBlockAppearance (render/block_appearance.h).
enum class Shape : uint8_t { None, Cube, Cross, Slab, Custom };

// ---- per-cell state (16 bits). The standard layout below is what the engine reads (facing of
// directional blocks; `on` selects a block model's active faces by default). The game gives the
// other bits their meaning.
//  bits 0-6  value   bits 7-8 facing: 0 N(-Z) 1 E(+X) 2 S(+Z) 3 W(-X)   bits 9-11 aux   bit 12 on
using CellState = uint16_t;
constexpr int stValue(CellState s) { return s & 0x7f; }
constexpr int stFacing(CellState s) { return (s >> 7) & 3; }
constexpr int stAux(CellState s) { return (s >> 9) & 7; }
constexpr bool stOn(CellState s) { return (s >> 12) & 1; }
/// Attach face (bits 13-15): the side the support block is on. 0 none (legacy / free-standing),
/// 1 below (-Y), 2 above (+Y), 3 -Z, 4 +Z, 5 -X, 6 +X.
constexpr int stAttach(CellState s) { return (s >> 13) & 7; }
constexpr CellState withAttach(CellState s, int face) { return CellState((s & 0x1fff) | ((face & 7) << 13)); }
constexpr CellState kAttachMask = 0xe000;
/// Offset from the cell to its support for attach face `face` (0 for none).
constexpr void attachOffset(int face, int& dx, int& dy, int& dz) {
  dx = face == 5 ? -1 : face == 6 ? 1 : 0;
  dy = face == 1 ? -1 : face == 2 ? 1 : 0;
  dz = face == 3 ? -1 : face == 4 ? 1 : 0;
}
constexpr int attachFaceFor(int dx, int dy, int dz) {
  return dy < 0 ? 1 : dy > 0 ? 2 : dz < 0 ? 3 : dz > 0 ? 4 : dx < 0 ? 5 : dx > 0 ? 6 : 0;
}
constexpr CellState makeState(int value, int facing = 0, int aux = 0, bool on = false) {
  return CellState((value & 0x7f) | ((facing & 3) << 7) | ((aux & 7) << 9) | (on ? 1 << 12 : 0));
}
// Horizontal directions (facing): 0 N, 1 E, 2 S, 3 W. Relative to a facing f:
// front = f, right = f+1, back = f+2, left = f+3 (seen from above).
constexpr int kDirX[4] = {0, 1, 0, -1}, kDirZ[4] = {-1, 0, 1, 0};
constexpr int dirBack(int f) { return (f + 2) & 3; }
constexpr int dirRight(int f) { return (f + 1) & 3; }
constexpr int dirLeft(int f) { return (f + 3) & 3; }

struct BlockDef {
  std::string_view name;        ///< display name
  uint32_t flags = 0;           ///< BlockFlags | game flags
  Shape shape = Shape::None;
  std::string_view tex{};       ///< texture-pack block name (manifest "block"), e.g. "stone"
  std::string_view behavior{};  ///< game behaviour name ("" = none); the engine never reads it
  // Game-defined payload: the engine stores it and never interprets it.
  uint16_t role = 0;            ///< game's block category (0 = none)
  uint8_t channel = 0;          ///< e.g. a signal colour
  uint8_t tier = 0;             ///< e.g. a palette size
  float friction = 0.8f;        ///< velocity retained per tick when an entity slides on it (1 = ice, 0 = stop)
  bool falls = false;           ///< sand-like: becomes an entity when unsupported
  uint8_t density = 0;          ///< resists projectiles (0 = air, 255 = unbreakable)
  /// Namespaced, stable id ("lithokraft:lamp"): what saves store in their block name table.
  /// Empty: derived as "<anonymous>:<tex or name>" (tests / tools).
  std::string_view id{};
  /// WS7 content fingerprint, COMPUTED AT REGISTRATION (callers leave it 0; add()
  /// always recomputes). CRC32 over the canonical serialization of the def's CONTENT
  /// fields (name, flags, shape, tex, behavior, role/channel/tier, friction, falls,
  /// density) — never the string id, which is identity, not content. Same id +
  /// different CRC between versions = the def changed (migration signal). Exposed
  /// for logging in decimal and hex.
  uint32_t crc32 = 0;
};

/// Runtime item id. uint32_t to match inventory.h's itemId. Id 0 is the empty
/// sentinel (no def is stored for it); real items start at 1. (Inventory
/// migration to StableId currency is a defined builder follow-up; the registry
/// is ready for it.)
using ItemId = uint32_t;

struct ItemDef {
  std::string_view name;        ///< display name
  uint32_t maxStack = 64;
  /// Stable id of the block this item places (a block-item); 0 = not a block-item.
  /// BlockItems are a field, not a mapping table.
  StableId placesBlock = 0;
  std::string_view behavior{};  ///< game behaviour name ("" = none); the engine never reads it
  /// Namespaced, stable id ("lithokraft:copper_ingot"): what saves persist.
  /// Empty: derived as "anonymous:<name>" (tests / tools).
  std::string_view id{};
  /// WS7 content fingerprint, COMPUTED AT REGISTRATION (callers leave it 0).
  /// CRC32 over the canonical serialization of the def's CONTENT fields
  /// (name, maxStack, placesBlock, behavior) — never the string id, which is
  /// identity, not content. Same id + different CRC between versions = the def
  /// changed (migration signal).
  uint32_t crc32 = 0;
};

/// Typed view over a ContentRegistry, filtered to blocks. This is the phase-1
/// BlockRegistry API, unchanged: the 109+ game call sites, ChunkMap, the mesher
/// path and the tests keep working with zero churn. Owns its registry when
/// default-constructed; views an external one when given.
class BlockRegistry;
/// Typed view over a ContentRegistry, filtered to items. Mirrors the BlockRegistry
/// facade: owns its registry when default-constructed, views an external one when
/// given. ItemIds are dense from 1; 0 is the empty sentinel.
class ItemRegistry;

/// The unified content registry (Infinite Registry, phase 2 — JD-locked option B).
///
/// One kind-tagged table for every content kind: blocks, items, and whatever
/// IMod / nested IExtensible extensions register next. The identity machinery
/// (stable ids, string keys, duplicate rejection, CRCs, kindOf) is fully generic
/// over kinds; per-kind def storage is a small typed wrapper around it.
///
/// Threading: built single-threaded at startup (mod-init window), then shared
/// read-only — same contract as the phase-1 BlockRegistry. The contract is
/// ENFORCED, not just documented (review F-M3): in debug builds every
/// mutator asserts it runs on the one thread that first mutated the
/// registry (see noteMutatorThread()); in all builds the def vectors'
/// mutation is serialized against def() readers via defMutex_. Read-only
/// lookups (findBlock/findItem/stableIdOf/...) stay lock-free and are safe
/// only once mutation has finished — the mod-init window rule.
///
/// Adding a content kind (the extension door — no new plumbing):
///   1. `registerKind("fluid")` -> EntryKind (deduplicates on name).
///   2. Add a `std::vector<YourDef>` + typed accessors that funnel through the
///      private generic `insertEntry()` (identity, dup rejection, CRC storage).
///   3. Derive stable ids as `stableIdFor(kindName(kind), stringId)`.
/// That's it: no new id scheme, no new remap path, no new wire codec, no new
/// save table. When forks 2/3 (seal, quarantine) land, they apply once, to all kinds.
class ContentRegistry {
 public:
  ContentRegistry();

  // ---- kinds: the extension door ----
  /// Register a content kind by name ("block"/"item" are pre-registered).
  /// Duplicate names return the existing kind. Numeric ids are session-local.
  /// NOTE (M1): new kinds are an engine-level extension — payload storage is
  /// private, so out-of-engine code cannot store entries of a custom kind.
  /// Mods add blocks/items to the existing kinds (which ARE runtime-usable
  /// until seal()); they do not invent new kinds.
  EntryKind registerKind(std::string_view name);
  /// The stable name behind a kind (feeds stable-id derivation).
  std::string_view kindName(EntryKind k) const;

  // ---- blocks (typed) ----
  /// Appends a block; dense BlockId in registration order. The stable id is
  /// FNV-1a-64("block:" + string id). First registration of a string id wins:
  /// a duplicate returns the EXISTING id — never a silent overwrite.
  BlockId addBlock(const BlockDef& d);
  /// Appends with a caller-supplied stable id (mods, IExtensible, engine entries).
  /// Returns nullopt when the (block,id) OR the stable id — checked globally
  /// across all kinds — is already registered. Originals untouched.
  std::optional<BlockId> addBlockWithStableId(const BlockDef& d, StableId stable);
  void reserveBlocks(size_t n);
  BlockDef blockDef(BlockId id) const;
  StableId blockStableId(BlockId id) const;
  uint32_t blockCrc(BlockId id) const;
  std::string blockStringId(BlockId id) const;
  /// The block with namespaced id `id` (aliases resolve after the direct lookup).
  std::optional<BlockId> findBlock(std::string_view id) const;
  /// The block with stable id `sid`; nullopt when unknown OR not a block
  /// (kind-checked: an item's stable id never resolves as a block).
  std::optional<BlockId> findBlockByStableId(StableId sid) const;
  std::optional<BlockId> findBlockByTexture(std::string_view tex) const;
  bool addBlockAlias(std::string_view oldId, std::string_view newId);
  size_t blockCount() const;

  // ---- items (typed) ----
  /// Appends an item; ItemIds are dense from 1 (0 is the empty sentinel).
  /// Same first-wins duplicate rule as blocks, keyed on (item, string id).
  ItemId addItem(const ItemDef& d);
  /// Caller-supplied stable id; nullopt on (item,id) or global stable-id duplicate.
  std::optional<ItemId> addItemWithStableId(const ItemDef& d, StableId stable);
  /// ItemId 0 / out-of-range -> the empty sentinel def. Returned BY VALUE
  /// under the shared lock, like blockDef (review F-M3): the old const&
  /// return was a reference into a vector addItem can reallocate — the
  /// exact torn-read/UAF pattern the block side had already fixed.
  ItemDef itemDef(ItemId id) const;
  /// 0 for the empty sentinel / out-of-range.
  StableId itemStableId(ItemId id) const;
  uint32_t itemCrc(ItemId id) const;
  std::string itemStringId(ItemId id) const;
  std::optional<ItemId> findItem(std::string_view id) const;
  /// Kind-checked: a block's stable id never resolves as an item.
  std::optional<ItemId> findItemByStableId(StableId sid) const;
  size_t itemCount() const;

  // ---- unified identity ----
  /// The kind behind a stable id; nullopt when unknown. Used for fail-fast kind
  /// checks (e.g. voxel-palette LUT builds assert kind == Block).
  std::optional<EntryKind> kindOf(StableId sid) const;
  /// Stable id for (kind, string id), alias-aware for blocks; nullopt when unknown.
  std::optional<StableId> stableIdOf(EntryKind kind, std::string_view id) const;

  // ---- typed views ----
  BlockRegistry blocks();
  ItemRegistry items();

  // ---- seal & hot-swap (debug prototyping) ----
  /// Seal the registry: addBlock/addItem/stageBlockUpdate refuse afterwards.
  /// Irreversible. The game seals at boot per the certificate trust model
  /// (see main.cpp): sealed:true in any manifest seals; release seals unless
  /// every loaded mod is Verified and one opts into hot_swap (live-ops);
  /// debug stays open unless sealed.
  void seal() { sealed_ = true; }
  bool isSealed() const { return sealed_; }
  /// Stage a replacement definition for an existing block. The live definition
  /// is untouched — in-flight systems keep the old def — until refreshStaged()
  /// applies it between ticks. Returns false when sealed or id is out of range.
  /// The stable ID never changes (derived from the string ID), so voxels pick up
  /// the new definition with no remap after refresh.
  bool stageBlockUpdate(BlockId id, const BlockDef& newDef);
  /// Apply all staged updates. Caller must be between ticks
  /// (SimThread::runBetweenTicks pauses the tick, applies, resumes).
  /// Bumps definitionEpoch(). Returns the number of definitions applied.
  size_t refreshStaged();
  /// Bumped by every refreshStaged() that applies updates. Caches (mesher, UI
  /// icons) compare against their last-seen epoch to invalidate.
  uint64_t definitionEpoch() const { return epoch_.load(std::memory_order_acquire); }
  size_t stagedUpdateCount() const { return staged_.size(); }

 private:
  struct Key {
    EntryKind kind;
    std::string id;
    bool operator==(const Key& o) const { return kind == o.kind && id == o.id; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const noexcept {
      size_t h = std::hash<uint16_t>{}(static_cast<uint16_t>(k.kind));
      h ^= std::hash<std::string>{}(k.id) + 0x9e3779b9u + (h << 6) + (h >> 2);
      return h;
    }
  };
  struct Entry {
    EntryKind kind;
    std::string stringId;   // owned (stringIdOf may build "anonymous:..." temporaries)
    StableId stableId;
    uint32_t crc32;
    uint32_t localIndex;    // index into the kind's def vector (== BlockId, or ItemId-1)
  };

  static std::string blockStringIdOf(const BlockDef& d);
  static std::string itemStringIdOf(const ItemDef& d);
  static uint32_t blockContentCrc(const BlockDef& d);
  static uint32_t itemContentCrc(const ItemDef& d);

  /// Generic insertion door. Rejects (kind,id) and global stable-id duplicates;
  /// appends the entry on success. No partial state on rejection.
  bool insertEntry(EntryKind kind, const std::string& stringId, StableId stable,
                   uint32_t crc, uint32_t localIndex);

  /// Review F-M3 enforcement: records (first call) and thereafter asserts
  /// (debug builds) that every registry mutator runs on that one thread —
  /// the single-threaded mod-init window, made checkable. Release builds
  /// pay one relaxed atomic load + compare. Mutable: the ContentRegistry
  /// ctor's air registration is the usual first mutation.
  void noteMutatorThread() const {
#ifndef NDEBUG
    const std::thread::id self = std::this_thread::get_id();
    std::thread::id expected = mutatorThread_.load(std::memory_order_relaxed);
    if (expected == std::thread::id{}) {
      mutatorThread_.store(self, std::memory_order_relaxed);
      return;
    }
    assert(expected == self &&
           "ContentRegistry: mutators must run on one thread (single-threaded mod-init window)");
#endif
  }
  mutable std::atomic<std::thread::id> mutatorThread_{};

  std::vector<std::string> kindNames_;  // index == uint16_t(kind); [0]="block", [1]="item"
  std::vector<Entry> entries_;                                        // global insertion order
  std::unordered_map<StableId, uint32_t> byStable_;                   // stable -> entries_ idx
  std::unordered_map<Key, uint32_t, KeyHash> byKey_;                  // (kind,id) -> entries_ idx
  std::unordered_map<Key, std::string, KeyHash> aliases_;             // (kind,oldId) -> newId
  std::vector<BlockDef> blockDefs_;
  std::vector<uint32_t> blockEntryIdx_;  // BlockId -> entries_ idx
  std::vector<ItemDef> itemDefs_;
  std::vector<uint32_t> itemEntryIdx_;   // (ItemId-1) -> entries_ idx; id 0 = empty sentinel
  inline static const ItemDef kEmptyItem{};
  bool sealed_ = false;
  // Hot-swap synchronization: blockDefs_ is read on mesh/UI/sim threads and
  // written only by refreshStaged() (between ticks). Readers take a shared lock;
  // refreshStaged() takes the unique lock. blockDef() returns BY VALUE so no
  // caller can hold a reference across a refresh (torn reads were UB).
  mutable std::shared_mutex defMutex_;
  std::atomic<uint64_t> epoch_ = 0;
  std::unordered_map<BlockId, BlockDef> staged_;  // BlockId -> pending replacement def
};

inline ContentRegistry::ContentRegistry() {
  kindNames_.push_back("block");
  kindNames_.push_back("item");
  // Row 0 is always kAir, as in phase 1 — now with its kind-tagged stable id.
  BlockDef air{"Air", kTransparent, Shape::None, {}, {}, 0, 0, 0, 0.8f, false, 0,
               "litholattice:air"};
  [[maybe_unused]] const auto id = addBlockWithStableId(air, kAirStableId);
  assert(id && *id == kAir);
}

inline EntryKind ContentRegistry::registerKind(std::string_view name) {
  noteMutatorThread();
  for (size_t i = 0; i < kindNames_.size(); ++i)
    if (kindNames_[i] == name) return EntryKind(static_cast<uint16_t>(i));
  kindNames_.emplace_back(name);
  return EntryKind(static_cast<uint16_t>(kindNames_.size() - 1));
}

inline std::string_view ContentRegistry::kindName(EntryKind k) const {
  const size_t i = static_cast<uint16_t>(k);
  return i < kindNames_.size() ? std::string_view(kindNames_[i]) : std::string_view{};
}

inline std::string ContentRegistry::blockStringIdOf(const BlockDef& d) {
  if (!d.id.empty()) return std::string(d.id);
  return "anonymous:" + std::string(d.tex.empty() ? d.name : d.tex);
}

inline std::string ContentRegistry::itemStringIdOf(const ItemDef& d) {
  if (!d.id.empty()) return std::string(d.id);
  return "anonymous:" + std::string(d.name);
}

/// WS7: CRC32 over the canonical serialization of the def's CONTENT fields.
/// Explicit little-endian, length-prefixed strings, field-by-field (no struct
/// padding): byte-identical on every platform for equal defs. Excludes the
/// string id (identity, not content) and the crc32 field itself.
inline uint32_t ContentRegistry::blockContentCrc(const BlockDef& d) {
  std::string buf;
  buf.reserve(96);
  const auto putU8 = [&](uint32_t v) { buf.push_back(char(v & 0xFF)); };
  const auto putU16 = [&](uint32_t v) { putU8(v); putU8(v >> 8); };
  const auto putU32 = [&](uint32_t v) { putU16(v); putU16(v >> 16); };
  const auto putStr = [&](std::string_view s) {
    putU32(uint32_t(s.size()));
    buf.append(s.data(), s.size());
  };
  putStr(d.name);
  putU32(d.flags);
  putU8(static_cast<uint32_t>(d.shape));
  putStr(d.tex);
  putStr(d.behavior);
  putU16(d.role);
  putU8(d.channel);
  putU8(d.tier);
  uint32_t frictionBits = 0;
  static_assert(sizeof(frictionBits) == sizeof(d.friction));
  std::memcpy(&frictionBits, &d.friction, sizeof(frictionBits));
  putU32(frictionBits);
  putU8(d.falls ? 1 : 0);
  putU8(d.density);
  return crc32(buf);
}

inline uint32_t ContentRegistry::itemContentCrc(const ItemDef& d) {
  std::string buf;
  buf.reserve(64);
  const auto putU8 = [&](uint32_t v) { buf.push_back(char(v & 0xFF)); };
  const auto putU16 = [&](uint32_t v) { putU8(v); putU8(v >> 8); };
  const auto putU32 = [&](uint32_t v) { putU16(v); putU16(v >> 16); };
  const auto putU64 = [&](uint64_t v) { putU32(uint32_t(v)); putU32(uint32_t(v >> 32)); };
  const auto putStr = [&](std::string_view s) {
    putU32(uint32_t(s.size()));
    buf.append(s.data(), s.size());
  };
  putStr(d.name);
  putU32(d.maxStack);
  putU64(d.placesBlock);
  putStr(d.behavior);
  return crc32(buf);
}

inline bool ContentRegistry::insertEntry(EntryKind kind, const std::string& stringId,
                                         StableId stable, uint32_t crc,
                                         uint32_t localIndex) {
  noteMutatorThread();
  const Key key{kind, stringId};
  if (byKey_.find(key) != byKey_.end()) return false;      // (kind,id) taken: first wins
  if (byStable_.find(stable) != byStable_.end()) return false;  // stable id taken globally
  const uint32_t e = static_cast<uint32_t>(entries_.size());
  byKey_.emplace(key, e);
  byStable_.emplace(stable, e);
  entries_.push_back(Entry{kind, stringId, stable, crc, localIndex});
  return true;
}

inline BlockId ContentRegistry::addBlock(const BlockDef& d) {
  noteMutatorThread();
  if (sealed_) throw std::runtime_error("ContentRegistry::addBlock: registry is sealed");
  const std::string sid = blockStringIdOf(d);
  if (const auto existing = findBlock(sid)) return *existing;  // first-wins
  const StableId derived = stableIdFor(kindName(EntryKind::Block), sid);
  // A derived collision with a *different* entry would be a 64-bit hash
  // collision (birthday bound ~5 billion entries). Review F-L11: the old
  // code guarded this with assert + .value(), so a release build threw
  // bad_optional_access uncaught at registration. Now: a LOGGED REFUSAL —
  // kAir is returned and the colliding def is not registered.
  if (byStable_.find(derived) != byStable_.end()) {
    logError("registry: addBlock('%s') refused: derived stable id collides with an existing entry",
             sid.c_str());
    return kAir;
  }
  const auto id = addBlockWithStableId(d, derived);
  if (!id) {  // e.g. the BlockId cap (F-M5) refused it: already logged there
    logError("registry: addBlock('%s') refused", sid.c_str());
    return kAir;
  }
  return *id;
}

inline std::optional<BlockId> ContentRegistry::addBlockWithStableId(const BlockDef& d,
                                                                    StableId stable) {
  noteMutatorThread();
  if (sealed_) return std::nullopt;
  // BlockId cap (review F-M5): BlockId is uint16_t, so local index 0x10000
  // would truncate to 0 (air) and silently alias the new block onto an
  // existing id. Fail closed with a logged refusal BEFORE any insertion.
  if (blockDefs_.size() > 0xFFFFu) {
    logError("registry: addBlock refused: block capacity exceeded (BlockId is 16-bit, max 65536 blocks)");
    return std::nullopt;
  }
  const std::string sid = blockStringIdOf(d);
  const uint32_t local = static_cast<uint32_t>(blockDefs_.size());
  BlockDef def = d;
  def.crc32 = blockContentCrc(def);
  if (!insertEntry(EntryKind::Block, sid, stable, def.crc32, local)) return std::nullopt;
  {
    // Serialize the def-vector growth against blockDef()'s shared-lock
    // readers (F-M3): the pair is what makes the by-value read sound.
    std::unique_lock lock(defMutex_);
    blockDefs_.push_back(def);
    blockEntryIdx_.push_back(static_cast<uint32_t>(entries_.size()) - 1);
  }
  return BlockId(local);
}

inline void ContentRegistry::reserveBlocks(size_t n) {
  noteMutatorThread();
  blockDefs_.reserve(n);
  blockEntryIdx_.reserve(n);
}

inline BlockDef ContentRegistry::blockDef(BlockId id) const {
  std::shared_lock lock(defMutex_);
  return id < blockDefs_.size() ? blockDefs_[id] : blockDefs_[0];  // air fallback, as before
}

inline StableId ContentRegistry::blockStableId(BlockId id) const {
  return id < blockEntryIdx_.size() ? entries_[blockEntryIdx_[id]].stableId : kAirStableId;
}

inline uint32_t ContentRegistry::blockCrc(BlockId id) const { return blockDef(id).crc32; }

inline std::string ContentRegistry::blockStringId(BlockId id) const {
  return id < blockEntryIdx_.size() ? entries_[blockEntryIdx_[id]].stringId
                                    : entries_[0].stringId;  // air
}

inline std::optional<BlockId> ContentRegistry::findBlock(std::string_view id) const {
  // Iterative alias resolution with a cycle guard: a mod-crafted alias cycle
  // must not recurse unbounded (stack overflow). Any chain longer than the
  // alias table itself necessarily repeats, so cap the hops there.
  std::string cur(id);
  const size_t maxHops = aliases_.size() + 1;
  for (size_t hop = 0; hop <= maxHops; ++hop) {
    const Key key{EntryKind::Block, cur};
    const auto it = byKey_.find(key);
    if (it != byKey_.end()) return BlockId(entries_[it->second].localIndex);
    const auto al = aliases_.find(key);
    if (al == aliases_.end()) return std::nullopt;
    cur = al->second;
  }
  return std::nullopt;  // cycle: treat as unresolvable rather than overflowing
}

inline std::optional<BlockId> ContentRegistry::findBlockByStableId(StableId sid) const {
  const auto it = byStable_.find(sid);
  if (it == byStable_.end()) return std::nullopt;
  const Entry& e = entries_[it->second];
  if (e.kind != EntryKind::Block) return std::nullopt;  // kind-checked
  return BlockId(e.localIndex);
}

inline std::optional<BlockId> ContentRegistry::findBlockByTexture(std::string_view tex) const {
  if (tex.empty()) return std::nullopt;
  std::shared_lock lock(defMutex_);
  for (size_t i = 0; i < blockDefs_.size(); ++i)
    if (blockDefs_[i].tex == tex) return BlockId(i);
  return std::nullopt;
}

inline bool ContentRegistry::addBlockAlias(std::string_view oldId, std::string_view newId) {
  noteMutatorThread();  // review F-M3: aliases_ was mutated with no enforcement at all
  if (oldId.empty() || oldId == newId) return false;
  // Reject alias cycles at registration: walk the chain from newId; if it
  // reaches oldId, adding oldId -> newId would close a loop.
  std::string cur(newId);
  const size_t maxHops = aliases_.size() + 1;
  for (size_t hop = 0; hop <= maxHops; ++hop) {
    if (cur == oldId) return false;  // cycle: refuse
    const auto al = aliases_.find(Key{EntryKind::Block, cur});
    if (al == aliases_.end()) break;
    cur = al->second;
  }
  aliases_[Key{EntryKind::Block, std::string(oldId)}] = std::string(newId);
  return true;
}

inline size_t ContentRegistry::blockCount() const { return blockDefs_.size(); }

inline bool ContentRegistry::stageBlockUpdate(BlockId id, const BlockDef& newDef) {
  if (sealed_) return false;
  // Unique lock: staged_ is WRITTEN here. A shared lock would let two staging
  // threads race on the map.
  std::unique_lock lock(defMutex_);
  if (id >= blockDefs_.size()) return false;
  // The string ID is the identity: a staged def must name the same block.
  // (A rename is a remove+add, not an update.)
  if (blockStringIdOf(newDef) != blockStringIdOf(blockDefs_[id])) return false;
  staged_[id] = newDef;
  return true;
}

inline size_t ContentRegistry::refreshStaged() {
  // Unique lock FIRST: checking staged_.empty() before the lock races with a
  // concurrent stageBlockUpdate write. No reader sees a torn BlockDef
  // mid-refresh. The epoch is bumped BEFORE the lock releases, so the
  // (defs, epoch) pair is always consistent: caches comparing
  // definitionEpoch() invalidate exactly once.
  std::unique_lock lock(defMutex_);
  if (staged_.empty()) return 0;
  for (auto& [id, def] : staged_) {
    BlockDef applied = def;
    applied.crc32 = blockContentCrc(applied);
    blockDefs_[id] = applied;
    entries_[blockEntryIdx_[id]].crc32 = applied.crc32;
  }
  const size_t n = staged_.size();
  staged_.clear();
  epoch_.fetch_add(1, std::memory_order_release);
  return n;
}

inline ItemId ContentRegistry::addItem(const ItemDef& d) {
  noteMutatorThread();
  if (sealed_) throw std::runtime_error("ContentRegistry::addItem: registry is sealed");
  const std::string sid = itemStringIdOf(d);
  if (const auto existing = findItem(sid)) return *existing;  // first-wins
  const StableId derived = stableIdFor(kindName(EntryKind::Item), sid);
  assert(byStable_.find(derived) == byStable_.end());
  return addItemWithStableId(d, derived).value();
}

inline std::optional<ItemId> ContentRegistry::addItemWithStableId(const ItemDef& d,
                                                                  StableId stable) {
  noteMutatorThread();
  if (sealed_) return std::nullopt;
  const std::string sid = itemStringIdOf(d);
  const uint32_t local = static_cast<uint32_t>(itemDefs_.size());
  ItemDef def = d;
  def.crc32 = itemContentCrc(def);
  if (!insertEntry(EntryKind::Item, sid, stable, def.crc32, local)) return std::nullopt;
  {
    // Serialized against itemDef()'s shared-lock by-value reads (F-M3).
    std::unique_lock lock(defMutex_);
    itemDefs_.push_back(def);
    itemEntryIdx_.push_back(static_cast<uint32_t>(entries_.size()) - 1);
  }
  return ItemId(local + 1);  // ids start at 1; 0 is the empty sentinel
}

inline ItemDef ContentRegistry::itemDef(ItemId id) const {
  // By value under the shared lock (review F-M3), mirroring blockDef().
  std::shared_lock lock(defMutex_);
  if (id == 0 || size_t(id - 1) >= itemDefs_.size()) return kEmptyItem;
  return itemDefs_[id - 1];
}

inline StableId ContentRegistry::itemStableId(ItemId id) const {
  if (id == 0 || size_t(id - 1) >= itemEntryIdx_.size()) return 0;
  return entries_[itemEntryIdx_[id - 1]].stableId;
}

inline uint32_t ContentRegistry::itemCrc(ItemId id) const { return itemDef(id).crc32; }

inline std::string ContentRegistry::itemStringId(ItemId id) const {
  if (id == 0 || size_t(id - 1) >= itemEntryIdx_.size()) return {};
  return entries_[itemEntryIdx_[id - 1]].stringId;
}

inline std::optional<ItemId> ContentRegistry::findItem(std::string_view id) const {
  const Key key{EntryKind::Item, std::string(id)};
  const auto it = byKey_.find(key);
  if (it == byKey_.end()) return std::nullopt;
  return ItemId(entries_[it->second].localIndex + 1);
}

inline std::optional<ItemId> ContentRegistry::findItemByStableId(StableId sid) const {
  const auto it = byStable_.find(sid);
  if (it == byStable_.end()) return std::nullopt;
  const Entry& e = entries_[it->second];
  if (e.kind != EntryKind::Item) return std::nullopt;  // kind-checked
  return ItemId(e.localIndex + 1);
}

inline size_t ContentRegistry::itemCount() const { return itemDefs_.size(); }

inline std::optional<EntryKind> ContentRegistry::kindOf(StableId sid) const {
  const auto it = byStable_.find(sid);
  if (it == byStable_.end()) return std::nullopt;
  return entries_[it->second].kind;
}

inline std::optional<StableId> ContentRegistry::stableIdOf(EntryKind kind,
                                                           std::string_view id) const {
  if (kind == EntryKind::Block) {
    const auto b = findBlock(id);
    return b ? std::optional<StableId>(blockStableId(*b)) : std::nullopt;
  }
  if (kind == EntryKind::Item) {
    const auto i = findItem(id);
    return i ? std::optional<StableId>(itemStableId(*i)) : std::nullopt;
  }
  const Key key{kind, std::string(id)};
  const auto it = byKey_.find(key);
  return it == byKey_.end() ? std::nullopt : std::optional<StableId>(entries_[it->second].stableId);
}

/// Typed view over a ContentRegistry, filtered to blocks. This is the phase-1
/// BlockRegistry API, unchanged: the 109+ game call sites, ChunkMap, the mesher
/// path and the tests keep working with zero churn. Owns its registry when
/// default-constructed; views an external one when given.
class BlockRegistry {
 public:
  BlockRegistry() : owned_(std::make_unique<ContentRegistry>()), reg_(owned_.get()) {}
  explicit BlockRegistry(ContentRegistry& content) : reg_(&content) {}
  BlockRegistry(BlockRegistry&&) noexcept = default;
  BlockRegistry& operator=(BlockRegistry&&) noexcept = default;

  /// Appends a block and returns its id (ids are dense, in registration order).
  /// The stable id is FNV-1a-64("block:" + string id) (deterministic).
  /// First registration of a string id wins: a duplicate returns the EXISTING id
  /// without modifying the entry — never a silent overwrite.
  BlockId add(const BlockDef& d) { return reg_->addBlock(d); }
  /// Appends with a caller-supplied stable id. Returns nullopt when the string id
  /// OR the stable id (checked globally, across kinds) is already registered.
  std::optional<BlockId> addWithStableId(const BlockDef& d, StableId stable) {
    return reg_->addBlockWithStableId(d, stable);
  }
  void reserve(size_t n) { reg_->reserveBlocks(n); }
  BlockDef def(BlockId id) const { return reg_->blockDef(id); }
  /// def() by stable id; the air def when unknown or not a block (in-memory voxels
  /// always resolve; unknown here means a programming error upstream, and air is
  /// the safe answer).
  BlockDef defByStableId(StableId sid) const {
    const auto id = findByStableId(sid);
    return def(id ? *id : kAir);
  }
  bool has(BlockId id, uint32_t f) const { return (def(id).flags & f) != 0; }     // any of f
  bool hasAll(BlockId id, uint32_t f) const { return (def(id).flags & f) == f; }  // all of f
  size_t size() const { return reg_->blockCount(); }
  /// The block's namespaced id (BlockDef::id, or "anonymous:<tex|name>" when unset).
  std::string idOf(BlockId id) const { return reg_->blockStringId(id); }
  /// The block's 64-bit stable id. kAirStableId for out-of-range (mirrors def()).
  StableId stableIdOf(BlockId id) const { return reg_->blockStableId(id); }
  /// The block's WS7 content fingerprint. Exposed for logging (decimal and hex).
  uint32_t crcOf(BlockId id) const { return reg_->blockCrc(id); }
  /// The block with namespaced id `id`; nullopt if none. Aliases (addAlias)
  /// resolve after the direct lookup, so renamed blocks load from old saves.
  std::optional<BlockId> findById(std::string_view id) const { return reg_->findBlock(id); }
  /// The block with 64-bit stable id `sid`; nullopt when unknown or not a block.
  std::optional<BlockId> findByStableId(StableId sid) const {
    return reg_->findBlockByStableId(sid);
  }
  /// Record that saves may contain `oldId` for the block now known as `newId`.
  /// Returns false when the alias would create a cycle (refused).
  bool addAlias(std::string_view oldId, std::string_view newId) {
    return reg_->addBlockAlias(oldId, newId);
  }
  /// The block whose texture key (BlockDef::tex) is `tex`, e.g. "stone"; nullopt if none.
  std::optional<BlockId> findByTexture(std::string_view tex) const {
    return reg_->findBlockByTexture(tex);
  }
  /// Unified-registry hook: the entry kind behind a stable id (nullopt = unknown).
  /// Voxel-palette LUT builds use this for the fail-fast kind check.
  std::optional<EntryKind> kindOf(StableId sid) const { return reg_->kindOf(sid); }
  /// The unified registry underneath (items, future kinds).
  ContentRegistry& content() { return *reg_; }
  const ContentRegistry& content() const { return *reg_; }

 private:
  std::unique_ptr<ContentRegistry> owned_;  // set when default-constructed
  ContentRegistry* reg_;
};

/// Typed view over a ContentRegistry, filtered to items. Mirrors the BlockRegistry
/// facade: owns its registry when default-constructed, views an external one when
/// given. ItemIds are dense from 1; 0 is the empty sentinel.
class ItemRegistry {
 public:
  ItemRegistry() : owned_(std::make_unique<ContentRegistry>()), reg_(owned_.get()) {}
  explicit ItemRegistry(ContentRegistry& content) : reg_(&content) {}
  ItemRegistry(ItemRegistry&&) noexcept = default;
  ItemRegistry& operator=(ItemRegistry&&) noexcept = default;

  ItemId add(const ItemDef& d) { return reg_->addItem(d); }
  std::optional<ItemId> addWithStableId(const ItemDef& d, StableId stable) {
    return reg_->addItemWithStableId(d, stable);
  }
  /// ItemId 0 / out-of-range -> the empty sentinel def. By value, like
  /// ContentRegistry::itemDef (review F-M3).
  ItemDef def(ItemId id) const { return reg_->itemDef(id); }
  size_t size() const { return reg_->itemCount(); }
  std::string idOf(ItemId id) const { return reg_->itemStringId(id); }
  StableId stableIdOf(ItemId id) const { return reg_->itemStableId(id); }
  uint32_t crcOf(ItemId id) const { return reg_->itemCrc(id); }
  std::optional<ItemId> findById(std::string_view id) const { return reg_->findItem(id); }
  /// Kind-checked: a block's stable id never resolves as an item.
  std::optional<ItemId> findByStableId(StableId sid) const {
    return reg_->findItemByStableId(sid);
  }
  std::optional<EntryKind> kindOf(StableId sid) const { return reg_->kindOf(sid); }
  ContentRegistry& content() { return *reg_; }
  const ContentRegistry& content() const { return *reg_; }

 private:
  std::unique_ptr<ContentRegistry> owned_;  // set when default-constructed
  ContentRegistry* reg_;
};

inline BlockRegistry ContentRegistry::blocks() { return BlockRegistry(*this); }
inline ItemRegistry ContentRegistry::items() { return ItemRegistry(*this); }

}  // namespace litholattice
