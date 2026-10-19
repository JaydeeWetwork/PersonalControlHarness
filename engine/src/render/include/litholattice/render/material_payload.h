// PBR chunk-material save payload (phase 2): the engine payload "MATS".
//
// WHAT: serialises the per-voxel 8-bit material bytes (SubChunk::materials) plus the
//   per-column string-ID table (ChunkColumn::materialPalette) into the chunk record.
// WHY: JD's locked rule — saves persist STRING IDs, never raw compact IDs (compact IDs are
//   registration-order dependent and differ across sessions/mods). On load the strings resolve
//   through the MaterialRegistry, so a save written when iron was ID 13 loads correctly when
//   iron is ID 1.
// HOW: the game registers it — WorldSave adds makeMaterialPayload(*materials) to its
//   PayloadRegistry (the game owns the registry instance; see initMaterialRegistry). It is a
//   separate payload tag rather than a BLKS version bump, so old saves (no MATS payload) keep
//   loading: they simply get the default palette (every byte -> global ID 0, the built-in
//   default). No migration chain needed.
//
// MATS v1 layout: `u16 tableSize (1..256)`, then per entry `u16 len, len bytes` (the string-ID
// table in voxel-index order), `u64 presentMask` (same convention as BLKS: bit per non-empty
// sub-chunk), then per present sub-chunk the material bytes, palette + bit-packed (u16 entry
// count, u8 entries, ceil(log2 n)-bit indices; a uniform sub-chunk takes 3 bytes).
// Load rules: tableSize > 256 is an error (phase-1 lock: overflow is an error, never silent
// truncation). Unknown string IDs resolve to the defined fallback kDefaultMaterialStringId
// (initMaterialRegistry guarantees it is registered; if it is somehow absent the load fails
// loudly rather than inventing an ID). A MATS sub-chunk with no BLKS counterpart is corrupt.
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include "litholattice/render/pbr_materials.h"
#include "litholattice/storage/storage.h"
#include "litholattice/world/chunk.h"

namespace litholattice {

/// Engine payload tag/version for chunk materials (see above).
constexpr uint32_t kMaterialsTag = storage::fourcc("MATS");
constexpr uint16_t kMaterialsVersion = 1;

namespace detail {

/// 8-bit palette codec for material bytes: mirrors storage's 16-bit encodePalette, but the
/// values are already bytes so the entry table is u8.
inline void encodeMatBytes(const uint8_t* v, storage::ByteWriter& w) {
  std::array<uint8_t, 256> entryOf;
  entryOf.fill(0xFF);
  std::vector<uint8_t> entries;
  entries.reserve(256);
  for (int i = 0; i < kSubChunkVolume; ++i) {
    const uint8_t x = v[i];
    if (entryOf[x] == 0xFF) {
      entryOf[x] = uint8_t(entries.size());
      entries.push_back(x);
    }
  }
  w.u16(uint16_t(entries.size()));
  for (const uint8_t e : entries) w.u8(e);
  int bits = 0;
  while ((size_t(1) << bits) < entries.size()) ++bits;
  if (!bits) return;  // uniform: no index bits, same convention as encodePalette
  const size_t words = (size_t(kSubChunkVolume) * size_t(bits) + 63) / 64;
  std::vector<uint64_t> packed(words, 0);
  for (int i = 0; i < kSubChunkVolume; ++i) {
    const uint64_t x = entryOf[v[i]];
    const size_t bit = size_t(i) * size_t(bits), wi = bit / 64, sh = bit % 64;
    packed[wi] |= x << sh;
    if (sh + size_t(bits) > 64) packed[wi + 1] |= x >> (64 - sh);
  }
  for (const uint64_t p : packed) w.u64(p);
}

inline bool decodeMatBytes(storage::ByteReader& r, uint8_t* v, std::string* err) {
  const uint16_t n = r.u16();
  if (!r.ok() || n == 0 || n > 256) {
    if (err) *err = "bad material byte-table size";
    return false;
  }
  uint8_t entries[256];
  for (uint16_t i = 0; i < n; ++i) entries[i] = r.u8();
  int bits = 0;
  while ((size_t(1) << bits) < n) ++bits;
  if (!bits) {
    std::fill(v, v + kSubChunkVolume, entries[0]);
    return r.ok();
  }
  const size_t words = (size_t(kSubChunkVolume) * size_t(bits) + 63) / 64;
  std::vector<uint64_t> packed(words);
  for (auto& p : packed) p = r.u64();
  if (!r.ok()) {
    if (err) *err = "truncated material bytes";
    return false;
  }
  const uint64_t mask = (uint64_t(1) << bits) - 1;
  for (int i = 0; i < kSubChunkVolume; ++i) {
    const size_t bit = size_t(i) * size_t(bits), wi = bit / 64, sh = bit % 64;
    uint64_t x = packed[wi] >> sh;
    if (sh + size_t(bits) > 64) x |= packed[wi + 1] << (64 - sh);
    x &= mask;
    if (x >= n) {
      if (err) *err = "material byte index out of range";
      return false;
    }
    v[i] = entries[size_t(x)];
  }
  return true;
}

}  // namespace detail

/// Builds the engine's "MATS" payload for `reg`. The save/load lambdas capture `reg` by
/// reference: the registry (owned by the game, e.g. LithoKraft's main()) must outlive the
/// PayloadRegistry / WorldStore using this payload.
inline storage::PayloadType makeMaterialPayload(const MaterialRegistry& reg) {
  storage::PayloadType mats;
  mats.tag = kMaterialsTag;
  mats.version = kMaterialsVersion;
  mats.save = [&reg](const ChunkColumn& c) {
    storage::ByteWriter w;
    bool any = false;
    for (const auto& s : c.subs)
      if (s && s->nonAir) {
        any = true;
        break;
      }
    if (!any) return w.out;  // all air: no material state worth persisting
    // Per-column string table in voxel-index order (saves persist STRING IDs, never compact).
    std::vector<std::string> table;
    const std::string* dflt = reg.stringIdFor(0);
    const std::string dfltName = dflt ? *dflt : std::string(kDefaultMaterialStringId);
    // Defensive: the in-memory invariant is 0..256 entries; clamp rather than read OOB.
    const size_t palSize = std::min(c.materialPaletteSize, size_t(256));
    const bool emptyPalette = palSize == 0;
    if (emptyPalette) {
      table.push_back(dfltName);  // empty palette: every byte is index 0 -> the default
    } else {
      table.reserve(palSize);
      for (size_t i = 0; i < palSize; ++i) {
        const std::string* s = reg.stringIdFor(c.materialPalette[i]);
        table.push_back(s ? *s : dfltName);  // stale in-memory ID: the defined fallback
      }
    }
    w.u16(uint16_t(table.size()));
    for (const std::string& s : table) {
      w.u16(uint16_t(s.size()));  // string IDs are short; the 64 KiB cap is plenty
      for (const char ch : s) w.u8(uint8_t(ch));
    }
    uint64_t present = 0;
    for (int s = 0; s < kSubChunks; ++s)
      if (c.subs[size_t(s)] && c.subs[size_t(s)]->nonAir) present |= uint64_t(1) << s;
    w.u64(present);
    // Bytes are meaningless without a palette, so an empty palette normalises them to index 0
    // (the engine never writes a record whose bytes index past its own table).
    static const uint8_t kZeroBytes[kSubChunkVolume] = {};
    for (int s = 0; s < kSubChunks; ++s)
      if (present >> s & 1)
        detail::encodeMatBytes(emptyPalette ? kZeroBytes : c.subs[size_t(s)]->materials.data(), w);
    return w.out;
  };
  mats.load = [&reg](const storage::Bytes& b, ChunkColumn& c) {
    if (b.empty()) return true;  // all-air column at save time: keep the default empty palette
    storage::ByteReader r(b.data(), b.size());
    std::string err;
    const uint16_t n = r.u16();
    if (!r.ok() || n == 0 || n > 256) return false;  // overflow is an error, never silent truncation
    std::vector<std::string> table;
    table.reserve(n);
    for (uint16_t i = 0; i < n; ++i) {
      const uint16_t len = r.u16();
      if (!r.ok() || len > 4096) return false;
      std::string s;
      s.reserve(len);
      for (uint16_t k = 0; k < len; ++k) s.push_back(char(r.u8()));
      if (!r.ok()) return false;
      table.push_back(std::move(s));
    }
    const uint64_t present = r.u64();
    if (!r.ok() || (present >> kSubChunks)) return false;
    // Resolve through the registry; unknown strings -> the defined fallback (never garbage).
    const std::string fallback(kDefaultMaterialStringId);
    std::vector<uint8_t> remap(n, 0xFF);  // table position -> final position
    std::vector<std::string> finalTable;
    finalTable.reserve(n + 1);
    for (uint16_t i = 0; i < n; ++i)
      if (reg.idFor(table[i]) != MaterialRegistry::kInvalidMaterialId) {
        remap[i] = uint8_t(finalTable.size());
        finalTable.push_back(table[i]);
      }
    uint8_t fallbackPos = 0xFF;
    for (uint16_t i = 0; i < n; ++i)
      if (remap[i] == 0xFF) {
        if (fallbackPos == 0xFF) {
          for (size_t j = 0; j < finalTable.size(); ++j)
            if (finalTable[j] == fallback) {
              fallbackPos = uint8_t(j);
              break;
            }
          if (fallbackPos == 0xFF) {
            if (finalTable.size() >= 256) return false;  // appending would overflow: error, locked
            fallbackPos = uint8_t(finalTable.size());
            finalTable.push_back(fallback);
          }
        }
        remap[i] = fallbackPos;
      }
    ChunkPalette pal;
    if (!pal.build(reg, finalTable, &err)) return false;  // every string known by construction
    // Attach bytes to the sub-chunks BLKS already created (BLKS sorts before MATS by tag).
    for (int s = 0; s < kSubChunks; ++s) {
      if (!(present >> s & 1)) continue;
      auto& sub = c.subs[size_t(s)];
      if (!sub) return false;  // material data with no block data: corrupt record
      uint8_t bytes[kSubChunkVolume];
      if (!detail::decodeMatBytes(r, bytes, &err)) return false;
      for (int i = 0; i < kSubChunkVolume; ++i) {
        const uint8_t t = bytes[i];
        if (t >= n) return false;
        sub->materials[i] = remap[t];
      }
    }
    if (!r.done()) return false;
    for (size_t i = 0; i < pal.size(); ++i) c.materialPalette[i] = pal.globalId(uint8_t(i));
    c.materialPaletteSize = pal.size();
    return true;
  };
  return mats;
}

}  // namespace litholattice
