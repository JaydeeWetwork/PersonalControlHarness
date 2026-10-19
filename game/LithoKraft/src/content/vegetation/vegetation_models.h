// Vegetation family models: the three procgen tree families as named,
// showcase-seeded .lthm assets (workstream 3, 2026-10-11).
//
// Mirrors the author_models pattern (game/LithoKraft/src/content/model/):
// author_*() functions build Models through the vegetation API, and
// author_vegetation (re)generates assets/models/*.lthm from them.
// Deterministic: the same showcase seed always yields the same plant, so
// re-running the tool produces byte-identical files (verified by
// vegetation_selftest against the checked-in assets).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "model.h"
#include "vegetation.h"

namespace lithokraft::vegetation {

// Fixed showcase seeds -- one per family. Changing a seed changes the
// checked-in .lthm, so seeds change only with JD's sign-off.
inline constexpr uint64_t kClockwoodShowcaseSeed = 0xC10C700Dull;
inline constexpr uint64_t kDopantShowcaseSeed = 0xD0A67C47ull;
inline constexpr uint64_t kTraceForestShowcaseSeed = 0x7ACEF0E57ull;

inline constexpr const char* kVegetationAuthor = "LithoKraft workstream 3";

/// Clockwood wirewood: tall thin copper-brown wirewood (Wafer: Clockwood
/// Forest, Capacitor Groves).
model::Model authorClockwoodWirewood();
/// Dopant crystal-tree: faceted blue/amber crystal with a white-hot apex
/// (Wafer: Dopant Groves).
model::Model authorDopantCrystalTree();
/// TraceForest circuit-tree: copper trunk, red/blue glowing tips -- the
/// procgen sibling of the hand-authored trace_tree.lthm (Wafer: TraceForest).
model::Model authorTraceForestTree();

struct VegetationModel {
  const char* fileStem;  // e.g. "clockwood-wirewood" -> assets/models/*.lthm
  const char* displayName;
  model::Model (*author)();
};

// All three family models, in a fixed order.
const std::vector<VegetationModel>& allVegetationModels();

}  // namespace lithokraft::vegetation
