// Self-test for the game-side ProcGen vegetation API (workstream 3, 2026-10-11).
//
// Verifies, per tree family (clockwood wirewood, dopant crystal-tree,
// trace-forest circuit-tree):
//   1. save -> load round-trips bit-identical (modelEquals on the leaf stream)
//   2. leaf counts sane, bbox inside the 64^3 volume, maxLevel in range
//   3. determinism: re-author => modelEquals, re-save => byte-identical file,
//      matching the checked-in assets/models/*.lthm byte for byte
//   4. palette binding: every leaf uses a known palette id, and each family's
//      expected game-palette materials actually appear
// Plus: the determinism contract (same seed -> same hashLeaves, different
// seed -> different plant), bind*Palette pass-through for hand-rolled params,
// the shrub 3D path (round-trip + determinism), the shrub 2D path
// (ShrubCrossInfo bundle, texture set sanity, voxel stand-in), and the
// 11-biome mapping table (coverage, sane densities, blendedDensity smoke).
//
// Exit code = number of failed checks. ctest entry: vegetation_selftest.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <litholattice/procgen/pipeline.h>
#include <litholattice/procgen/stages.h>
#include <litholattice/subdiv/octree.h>

#include "lithokraft/game/wafer_biomes.h"

#include "biome_vegetation.h"
#include "models.h"
#include "vegetation.h"
#include "vegetation_models.h"

namespace fs = std::filesystem;
namespace procgen = litholattice::procgen;
namespace palette = lithokraft::model::authored::palette;
using lithokraft::model::LoadError;
using lithokraft::model::LoadResult;
using lithokraft::model::MicroCell;
using lithokraft::model::Model;
using lithokraft::model::loadModel;
using lithokraft::model::modelEquals;
using lithokraft::model::saveModel;
using lithokraft::vegetation::ShrubCrossInfo;
using lithokraft::vegetation::VegetationModel;
using lithokraft::vegetation::allVegetationModels;
using lithokraft::vegetation::bindShrubPalette;
using lithokraft::vegetation::bindTreePalette;
using lithokraft::vegetation::fillShrubDensity;
using lithokraft::vegetation::fillTreeDensity;
using lithokraft::vegetation::generateShrubModel;
using lithokraft::vegetation::generateTreeModel;
using lithokraft::vegetation::kBiomeVegetationCount;
using lithokraft::vegetation::kBiomeVegetationTable;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_failures;
}

const char* loadErrorName(LoadError e) {
  switch (e) {
    case LoadError::Ok:
      return "Ok";
    case LoadError::IoError:
      return "IoError";
    case LoadError::BadMagic:
      return "BadMagic";
    case LoadError::UnsupportedVersion:
      return "UnsupportedVersion";
    case LoadError::Truncated:
      return "Truncated";
    case LoadError::BadData:
      return "BadData";
    case LoadError::BadCrc:
      return "BadCrc";
  }
  return "?";
}

std::vector<uint8_t> readBytes(const fs::path& p) {
  std::vector<uint8_t> out;
  FILE* f = std::fopen(p.string().c_str(), "rb");
  if (!f) return out;
  uint8_t buf[8192];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
    out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return out;
}

bool near(float a, float b) {
  const float d = a - b;
  return d > -1e-6f && d < 1e-6f;
}

// Sanity bounds: non-degenerate but sparse (a full 64^3 fill is 262144).
constexpr size_t kMinTreeLeaves = 50;
constexpr size_t kMaxTreeLeaves = 120000;

}  // namespace

int main() {
  const fs::path modelsDir(LITHOKRAFT_MODELS_SRC_DIR);
  const fs::path tmp = fs::temp_directory_path() / "lthm_vegetation_selftest";
  std::error_code ec;
  fs::create_directories(tmp, ec);
  if (ec) {
    std::printf("FAIL: cannot create temp dir: %s\n", ec.message().c_str());
    return 1;
  }

  // ---- 1-4: the three family models --------------------------------------
  for (const VegetationModel& vm : allVegetationModels()) {
    std::printf("model %-20s (%s):\n", vm.fileStem, vm.displayName);
    const Model m1 = vm.author();
    const size_t leaves = m1.geometry.leafCount();
    char buf[256];

    std::snprintf(buf, sizeof buf, "leaf count %zu within [%zu, %zu]", leaves,
                  kMinTreeLeaves, kMaxTreeLeaves);
    check(leaves >= kMinTreeLeaves && leaves <= kMaxTreeLeaves, buf);
    std::snprintf(buf, sizeof buf, "maxLevel %d in [1, 6]", m1.maxLevel);
    check(m1.maxLevel >= 1 && m1.maxLevel <= 6, buf);

    int minX = 64, minY = 64, minZ = 64, maxX = -1, maxY = -1, maxZ = -1;
    bool inRange = true, paletteOk = true;
    bool seen[14] = {false};
    for (const MicroCell& c : m1.geometry.leaves()) {
      const int span = 1 << c.level;
      if (c.x >= span || c.y >= span || c.z >= span) inRange = false;
      if (c.x < minX) minX = c.x;
      if (c.y < minY) minY = c.y;
      if (c.z < minZ) minZ = c.z;
      if (c.x > maxX) maxX = c.x;
      if (c.y > maxY) maxY = c.y;
      if (c.z > maxZ) maxZ = c.z;
      if (c.material >= 14 || std::strcmp(palette::name(c.material), "?") == 0)
        paletteOk = false;
      else
        seen[c.material] = true;
    }
    check(inRange, "all leaves inside the 64^3 volume");
    std::snprintf(buf, sizeof buf, "bbox x[%d,%d] y[%d,%d] z[%d,%d]",
                  minX, maxX, minY, maxY, minZ, maxZ);
    check(maxX - minX >= 2 && maxY - minY >= 8 && maxZ - minZ >= 2, buf);
    check(paletteOk, "all materials are known palette ids");

    // Family-specific palette binding (the engine placeholder slots must
    // have become real game palette ids).
    const std::string stem(vm.fileStem);
    if (stem == "clockwood-wirewood") {
      check(seen[palette::kCopper], "clockwood trunk is kCopper");
      bool onlyCopper = true;
      for (int i = 1; i < 14; ++i)
        if (seen[i] && i != palette::kCopper) onlyCopper = false;
      check(onlyCopper, "clockwood uses only kCopper (no leaves/glow)");
    } else if (stem == "dopant-crystal-tree") {
      check(seen[palette::kCrystalBlue] && seen[palette::kCrystalAmber],
            "dopant shows blue + amber crystal");
      check(seen[palette::kCrystalCore], "dopant has white-hot apex core");
    } else if (stem == "trace-forest-tree") {
      check(seen[palette::kCopper], "trace-forest trunk is kCopper");
      check(seen[palette::kTraceRed] && seen[palette::kTraceBlue],
            "trace-forest has red AND blue glowing tips");
    }

    // Round-trip through disk.
    const fs::path t1 = tmp / (stem + ".a.lthm");
    const fs::path t2 = tmp / (stem + ".b.lthm");
    std::string err;
    check(saveModel(t1.string(), m1, &err), "saveModel ok");
    const LoadResult lr = loadModel(t1.string());
    std::snprintf(buf, sizeof buf, "loadModel ok (err=%s)",
                  loadErrorName(lr.error));
    check(lr.error == LoadError::Ok, buf);
    if (lr.error == LoadError::Ok)
      check(modelEquals(m1, lr.model), "round-trip modelEquals");

    // Determinism: re-author => equal model, byte-identical file.
    const Model m2 = vm.author();
    check(modelEquals(m1, m2), "re-author modelEquals");
    check(saveModel(t2.string(), m2, &err), "saveModel (2nd) ok");
    check(readBytes(t1.string()) == readBytes(t2.string()),
          "re-author byte-identical file");

    // The checked-in file must match the authoring code exactly.
    const fs::path ref = modelsDir / (stem + ".lthm");
    const LoadResult rr = loadModel(ref.string());
    std::snprintf(buf, sizeof buf, "checked-in %s loads (err=%s)",
                  ref.string().c_str(), loadErrorName(rr.error));
    check(rr.error == LoadError::Ok, buf);
    if (rr.error == LoadError::Ok) {
      check(modelEquals(m1, rr.model), "checked-in modelEquals author");
      check(readBytes(ref.string()) == readBytes(t1.string()),
            "checked-in byte-identical to re-author");
    }
    std::printf("  leaves=%zu maxLevel=%d\n", leaves, m1.maxLevel);
  }

  // ---- determinism contract ------------------------------------------------
  std::printf("determinism contract:\n");
  {
    procgen::TreeParams p = procgen::clockwoodWirewood(1234);
    const Model a = generateTreeModel(p, "a", "");
    const Model b = generateTreeModel(p, "a", "");
    check(modelEquals(a, b), "same params+seed -> modelEquals");
    check(litholattice::subdiv::hashLeaves(a.geometry) ==
              litholattice::subdiv::hashLeaves(b.geometry),
          "same params+seed -> same hashLeaves");
    procgen::TreeParams p2 = procgen::clockwoodWirewood(1235);
    const Model c = generateTreeModel(p2, "a", "");
    check(!modelEquals(a, c), "different seed -> different plant");
    check(litholattice::subdiv::hashLeaves(a.geometry) !=
              litholattice::subdiv::hashLeaves(c.geometry),
          "different seed -> different hashLeaves");
  }

  // ---- bind*Palette pass-through for hand-rolled params --------------------
  std::printf("palette binding:\n");
  {
    procgen::TreeParams hand;
    hand.trunkMaterial = palette::kHeadMetal;
    hand.leafMaterial = palette::kShellDark;
    hand.tipMaterial = palette::kSeamRed;
    bindTreePalette(hand);
    check(hand.trunkMaterial == palette::kHeadMetal &&
              hand.leafMaterial == palette::kShellDark &&
              hand.tipMaterial == palette::kSeamRed,
          "hand-rolled tree params keep caller-supplied ids");
    procgen::ShrubParams shand;
    shand.stemMaterial = palette::kHeadMetal;
    shand.leafMaterial = palette::kShellPlate;
    bindShrubPalette(shand);
    check(shand.stemMaterial == palette::kHeadMetal &&
              shand.leafMaterial == palette::kShellPlate,
          "hand-rolled shrub params keep caller-supplied ids");
    // Placeholder patterns map to the game palette.
    procgen::TreeParams dp = procgen::dopantCrystalTree(7);
    bindTreePalette(dp);
    check(dp.trunkMaterial == palette::kCrystalBlue &&
              dp.leafMaterial == palette::kCrystalAmber &&
              dp.tipMaterial == palette::kCrystalCore,
          "dopant placeholders -> crystal palette ids");
  }

  // ---- shrub 3D path --------------------------------------------------------
  std::printf("shrub 3D path:\n");
  {
    const procgen::ShrubParams p = procgen::presetBerryShrub();
    const Model a = generateShrubModel(p, "Berry Shrub", "");
    const Model b = generateShrubModel(p, "Berry Shrub", "");
    check(!a.geometry.empty(), "berry shrub non-empty");
    check(a.geometry.leafCount() <= size_t(procgen::kMaxShrubLeaves) + 256,
          "berry shrub leaf count bounded");
    check(modelEquals(a, b), "berry shrub re-generate modelEquals");
    const procgen::ShrubParams p2 = procgen::presetTwistedShrub();
    const Model t = generateShrubModel(p2, "Twisted Shrub", "");
    check(!modelEquals(a, t), "twisted shrub differs from berry shrub");
    // Round-trip through disk.
    const fs::path t1 = tmp / "berry.a.lthm";
    std::string err;
    check(saveModel(t1.string(), a, &err), "berry saveModel ok");
    const LoadResult lr = loadModel(t1.string());
    check(lr.error == LoadError::Ok &&
              modelEquals(a, lr.model),
          "berry round-trip modelEquals");
    // Materials are the bound game palette ids.
    bool ok = true;
    for (const MicroCell& c : a.geometry.leaves())
      if (c.material != palette::kHandleWood && c.material != palette::kTraceRed)
        ok = false;
    check(ok, "berry shrub uses kHandleWood + kTraceRed only");
  }

  // ---- shrub 2D path ----------------------------------------------------------
  std::printf("shrub 2D path:\n");
  {
    ShrubCrossInfo info;
    const Model m =
        generateShrubModel(procgen::presetMeadowTuft(), "Meadow Tuft", "", &info);
    check(info.textureIndex < procgen::numPlantTextures(),
          "textureIndex in range");
    check(info.textureName != nullptr &&
              std::strcmp(info.textureName,
                           procgen::plantTextureName(info.textureIndex)) == 0,
          "textureName matches plantTextureName(index)");
    char buf[128];
    std::snprintf(buf, sizeof buf, "footprint %.2f x %.2f blocks",
                  info.widthBlocks, info.heightBlocks);
    check(info.widthBlocks > 0.f && info.heightBlocks > 0.f, buf);
    check(!m.geometry.empty(), "cross stand-in non-empty");
    check(m.geometry.leafCount() < 20000, "cross stand-in small");
    bool ok = true;
    for (const MicroCell& c : m.geometry.leaves())
      if (c.material != palette::kHandleWood && c.material != palette::kShellDark)
        ok = false;
    check(ok, "cross stand-in uses kHandleWood + kShellDark only");
    // Null crossOut is fine.
    const Model m2 = generateShrubModel(procgen::presetDryTuft(), "Dry", "");
    check(!m2.geometry.empty(), "dry tuft stand-in non-empty (null crossOut)");
    // Dry tuft differs from meadow tuft in the 2D bundle.
    ShrubCrossInfo dry;
    generateShrubModel(procgen::presetDryTuft(), "Dry", "", &dry);
    check(dry.textureIndex != info.textureIndex ||
              dry.tintR != info.tintR || dry.tintG != info.tintG ||
              dry.tintB != info.tintB || dry.widthBlocks != info.widthBlocks,
          "dry tuft 2D bundle differs from meadow tuft");
    // Fail-closed: invalid params -> zeroed plant -> empty stand-in.
    procgen::ShrubParams bad = procgen::presetMeadowTuft();
    bad.sizeCells = 0;
    ShrubCrossInfo badInfo;
    const Model badM = generateShrubModel(bad, "Bad", "", &badInfo);
    check(badM.geometry.empty() && badInfo.widthBlocks == 0.f,
          "invalid shrub params -> empty stand-in, zeroed bundle");
  }

  // ---- procedural texture set -------------------------------------------------
  std::printf("texture set:\n");
  {
    check(procgen::numPlantTextures() == 3, "numPlantTextures() == 3");
    for (uint32_t i = 0; i < procgen::numPlantTextures(); ++i) {
      const char* name = procgen::plantTextureName(i);
      const uint8_t* px = procgen::plantTexturePixels(i);
      char buf[128];
      std::snprintf(buf, sizeof buf, "texture %u name='%s' pixels!=null", i,
                    name ? name : "(null)");
      check(name != nullptr && name[0] != '\0' && px != nullptr, buf);
      if (px) {
        bool seenClear = false, seenOpaque = false;
        for (int k = 0; k < procgen::kPlantTextureBytes; k += 4) {
          if (px[k + 3] == 0) seenClear = true;
          if (px[k + 3] == 255) seenOpaque = true;
        }
        std::snprintf(buf, sizeof buf, "texture %u has cutout + opaque texels",
                      i);
        check(seenClear && seenOpaque, buf);
        check(std::memcmp(px, procgen::plantTexturePixels(i),
                          procgen::kPlantTextureBytes) == 0,
              "texture pixels deterministic across calls");
      }
    }
    check(procgen::plantTexturePixels(99) == nullptr,
          "out-of-range texture index -> nullptr");
    check(procgen::plantTextureName(99) == nullptr,
          "out-of-range texture name -> nullptr");
  }

  // ---- biome -> vegetation mapping table --------------------------------------
  std::printf("biome mapping:\n");
  {
    using lithokraft::vegetation::ShrubKind;
    using lithokraft::vegetation::ShrubStyle;
    using lithokraft::vegetation::TreePresence;
    check(kBiomeVegetationCount == 11, "table covers 11 biomes");
    const char* expected[11] = {
        "TraceForest",   "DicingPlains", "CapacitorGroves", "ResonatorSpires",
        "DeadZones",     "EtchedPlains", "ClockwoodForest", "DopantGroves",
        "OxideDunes",    "StaticMarsh",  "FabRuins"};
    int liveCount = 0, newCount = 0;
    bool namesOk = true, densitiesOk = true, seenLive[5] = {false};
    for (int i = 0; i < kBiomeVegetationCount; ++i) {
      const auto& row = kBiomeVegetationTable[i];
      if (std::strcmp(row.name, expected[i]) != 0) namesOk = false;
      if (!(row.treeDensity >= 0.f && row.treeDensity <= 1.f &&
            row.shrubDensity >= 0.f && row.shrubDensity <= 1.f))
        densitiesOk = false;
      if (row.inWaferJson) {
        ++liveCount;
        if (row.waferBiomeIndex < 0 || row.waferBiomeIndex > 4) namesOk = false;
        else
          seenLive[row.waferBiomeIndex] = true;
      } else {
        ++newCount;
        if (row.waferBiomeIndex != -1) namesOk = false;
      }
      if (row.tree == TreePresence::None && row.treeDensity != 0.f)
        densitiesOk = false;
      if (row.shrubStyle == ShrubStyle::None && row.shrubDensity != 0.f)
        densitiesOk = false;
    }
    check(namesOk, "biome names/indices in the expected order");
    check(densitiesOk, "densities in [0,1]; None entries have 0 density");
    check(liveCount == 5 && newCount == 6, "5 live + 6 JD-approved biomes");
    bool allLive = true;
    for (bool s : seenLive) allLive = allLive && s;
    check(allLive, "live indices cover 0..4 exactly once");
    // WaferBiome enum order matches wafer.json (and the table indices).
    check(kBiomeVegetationTable[0].waferBiomeIndex ==
                  int(lithokraft::WaferBiome::TraceForest) &&
              kBiomeVegetationTable[4].waferBiomeIndex ==
                  int(lithokraft::WaferBiome::DeadZones),
          "table indices match the WaferBiome enum");
    // DeadZones grows nothing.
    const auto& dead = kBiomeVegetationTable[4];
    check(dead.tree == TreePresence::None && dead.treeDensity == 0.f &&
              dead.shrubStyle == ShrubStyle::None && dead.shrubDensity == 0.f,
          "DeadZones: no trees, no shrubs");
    // Fab Ruins is in the Organic, not the Wafer.
    const auto& fab = kBiomeVegetationTable[10];
    check(std::strcmp(fab.dimension, "Organic") == 0 && !fab.inWaferJson,
          "Fab Ruins is Organic, not in wafer.json");
    // blendedDensity consumption smoke test: one-hot weight on
    // CapacitorGroves yields exactly its table density.
    procgen::BiomeDensity td, sd;
    fillTreeDensity(td);
    fillShrubDensity(sd);
    procgen::ColumnMaps maps;
    maps.biome = {2};
    maps.biomeCount = 5;
    maps.biomeWeight = {0.f, 0.f, 1.f, 0.f, 0.f};
    check(near(procgen::blendedDensity(td, maps, 0), 0.045f),
          "blendedDensity one-hot CapacitorGroves -> 0.045 tree");
    check(near(procgen::blendedDensity(sd, maps, 0), 0.060f),
          "blendedDensity one-hot CapacitorGroves -> 0.060 shrub");
    maps.biome = {4};
    maps.biomeWeight = {0.f, 0.f, 0.f, 0.f, 1.f};
    check(procgen::blendedDensity(td, maps, 0) == 0.f &&
              procgen::blendedDensity(sd, maps, 0) == 0.f,
          "blendedDensity one-hot DeadZones -> 0");
    // Formula rows (2026-10-12): shrubParamsForRow plumbs row super* fields
    // into ShrubParams; the cross path then binds the formula sprite whose
    // family entry the row mirrors.
    using lithokraft::vegetation::shrubParamsForRow;
    {
      const auto& marsh = kBiomeVegetationTable[9];  // StaticMarsh, flower
      const procgen::ShrubParams p = shrubParamsForRow(marsh, 777);
      check(p.seed == 777 && p.style == procgen::PlantStyle::CrossedQuads &&
                p.superM == 6.0 && p.superN1 == 2.0 && p.superN2 == 4.0 &&
                p.superN3 == 4.0 && p.superA == 1.0 && p.superB == 1.0,
            "row plumbing: StaticMarsh carries the flower formula set");
      const procgen::CrossQuadPlant c = procgen::generateShrubCross(p);
      check(c.textureIndex == procgen::kFirstFormulaTextureIndex + 0 &&
                std::strcmp(procgen::plantTextureNameAny(c.textureIndex),
                            "procgen_formula_flower") == 0,
            "row plumbing: StaticMarsh cross binds procgen_formula_flower");
      const auto& dicing = kBiomeVegetationTable[1];  // DicingPlains, leaf
      const procgen::CrossQuadPlant dc = procgen::generateShrubCross(
          shrubParamsForRow(dicing, 5));
      check(dc.textureIndex == procgen::kFirstFormulaTextureIndex + 2,
            "row plumbing: DicingPlains cross binds the leaf formula sprite");
      const auto& spires = kBiomeVegetationTable[3];  // ResonatorSpires, blob
      const procgen::CrossQuadPlant sc = procgen::generateShrubCross(
          shrubParamsForRow(spires, 5));
      check(sc.textureIndex == procgen::kFirstFormulaTextureIndex + 3,
            "row plumbing: ResonatorSpires cross binds the blob formula sprite");
      // A row without formula fields keeps its preset's own super* values
      // (TraceForest: berry preset's m=5 octree cross-section).
      const auto& trace = kBiomeVegetationTable[0];
      const procgen::ShrubParams tp = shrubParamsForRow(trace, 9);
      check(tp.superM == 5.0 && tp.style == procgen::PlantStyle::Modeled3D,
            "row plumbing: formula-free rows keep preset params and style");
      // End-to-end through the game API: texture name resolves for a
      // formula-bound cross bundle.
      ShrubCrossInfo info;
      generateShrubModel(shrubParamsForRow(marsh, 31337), "Marsh Flower", "",
                         &info);
      check(info.textureName != nullptr &&
                std::strcmp(info.textureName, "procgen_formula_flower") == 0,
            "row plumbing: ShrubCrossInfo names the formula texture");
    }
  }

  std::printf(g_failures ? "vegetation_selftest: %d FAILURE(S)\n"
                         : "vegetation_selftest: ALL GREEN\n",
              g_failures);
  return g_failures ? 1 : 0;
}
