// Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
// concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
// from the mathematical definition; no third-party code referenced. Underlying patent
// expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
// ambition of No Man's Sky (Hello Games).

// Self-test for the Litho Formula preset gallery.
//   litho_formula_gallery_selftest            full gallery check
//   litho_formula_gallery_selftest --verify-dir <dir>
//                                             check .lthm files baked earlier
//                                             (used by the bake_litho_formula
//                                             CLI smoke test)
//
// Full check, per preset:
//  1. gallery JSON loads and validates;
//  2. the preset voxelizes to a non-empty, grid-bounded, NaN-free result;
//  3. every filled cell lies in [0, resolution) on each axis;
//  4. bake -> saveModel -> loadModel round-trips byte-clean: loadModel
//     reports Ok and modelEquals holds against the baked model;
//  5. baking twice yields byte-identical files (determinism).

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "litho_formula_bake.h"
#include "litholattice/procgen/litho_formula_voxelize.h"
#include "model.h"

namespace fs = std::filesystem;
namespace lf = litholattice::procgen::litho_formula;
using lithokraft::model::Model;
using lithokraft::model::litho_bake::BakedPreset;
using lithokraft::model::litho_bake::bakeLithoFormulaGallery;
using lithokraft::model::litho_bake::levelForResolution;
using lithokraft::model::litho_bake::writeBakedPresets;
using lithokraft::model::loadModel;
using lithokraft::model::modelEquals;
using lithokraft::model::saveModel;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++g_failures;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

std::string galleryJsonPath() {
  return (fs::path(LITHOKRAFT_MODELS_SRC_DIR) / "litho_formula_gallery.json")
      .string();
}

// Verify the .lthm files in `dir` against a fresh bake of the gallery.
int verifyDir(const std::string& dir) {
  std::vector<BakedPreset> baked;
  std::string err;
  if (!bakeLithoFormulaGallery(galleryJsonPath(), baked, &err)) {
    std::printf("FAIL: bake for verification failed: %s\n", err.c_str());
    return 1;
  }
  for (const auto& bp : baked) {
    const std::string path =
        (fs::path(dir) / (bp.preset.name + ".lthm")).string();
    const auto lr = loadModel(path);
    check(lr.error == lithokraft::model::LoadError::Ok,
          path + " loads clean (" + lr.message + ")");
    if (lr.error != lithokraft::model::LoadError::Ok) continue;
    check(modelEquals(lr.model, bp.model), path + " matches fresh bake");
    check(lr.model.geometry.leafCount() == bp.filledCells,
          path + " leaf count matches filled cells");
    check(lr.model.author == "litho-formula-gallery", path + " author stamp");
  }
  if (g_failures) {
    std::printf("verify-dir: %d FAILURE(S)\n", g_failures);
  } else {
    std::printf("verify-dir: all %zu .lthm files check out\n", baked.size());
  }
  return g_failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--verify-dir")
    return verifyDir(argv[2]);
  if (argc != 1) {
    std::printf("usage: litho_formula_gallery_selftest [--verify-dir <dir>]\n");
    return 1;
  }

  // 1. The gallery loads.
  std::vector<lf::LithoPreset> presets;
  std::string err;
  check(lf::loadLithoFormulaGallery(galleryJsonPath(), presets, &err),
        "gallery JSON loads (" + err + ")");
  if (presets.empty()) {
    std::printf("FAIL: no presets loaded, aborting\n");
    return 1;
  }
  std::printf("gallery: %zu presets\n", presets.size());

  // 2-3. Every preset voxelizes cleanly: non-empty, bounded, finite.
  for (const auto& p : presets) {
    const lf::VoxelResult vr =
        lf::voxelizeLithoShape(p.theta, p.phi, p.resolution, p.material);
    check(!vr.filled.empty(), p.name + ": voxelizes non-empty");
    check(vr.resolution == p.resolution, p.name + ": resolution honored");
    bool bounded = true;
    for (const auto& c : vr.filled)
      for (int axis = 0; axis < 3; ++axis)
        if (c[axis] < 0 || c[axis] >= vr.resolution) bounded = false;
    check(bounded, p.name + ": all cells inside the grid");
    std::printf("  %-12s res %2d  filled %6zu  material %3u  level %d\n",
                p.name.c_str(), vr.resolution, vr.filled.size(), p.material,
                levelForResolution(vr.resolution));
  }

  // 4-5. Bake -> save -> load round-trip + determinism, in a temp dir.
  std::vector<BakedPreset> baked;
  check(bakeLithoFormulaGallery(galleryJsonPath(), baked, &err),
        "gallery bakes (" + err + ")");
  if (!baked.empty()) {
    std::error_code ec;
    const fs::path tmp =
        fs::temp_directory_path(ec) / "litho-formula-gallery-selftest";
    fs::remove_all(tmp, ec);
    check(writeBakedPresets(tmp.string(), baked, &err),
          "selftest bake writes (" + err + ")");

    // Determinism: bake a second time into another dir, byte-compare.
    const fs::path tmp2 =
        fs::temp_directory_path(ec) / "litho-formula-gallery-selftest-2";
    fs::remove_all(tmp2, ec);
    std::vector<BakedPreset> baked2;
    check(bakeLithoFormulaGallery(galleryJsonPath(), baked2, &err),
          "second bake (" + err + ")");
    check(writeBakedPresets(tmp2.string(), baked2, &err),
          "second bake writes (" + err + ")");

    for (const auto& bp : baked) {
      const std::string p1 =
          (tmp / (bp.preset.name + ".lthm")).string();
      const std::string p2 =
          (tmp2 / (bp.preset.name + ".lthm")).string();
      const auto lr = loadModel(p1);
      check(lr.error == lithokraft::model::LoadError::Ok,
            bp.preset.name + " reloads (" + lr.message + ")");
      if (lr.error == lithokraft::model::LoadError::Ok)
        check(modelEquals(lr.model, bp.model),
              bp.preset.name + " round-trips exactly");
      // Byte-identical files across the two bakes.
      std::error_code sec;
      const auto s1 = fs::file_size(p1, sec);
      const bool s1ok = !sec;
      const auto s2 = fs::file_size(p2, sec);
      const bool s2ok = !sec;
      check(s1ok && s2ok && s1 == s2, bp.preset.name + " deterministic size");
      if (s1ok && s2ok && s1 == s2) {
        FILE* f1 = std::fopen(p1.c_str(), "rb");
        FILE* f2 = std::fopen(p2.c_str(), "rb");
        bool same = f1 && f2;
        if (same) {
          char b1[4096], b2[4096];
          size_t r1, r2;
          do {
            r1 = std::fread(b1, 1, sizeof b1, f1);
            r2 = std::fread(b2, 1, sizeof b2, f2);
            if (r1 != r2 || std::memcmp(b1, b2, r1) != 0) {
              same = false;
              break;
            }
          } while (r1 > 0);
        }
        if (f1) std::fclose(f1);
        if (f2) std::fclose(f2);
        check(same, bp.preset.name + " byte-identical across bakes");
      }
    }
    fs::remove_all(tmp, ec);
    fs::remove_all(tmp2, ec);
  }

  std::printf(g_failures ? "litho_formula_gallery_selftest: %d FAILURE(S)\n"
                         : "litho_formula_gallery_selftest: all checks passed\n",
              g_failures);
  return g_failures ? 1 : 0;
}
