// Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
// concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
// from the mathematical definition; no third-party code referenced. Underlying patent
// expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
// ambition of No Man's Sky (Hello Games).

#include "litho_formula_bake.h"

#include <filesystem>

#include "litholattice/procgen/litho_formula_voxelize.h"

namespace fs = std::filesystem;
namespace lf = litholattice::procgen::litho_formula;

namespace lithokraft::model::litho_bake {

bool bakeLithoFormulaGallery(const std::string& jsonPath,
                             std::vector<BakedPreset>& out,
                             std::string* err) {
  std::vector<lf::LithoPreset> presets;
  std::string errLocal;
  if (!lf::loadLithoFormulaGallery(jsonPath, presets, &errLocal)) {
    if (err) *err = errLocal;
    return false;
  }
  std::vector<BakedPreset> baked;
  for (const auto& p : presets) {
    const lf::VoxelResult vr =
        lf::voxelizeLithoShape(p.theta, p.phi, p.resolution, p.material);
    if (vr.filled.empty()) {
      if (err) *err = "preset \"" + p.name + "\" voxelized to nothing";
      return false;
    }
    ModelBuilder b(p.name, kBakeAuthor);
    const int level = levelForResolution(vr.resolution);
    for (const auto& cell : vr.filled)
      b.setCell(level, cell[0], cell[1], cell[2], p.material);
    BakedPreset bp;
    bp.preset = p;
    bp.model = b.build();
    bp.filledCells = vr.filled.size();
    if (bp.model.geometry.leafCount() == 0) {
      if (err) *err = "preset \"" + p.name + "\" built an empty model";
      return false;
    }
    baked.push_back(std::move(bp));
  }
  out = std::move(baked);
  return true;
}

bool writeBakedPresets(const std::string& outDir,
                       const std::vector<BakedPreset>& baked,
                       std::string* err) {
  std::error_code ec;
  fs::create_directories(outDir, ec);
  if (ec) {
    if (err) *err = "cannot create " + outDir + ": " + ec.message();
    return false;
  }
  for (const auto& bp : baked) {
    const fs::path p = fs::path(outDir) / (bp.preset.name + ".lthm");
    std::string saveErr;
    if (!saveModel(p.string(), bp.model, &saveErr)) {
      if (err) *err = p.string() + ": " + saveErr;
      return false;
    }
  }
  return true;
}

}  // namespace lithokraft::model::litho_bake
