// Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
// concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
// from the mathematical definition; no third-party code referenced. Underlying patent
// expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
// ambition of No Man's Sky (Hello Games).

// Preset gallery for the Litho Formula. The JSON data file is the source of
// truth — artists and JD add shapes by editing
// game/LithoKraft/assets/models/litho_formula_gallery.json, never code.
// This header is the code registry: it loads that file into memory.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "litholattice/procgen/litho_formula.h"

namespace litholattice::procgen::litho_formula {

/// One named Litho Formula preset: the two parameter sets (theta drives the
/// longitude factor, phi the latitude factor), the voxel grid resolution
/// (power of two, 8..64), the .lthm palette material id, and a one-line
/// description shown in gallery tooling.
struct LithoPreset {
  std::string name;
  LithoParams theta;
  LithoParams phi;
  int resolution = 32;
  uint8_t material = 1;
  std::string description;
};

/// Load the preset gallery from a JSON file (see litho_formula_gallery.json
/// for the schema). Returns false and sets *err when the file cannot be
/// read or parsed, or when any preset is invalid: empty/duplicate/illegal
/// name, missing theta/phi parameters, resolution outside [8, 64], a zero
/// material id, or an empty description. On success `out` holds every
/// preset in file order.
bool loadLithoFormulaGallery(const std::string& jsonPath,
                             std::vector<LithoPreset>& out,
                             std::string* err = nullptr);

}  // namespace litholattice::procgen::litho_formula
