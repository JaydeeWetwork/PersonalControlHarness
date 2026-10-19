// The structure block: JD's in-game design tool (Minecraft's structure block,
// renamed — nobody owns the words "structure" or "block").
//
// Modes (stored in the block's cell-state value bits by the game):
//   CORNER (0): two corners mark a capture region.
//   SAVE   (1): captures the region between the two CORNER blocks into sorted
//              blueprint cells and registers them under a name in the
//              StructureRegistry. The name travels with the action (UI/console),
//              not in block state.
//   LOAD   (2): places a named structure from the registry at the block's position.
//
// The engine side is pure capture/place helpers over reader/writer callbacks —
// procgen never touches the game world directly. The game wires right-click
// behavior ("structure_block") to these.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "litholattice/procgen/stages.h"  // BlueprintCell

namespace litholattice::procgen {

/// Structure block modes (cell-state value bits 0-6).
enum class StructureBlockMode : uint8_t {
  Corner = 0,  ///< region corner marker
  Save = 1,    ///< capture region -> register named structure
  Load = 2,    ///< place named structure here
};

/// Reads a material at a world position; kAirMaterial for empty/out of range.
using BlockReader = std::function<MaterialId(int32_t x, int32_t y, int32_t z)>;
/// Writes a material; returns false when out of range or refused.
using BlockWriter = std::function<bool(int32_t x, int32_t y, int32_t z, MaterialId m)>;

/// Capture the axis-aligned region [min, max] (inclusive) into blueprint cells
/// relative to `min`. Deterministic: cells are sorted by (dy, dz, dx), air is
/// skipped, and structure blocks themselves are skipped (no recursive capture).
/// `structureBlockMaterial` is the material id of the structure block to exclude.
std::vector<BlueprintCell> captureRegion(BlockReader reader, int32_t minX, int32_t minY,
                                        int32_t minZ, int32_t maxX, int32_t maxY,
                                        int32_t maxZ, MaterialId structureBlockMaterial);

/// Place blueprint cells at (bx, by, bz) with rotation rot (0..3, like
/// StructureFeature). Returns the number of cells written. Writes through
/// `writer` (the game decides overwrite vs. air-only policy).
size_t placeCells(BlockWriter writer, const std::vector<BlueprintCell>& cells, int32_t bx,
                  int32_t by, int32_t bz, int rot);

/// SAVE: capture + register under `name`. Returns false when the registry is
/// sealed, the name is taken, or the region is empty.
bool saveStructureAs(BlockReader reader, std::string_view name, int32_t minX, int32_t minY,
                     int32_t minZ, int32_t maxX, int32_t maxY, int32_t maxZ,
                     MaterialId structureBlockMaterial);

/// LOAD: place the named structure's first variant at (bx, by, bz).
/// Returns 0 when the name is unknown (nothing placed).
size_t loadStructureAt(std::string_view name, BlockWriter writer, int32_t bx, int32_t by,
                       int32_t bz, int rot);

}  // namespace litholattice::procgen
