#pragma once
#include "litholattice/world/chunk_map.h"

namespace litholattice {
// Voxel DDA (Amanatides & Woo). Returns the first non-air cell and the cell in front of it.
/// With `blocks`, kFlatPick cells are hit only through their flat plate (look past a wire).
bool raycast(const ChunkMap& w, float ox, float oy, float oz, float dx, float dy, float dz,
             float maxDist, int hit[3], int prev[3], const BlockRegistry* blocks = nullptr);
/// The pick box of a kFlatPick cell: [x, x + 1] x [y0, y1] x [z, z + 1].
void flatPickRange(const ChunkMap& w, const BlockRegistry& blocks, int x, int y, int z, float& y0, float& y1);
}  // namespace litholattice
