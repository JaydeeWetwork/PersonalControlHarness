// LithoLattice spawning: engine interface for mob spawning.
// Runs at low frequency (1 Hz), samples random positions, uses ILighting.
// Separate from per-block ticking for performance.
#pragma once
#include <cstdint>
#include "litholattice/world/lighting.h"

namespace litholattice {

/// Mob spawning system. The engine owns the timing; the game defines what spawns.
class ISpawning {
 public:
  virtual ~ISpawning() = default;

  /// Called once per second (not per tick). Samples random positions in loaded
  /// chunks and spawns mobs where ILighting::canSpawnMob returns true.
  /// @param lighting the lighting system to query
  /// @param tick the current tick (for deterministic RNG)
  virtual void onSpawnTick(const ILighting& lighting, uint64_t tick) = 0;

  /// How many random samples per chunk per spawn tick (default 8).
  virtual uint32_t samplesPerChunk() const { return 8; }
};

}  // namespace litholattice
