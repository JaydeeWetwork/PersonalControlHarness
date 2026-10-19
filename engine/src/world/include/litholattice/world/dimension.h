// Multi-dimension framework. A Dimension owns its chunk map, terrain generator (a procgen
// pipeline, see column_generator.h), biome set and structure setup. The player is in exactly one
// dimension; the others stay unloaded. The game builds its registry (which dimensions exist, their
// generators and setup); the engine knows none of them.
#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "litholattice/core/threading.h"
#include "litholattice/world/chunk_map.h"
#include "litholattice/world/column_generator.h"

namespace litholattice {

/// Camera position and orientation for entering a dimension.
struct Spawn { float x, y, z, yaw, pitch; };

struct Dimension {
  using SetupFn = Spawn (*)(ChunkMap& w, int sizeChunks);  // structures + spawn point

  std::string id;      // registry key chosen by the game
  std::string name;    // display name
  std::vector<std::string> biomes;
  std::shared_ptr<const ColumnGenerator> generator;  // shared with worldgen jobs
  SetupFn setup = nullptr;
  int randomTicksPerSection = 8;  ///< random ticks per 16^3 section per world tick (0 = off)
  BlockId belowWorldFill = kAir;  ///< what meshing sees below y = 0 (a solid block hides the bottom faces)

  ChunkMap chunks;   // owned by main while loading, by the sim thread while active
  bool loaded = false;
  int lastLoadFailures = 0;  ///< columns whose generation threw during the last load()

  Spawn load(JobPool& pool, int sizeChunks);  // parallel worldgen + structures
  void unload() {
    // Preserve the registry across the map reset (Infinite Registry): get()/set()
    // translate through it, and the replacement map needs it too.
    const BlockRegistry* reg = chunks.registry();
    chunks = ChunkMap{};
    chunks.setRegistry(reg);
    loaded = false;
  }
};

class DimensionRegistry {
 public:
  Dimension& add(std::unique_ptr<Dimension> d) { dims_.push_back(std::move(d)); return *dims_.back(); }
  /// `from` is accepted by find() as another name for dimension `to` (old names keep working).
  void addAlias(std::string from, std::string to) { aliases_.emplace_back(std::move(from), std::move(to)); }
  Dimension* find(const std::string& idIn) {
    std::string_view id = idIn;
    for (const auto& [from, to] : aliases_) if (from == id) { id = to; break; }
    for (auto& d : dims_) if (d->id == id) return d.get();
    return nullptr;
  }
  const std::vector<std::unique_ptr<Dimension>>& all() const { return dims_; }

 private:
  std::vector<std::unique_ptr<Dimension>> dims_;
  std::vector<std::pair<std::string, std::string>> aliases_;
};

}  // namespace litholattice
