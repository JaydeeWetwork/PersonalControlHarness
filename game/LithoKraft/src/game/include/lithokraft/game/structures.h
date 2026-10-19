// Hand-built structures placed after terrain generation (pure functions of the chunk map).
#pragma once
#include "lithokraft/game/engine.h"
#include "litholattice/world/chunk_map.h"
#include "litholattice/world/dimension.h"

namespace lithokraft {

/// Pre-placed demo circuits on the wafer (see README), on a silicon platform raised to the
/// highest terrain under it. Returns a camera spawn looking over the rows.
Spawn buildDemoCircuit(ChunkMap& w, int ox, int oz);

}  // namespace lithokraft
