// Step 1 (GPU Trace Optimization): CPU-side trace strength slot buffer.
// Each trace cell gets a persistent slot. Strengths (0-64) pack 4 per uint32.
// A dirty bitset tracks changed slots; uploadDirty() returns only the changed
// words for the GPU upload. The GL SSBO and shader changes are thin wrappers
// (untestable headless); this management layer is fully tested.
#pragma once
#include <cstdint>
#include <vector>
#include <bit>
#include <unordered_map>

namespace litholattice {

// Packed trace strengths: 4 bytes per uint32, dirty-tracked per word.
//
// KEY DISCIPLINE (P3-2 B-M4): cell keys are WORLD packPos keys
// (litholattice::packPos of the world cell coordinate) end to end — the
// signal sim writes with packPos keys, and the mesher must look up with
// the same world key (it previously used sub-chunk-LOCAL coordinates,
// so its lookups could only ever collide with unrelated cells' slots or
// miss). Slot 0 is a REAL slot (the first cell assigned); "no slot" is
// kNoSlot, never 0.
class TraceStrengthBuffer {
 public:
  /// Sentinel returned by slotOf() when a cell has no slot yet. Distinct
  /// from every real slot (slots are dense from 0).
  static constexpr uint32_t kNoSlot = UINT32_MAX;
  /// Assigns (or returns) the persistent slot for a trace cell key.
  uint32_t slotFor(uint64_t cellKey) {
    auto [it, fresh] = slots_.try_emplace(cellKey, uint32_t(strengths_.size()));
    if (fresh) strengths_.push_back(0);
    return it->second;
  }
  /// Sets the strength (0-64); marks the word dirty if changed.
  void set(uint32_t slot, uint8_t strength) {
    if (slot >= strengths_.size() || strengths_[slot] == strength) return;
    strengths_[slot] = strength;
    dirtyWords_.set(slot >> 2);
  }
  uint8_t get(uint32_t slot) const {
    return slot < strengths_.size() ? strengths_[slot] : 0;
  }
  /// Read-only slot lookup (for the mesher); kNoSlot if the cell has no
  /// slot yet (B-M4: 0 is a real slot — the first assigned cell's).
  uint32_t slotOf(uint64_t cellKey) const {
    const auto it = slots_.find(cellKey);
    return it == slots_.end() ? kNoSlot : it->second;
  }
  bool hasSlot(uint64_t cellKey) const { return slots_.find(cellKey) != slots_.end(); }
  /// Words changed since the last upload; call uploadAck() after uploading.
  std::vector<uint32_t> dirtyWordIndices() const { return dirtyWords_.indices(); }
  /// Packed word i (4 strengths, little-endian bytes).
  uint32_t packedWord(uint32_t i) const {
    uint32_t w = 0;
    for (int b = 0; b < 4; ++b) {
      const uint32_t s = i * 4 + b;
      if (s < strengths_.size()) w |= uint32_t(strengths_[s]) << (b * 8);
    }
    return w;
  }
  uint32_t wordCount() const { return uint32_t((strengths_.size() + 3) / 4); }
  void uploadAck() { dirtyWords_.clear(); }
  size_t slotCount() const { return strengths_.size(); }

 private:
  // Tiny bitset for dirty words.
  struct BitSet {
    void set(uint32_t i) {
      if (i >= bits_.size() * 32) bits_.resize((i + 32) / 32, 0);
      bits_[i >> 5] |= 1u << (i & 31);
    }
    void clear() { std::fill(bits_.begin(), bits_.end(), 0); }
    std::vector<uint32_t> indices() const {
      std::vector<uint32_t> out;
      for (size_t i = 0; i < bits_.size(); ++i) {
        uint32_t w = bits_[i];
        while (w) { const unsigned b = std::countr_zero(w); out.push_back(uint32_t(i * 32 + b)); w &= w - 1; }
      }
      return out;
    }
    std::vector<uint32_t> bits_;
  };
  std::unordered_map<uint64_t, uint32_t> slots_;
  std::vector<uint8_t> strengths_;
  BitSet dirtyWords_;
};

}  // namespace litholattice
