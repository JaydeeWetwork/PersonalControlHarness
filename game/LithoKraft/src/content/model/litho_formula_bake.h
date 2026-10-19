// Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
// concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
// from the mathematical definition; no third-party code referenced. Underlying patent
// expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
// ambition of No Man's Sky (Hello Games).

// Baking path for the Litho Formula preset gallery: voxelize each preset and
// build a .lthm Model through ModelBuilder. Shared by the bake_litho_formula
// CLI and the gallery self-test.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "model.h"
#include "litholattice/procgen/litho_formula_gallery.h"

namespace lithokraft::model::litho_bake {

/// One baked preset: the gallery preset plus its built model and the
/// voxelizer's filled-cell count (used by the self-test's assertions).
struct BakedPreset {
  litholattice::procgen::litho_formula::LithoPreset preset;
  Model model;
  size_t filledCells = 0;
};

/// Author string stamped on every baked model.
inline constexpr const char* kBakeAuthor = "litho-formula-gallery";

/// Load the gallery at `jsonPath`, voxelize every preset, and build one
/// Model per preset (cells stamped at level log2(resolution) with the
/// preset's material). Deterministic: the same gallery file always yields
/// the same models. Returns false + *err on any failure.
bool bakeLithoFormulaGallery(const std::string& jsonPath,
                             std::vector<BakedPreset>& out,
                             std::string* err = nullptr);

/// Write every baked preset's model to `<outDir>/<name>.lthm`.
/// Returns false + *err on the first I/O failure.
bool writeBakedPresets(const std::string& outDir,
                       const std::vector<BakedPreset>& baked,
                       std::string* err = nullptr);

/// Octree level for a gallery resolution (power of two in [8, 64]).
inline int levelForResolution(int resolution) {
  int level = 0;
  for (int p = resolution; p > 1; p >>= 1) ++level;
  return level;
}

}  // namespace lithokraft::model::litho_bake
