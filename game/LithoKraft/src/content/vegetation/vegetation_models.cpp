// Vegetation family models: implementation (workstream 3, 2026-10-11).
//
// Each family: engine preset at the fixed showcase seed -> game palette
// binding -> Model. The author_*() functions are what author_vegetation
// serializes to assets/models/*.lthm.

#include "vegetation_models.h"

namespace lithokraft::vegetation {

model::Model authorClockwoodWirewood() {
  return generateTreeModel(procgen::clockwoodWirewood(kClockwoodShowcaseSeed),
                           "Clockwood Wirewood", kVegetationAuthor);
}

model::Model authorDopantCrystalTree() {
  return generateTreeModel(procgen::dopantCrystalTree(kDopantShowcaseSeed),
                           "Dopant Crystal-Tree", kVegetationAuthor);
}

model::Model authorTraceForestTree() {
  return generateTreeModel(procgen::traceForestTree(kTraceForestShowcaseSeed),
                           "TraceForest Circuit-Tree", kVegetationAuthor);
}

const std::vector<VegetationModel>& allVegetationModels() {
  static const std::vector<VegetationModel> models = {
      {"clockwood-wirewood", "Clockwood Wirewood", authorClockwoodWirewood},
      {"dopant-crystal-tree", "Dopant Crystal-Tree", authorDopantCrystalTree},
      {"trace-forest-tree", "TraceForest Circuit-Tree", authorTraceForestTree},
  };
  return models;
}

}  // namespace lithokraft::vegetation
