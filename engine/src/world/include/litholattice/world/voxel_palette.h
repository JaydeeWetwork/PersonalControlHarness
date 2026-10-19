// Width-tagged paletted voxel storage (Infinite Registry, phase 1).
//
// Indices are stored at 8/16/32/64 bits, chosen from the palette size; the palette
// maps index -> StableId. At 64-bit width the palette degenerates: the index IS the
// StableId (no palette entries). Random access is O(1) at every width ("pack the
// zeros": we pay for width only when we use it).
//
// Growth is one-way: the palette and the width only grow, never shrink. Removing the
// last voxel of a type leaves its palette entry in place (a compact() can come later).
// A fresh palette is all-air: width 8, single palette entry kAirStableId, indices 0.
#pragma once
#include <cassert>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "litholattice/world/block.h"

namespace litholattice {

class VoxelPalette {
 public:
  /// Width tag, stored in the subsection header. Values are stable (0..3).
  enum class Width : uint8_t { W8 = 0, W16 = 1, W32 = 2, W64 = 3 };

  explicit VoxelPalette(size_t voxelCount)
      : count_(voxelCount), data_(voxelCount, 0), palette_{kAirStableId} {
    assert(voxelCount > 0);
    indexOf_[kAirStableId] = 0;
  }

  size_t voxelCount() const { return count_; }
  Width width() const { return width_; }
  /// Bytes per stored index: 1/2/4/8.
  static constexpr size_t bytesForWidth(Width w) {
    return w == Width::W8 ? 1 : w == Width::W16 ? 2 : w == Width::W32 ? 4 : 8;
  }
  /// Palette entries (index -> StableId). Empty at 64-bit width (degenerate:
  /// the index IS the stable id).
  const std::vector<StableId>& palette() const { return palette_; }
  size_t paletteSize() const { return palette_.size(); }

  /// O(1) random read.
  StableId at(size_t i) const {
    assert(i < count_);
    if (width_ == Width::W64) return readU64(i);
    return palette_[readIndex(i)];
  }
  /// O(1) raw palette index (== the StableId itself at 64-bit width).
  /// Hot paths resolve the palette to BlockIds once per sub-chunk, then use this.
  uint64_t rawIndex(size_t i) const {
    assert(i < count_);
    return width_ == Width::W64 ? readU64(i) : readIndex(i);
  }

  /// O(1) amortized write. Grows the palette and steps the width up when the
  /// palette outgrows it (8 -> 16 -> 32 -> 64).
  void set(size_t i, StableId id) {
    assert(i < count_);
    // indexFor() may step the width all the way to W64 (palette degenerated);
    // re-check before writing.
    if (width_ != Width::W64) {
      const uint64_t idx = indexFor(id);
      if (width_ != Width::W64) {
        writeIndex(i, idx);
        return;
      }
    }
    writeU64(i, id);
  }

  /// Fill every voxel with `id`.
  void fill(StableId id) {
    if (width_ != Width::W64) {
      const uint64_t idx = indexFor(id);
      if (width_ != Width::W64) {
        const size_t bytes = bytesForWidth(width_);
        if (bytes == 1) {
          std::fill(data_.begin(), data_.end(), uint8_t(idx));
        } else {
          for (size_t i = 0; i < count_; ++i) writeIndex(i, idx);
        }
        return;
      }
    }
    for (size_t i = 0; i < count_; ++i) writeU64(i, id);
  }

  /// Reset to the fresh all-air state (width 8, one palette entry).
  void clear() {
    width_ = Width::W8;
    data_.assign(count_, 0);
    palette_.assign(1, kAirStableId);
    indexOf_.clear();
    indexOf_[kAirStableId] = 0;
  }

  /// Resolves the palette to BlockIds via `reg`: one hash lookup per palette entry,
  /// not per voxel. After this, `lut[rawIndex(i)]` is the voxel's BlockId.
  /// `lut` is empty at 64-bit width (degenerate: rawIndex() IS the StableId there,
  /// resolve each with reg.findByStableId()). Unresolvable entries map to kAir.
  ///
  /// Infinite Registry (phase 2): voxel storage holds blocks. A non-block stable
  /// id in a palette is a programming error upstream — fail fast here, once per
  /// palette entry, never per voxel. (In release builds the assert compiles out
  /// and the kind-filtered findByStableId below maps it to kAir instead: loud in
  /// dev, safe in prod.)
  void resolveBlockIds(const BlockRegistry& reg, std::vector<BlockId>& lut) const {
    lut.clear();
    if (width_ == Width::W64) return;
    lut.reserve(palette_.size());
    for (StableId sid : palette_) {
      [[maybe_unused]] const auto kind = reg.kindOf(sid);
      assert(!kind || *kind == EntryKind::Block);
      const auto id = reg.findByStableId(sid);
      lut.push_back(id ? *id : kAir);
    }
  }

  bool operator==(const VoxelPalette& o) const {
    return width_ == o.width_ && count_ == o.count_ && palette_ == o.palette_ &&
           data_ == o.data_;
  }
  bool operator!=(const VoxelPalette& o) const { return !(*this == o); }

 private:
  /// Palette capacity at each width; exceeding it steps the width up.
  static constexpr uint64_t capacityForWidth(Width w) {
    return w == Width::W8 ? 256ULL : w == Width::W16 ? 65536ULL : 0x100000000ULL;
  }
  static constexpr Width nextWidth(Width w) {
    return w == Width::W8 ? Width::W16 : w == Width::W16 ? Width::W32 : Width::W64;
  }

  uint64_t readIndex(size_t i) const {
    const uint8_t* p = data_.data() + i * bytesForWidth(width_);
    uint64_t v = p[0];
    if (width_ != Width::W8) {
      v |= uint64_t(p[1]) << 8;
      if (width_ == Width::W32) v |= uint64_t(p[2]) << 16 | uint64_t(p[3]) << 24;
    }
    return v;
  }
  void writeIndex(size_t i, uint64_t idx) {
    if (width_ == Width::W64) {  // defensive: index IS the id at W64
      writeU64(i, idx);
      return;
    }
    uint8_t* p = data_.data() + i * bytesForWidth(width_);
    p[0] = uint8_t(idx);
    if (width_ != Width::W8) {
      p[1] = uint8_t(idx >> 8);
      if (width_ == Width::W32) {
        p[2] = uint8_t(idx >> 16);
        p[3] = uint8_t(idx >> 24);
      }
    }
  }
  uint64_t readU64(size_t i) const {
    const uint8_t* p = data_.data() + i * 8;
    uint64_t v = 0;
    for (int b = 0; b < 8; ++b) v |= uint64_t(p[b]) << (8 * b);
    return v;
  }
  void writeU64(size_t i, uint64_t v) {
    uint8_t* p = data_.data() + i * 8;
    for (int b = 0; b < 8; ++b) p[b] = uint8_t(v >> (8 * b));
  }

  /// Palette index for `id`, adding it (and stepping the width) if new.
  /// May step the width all the way to W64, in which case the caller must
  /// re-check width() before using the returned index (it is meaningless:
  /// at W64 the id is written directly).
  uint32_t indexFor(StableId id) {
    const auto it = indexOf_.find(id);
    if (it != indexOf_.end()) return it->second;
    uint64_t idx = palette_.size();
    if (idx >= capacityForWidth(width_)) {
      stepWidth();
      if (width_ == Width::W64) return 0;
      idx = palette_.size();
    }
    palette_.push_back(id);
    indexOf_[id] = uint32_t(idx);
    return uint32_t(idx);
  }

  /// Widen the index storage, preserving every index value. At the W32 -> W64
  /// step the palette degenerates: indices become the StableIds themselves.
  void stepWidth() {
    const Width nw = nextWidth(width_);
    if (nw == Width::W64) {
      std::vector<uint8_t> nd(count_ * 8);
      for (size_t i = 0; i < count_; ++i) {
        const uint64_t v = palette_[readIndex(i)];
        for (int b = 0; b < 8; ++b) nd[i * 8 + b] = uint8_t(v >> (8 * b));
      }
      data_ = std::move(nd);
      palette_.clear();
      indexOf_.clear();
      width_ = Width::W64;
      return;
    }
    std::vector<uint8_t> nd(count_ * bytesForWidth(nw));
    const size_t ob = bytesForWidth(width_), nb = bytesForWidth(nw);
    for (size_t i = 0; i < count_; ++i) {
      uint64_t v = 0;
      for (size_t b = 0; b < ob; ++b) v |= uint64_t(data_[i * ob + b]) << (8 * b);
      for (size_t b = 0; b < nb; ++b) nd[i * nb + b] = uint8_t(v >> (8 * b));
    }
    data_ = std::move(nd);
    width_ = nw;
  }

  size_t count_ = 0;
  Width width_ = Width::W8;
  std::vector<uint8_t> data_;  // count_ indices at the current width, little-endian
  std::vector<StableId> palette_;
  std::unordered_map<StableId, uint32_t> indexOf_;
};

}  // namespace litholattice
