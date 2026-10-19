// Hash utilities for the Infinite Registry (phase 1).
//
// fnv1a64: the deterministic 64-bit hash used to derive a block/item's stable ID
//   from its namespaced string ID. constexpr so well-known IDs (air) are compile-time.
// crc32: standard CRC-32/ISO-HDLC (poly 0xEDB88320, init/xorout 0xFFFFFFFF) — the same
//   algorithm as litholattice::storage::crc32, so fingerprints agree across modules.
//   Used for per-entry BlockDef content fingerprints (WS7). NOT the string id: the id
//   is identity, the CRC is content.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace litholattice {

/// FNV-1a 64-bit. Standard offset basis 14695981039346656037, prime 1099511628211.
constexpr uint64_t fnv1a64(std::string_view s) noexcept {
  uint64_t h = 14695981039346656037ULL;
  for (char c : s) {
    h ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
    h *= 1099511628211ULL;
  }
  return h;
}

/// Standard CRC-32 over `n` bytes. Deterministic across platforms (byte-wise, no padding).
uint32_t crc32(const uint8_t* data, size_t n);

inline uint32_t crc32(std::string_view s) {
  return crc32(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

/// Kind-tagged stable ID derivation (Infinite Registry, phase 2): FNV-1a-64 over
/// `kindName + ":" + namespacedId`, fed piecewise — byte-identical to hashing the
/// joined string, because FNV-1a is byte-sequential. The kind prefix keeps every
/// content kind (block, item, future IMod/IExtensible kinds) in one 64-bit space
/// with no cross-kind collisions. Deterministic across sessions and platforms.
inline uint64_t stableIdFor(std::string_view kindName, std::string_view id) {
  uint64_t h = 14695981039346656037ULL;
  const auto mix = [&](std::string_view s) {
    for (char c : s) {
      h ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
      h *= 1099511628211ULL;
    }
  };
  mix(kindName);
  mix(":");
  mix(id);
  return h;
}

}  // namespace litholattice
