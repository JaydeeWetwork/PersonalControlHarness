// ModelResolver over the baked Litho Formula gallery: resolves a preset name
// ("starfish") to its stampable unit cells for the "lithoform_stamp" worldgen
// feature. Thread-safe; each model loads from its .lthm exactly once.
#pragma once
#include <optional>
#include <string_view>
#include "litholattice/procgen/generator_loader.h"

namespace lithokraft::model {

/// Loads one gallery model by preset name; nullopt when absent or unreadable.
/// Exposed for tests; prefer makeLithoStampModelResolver() in production.
std::optional<litholattice::procgen::LithoStampModel> loadLithoStampModel(std::string_view name);

/// Builds the resolver handed to procgen's loadGenerator for "lithoform_stamp".
litholattice::procgen::ModelResolver makeLithoStampModelResolver();

}  // namespace lithokraft::model
