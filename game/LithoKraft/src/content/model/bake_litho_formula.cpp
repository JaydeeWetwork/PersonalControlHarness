// Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
// concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
// from the mathematical definition; no third-party code referenced. Underlying patent
// expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
// ambition of No Man's Sky (Hello Games).

// Baking tool: (re)generates one .lthm model file per Litho Formula gallery
// preset under game/LithoKraft/assets/models/ from the preset data in
// litho_formula_gallery.json.
//
// Usage: bake_litho_formula [outdir] [gallery.json]
//   outdir       defaults to the compiled-in LITHOKRAFT_MODELS_SRC_DIR
//                (the source tree's assets/models/).
//   gallery.json defaults to <LITHOKRAFT_MODELS_SRC_DIR>/litho_formula_gallery.json.
//
// The pipeline is deterministic: running this twice produces byte-identical
// files (verified by litho_formula_gallery_selftest).

#include <cstdio>
#include <filesystem>
#include <string>

#include "litho_formula_bake.h"

namespace fs = std::filesystem;
using lithokraft::model::litho_bake::BakedPreset;
using lithokraft::model::litho_bake::bakeLithoFormulaGallery;
using lithokraft::model::litho_bake::kBakeAuthor;
using lithokraft::model::litho_bake::writeBakedPresets;

int main(int argc, char** argv) {
  const fs::path modelsDir =
      (argc > 1) ? fs::path(argv[1]) : fs::path(LITHOKRAFT_MODELS_SRC_DIR);
  const fs::path galleryJson =
      (argc > 2) ? fs::path(argv[2])
                 : fs::path(LITHOKRAFT_MODELS_SRC_DIR) / "litho_formula_gallery.json";

  if (argc > 3 || (argc > 1 && std::string(argv[1]) == "--help")) {
    std::printf(
        "bake_litho_formula: bake the Litho Formula preset gallery to .lthm files\n"
        "usage: bake_litho_formula [outdir] [gallery.json]\n"
        "  outdir       destination for <preset>.lthm (default: %s)\n"
        "  gallery.json preset data (default: <outdir-default>/litho_formula_gallery.json)\n"
        "Each preset is voxelized and stamped into a Model authored by \"%s\".\n",
        LITHOKRAFT_MODELS_SRC_DIR, kBakeAuthor);
    return argc > 3 ? 1 : 0;
  }

  std::vector<BakedPreset> baked;
  std::string err;
  if (!bakeLithoFormulaGallery(galleryJson.string(), baked, &err)) {
    std::printf("bake_litho_formula: %s\n", err.c_str());
    return 1;
  }
  if (!writeBakedPresets(modelsDir.string(), baked, &err)) {
    std::printf("bake_litho_formula: %s\n", err.c_str());
    return 1;
  }

  for (const auto& bp : baked) {
    const fs::path p = modelsDir / (bp.preset.name + ".lthm");
    std::printf("wrote %-42s %6zu leaves  maxLevel %d  material %u\n",
                p.string().c_str(), bp.model.geometry.leafCount(),
                bp.model.maxLevel, bp.preset.material);
  }
  std::printf("bake_litho_formula: all %zu presets written\n", baked.size());
  return 0;
}
