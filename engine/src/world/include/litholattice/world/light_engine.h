// BFS light propagation engine.
// STAGED / UNWIRED (P3-2 B-L5): LightEngine has NO production callers —
// nothing in the engine or the game constructs one today. The live
// lighting path is the flood-fill ambient in the renderer (see
// lighting.h). This class is kept, tested in isolation, as the staged
// replacement; do not treat it as the world's lighting until a wiring
// workstream connects it (and re-verifies levels end to end).
// Clean-room implementation inspired by Starlight's approach:
// breadth-first search for light spread, deferred updates.
//
// Key ideas (well-known algorithms, our own code):
// - BFS queue instead of recursive flood-fill (better cache behavior)
// - 4-bit nibble storage (0-15 light levels, packed)
// - Deferred: batch light updates, process once per tick
#pragma once
#include "litholattice/world/lighting.h"
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <queue>

namespace litholattice {

// kMaxLight lives in lighting.h (single definition; uint8_t promotes to int
// where needed). Do not redefine here — duplicate definitions in the same
// namespace are a hard compile error when both headers are included.

struct LightPos {
  int x, y, z;
  uint8_t level;
};

/// BFS light propagator for a chunk column.
class LightEngine {
 public:
  LightEngine(int sizeX, int sizeY, int sizeZ);

  /// Set a light source (e.g. torch, lamp, sky). `level` is clamped to
  /// kMaxLight (64): the 1..64 scale is the world's light currency, and an
  /// out-of-range emission would otherwise flood phantom light (B-L5).
  void addSource(int x, int y, int z, uint8_t level);

  /// Remove a light source (triggers re-propagation).
  void removeSource(int x, int y, int z);

  /// Set block opacity (0 = transparent, 64 = fully opaque).
  void setOpacity(int x, int y, int z, uint8_t opacity);

  /// Propagate all pending light updates via BFS.
  /// Returns the number of cells updated (for profiling).
  size_t propagate();

  /// Get light level at a position.
  uint8_t getLight(int x, int y, int z) const;

  /// Clear all light (e.g. dimension change).
  void clear();

 private:
  int sx_, sy_, sz_;
  std::vector<uint8_t> light_;    // light levels
  std::vector<uint8_t> opacity_;  // block opacity
  std::queue<LightPos> bfsQueue_;
  // Live sources: packed position -> emission level. removeSource() needs the
  // survivor set for its brighten pass (re-propagate surviving sources).
  std::unordered_map<int64_t, uint8_t> sources_;

  inline int idx(int x, int y, int z) const { return (y * sz_ + z) * sx_ + x; }
  inline bool inBounds(int x, int y, int z) const {
    return x >= 0 && x < sx_ && y >= 0 && y < sy_ && z >= 0 && z < sz_;
  }
};

}  // namespace litholattice
