// Game-side ProcGen vegetation API: implementation (workstream 3, 2026-10-11).
#include "vegetation.h"

#include <cmath>

#include <litholattice/subdiv/octree.h>

#include "models.h"  // game palette ids

namespace lithokraft::vegetation {

namespace procgen = litholattice::procgen;
namespace palette = lithokraft::model::authored::palette;

namespace {

// Engine placeholder material slots. These are the literal values the engine
// family presets write into the material fields; the game binding translates
// them (see bindTreePalette / bindShrubPalette). They mirror the "Caller
// remap" comments in engine/src/procgen/trees.cpp and shrubs.cpp -- if the
// engine ever changes a preset's slots, the self-test's palette checks fail
// loudly (that is the point: the binding must be re-verified, not silently
// drift).
inline constexpr uint8_t kClockwoodTrunkSlot = 3;

inline constexpr uint8_t kDopantTrunkSlot = 4;
inline constexpr uint8_t kDopantLeafSlot = 5;
inline constexpr uint8_t kDopantTipSlot = 6;

inline constexpr uint8_t kTraceTrunkSlot = 7;
inline constexpr uint8_t kTraceTipSlot = 8;
inline constexpr uint8_t kTraceLeafSlot = 9;

inline constexpr uint8_t kShrubStemSlot = 3;
inline constexpr uint8_t kBerryLeafSlot = 4;
inline constexpr uint8_t kTwistedLeafSlot = 5;
inline constexpr uint8_t kTuftStemSlot = 1;
inline constexpr uint8_t kTuftLeafSlot = 2;

/// Highest subdivision level carrying any leaf (0 for an empty octree).
int maxLeafLevel(const litholattice::subdiv::SparseOctree& tree) {
  int level = 0;
  for (const litholattice::subdiv::MicroCell& c : tree.leaves())
    if (c.level > level) level = c.level;
  return level;
}

/// Voxel stand-in for a crossed-quad shrub: two 1-cell-thick vertical planes
/// crossing at the volume centre (an X from above), leaf material, with a
/// 2x2 stem-material core. Sizing follows the CrossQuadPlant footprint
/// (blocks -> level-6 cells, 1 block = 64 cells). Deterministic: pure
/// function of (params, plant). A zero footprint yields an empty octree
/// (mirrors the engine's fail-closed contract for invalid params).
litholattice::subdiv::SparseOctree buildCrossStandIn(
    const procgen::ShrubParams& p, const procgen::CrossQuadPlant& plant) {
  litholattice::subdiv::SparseOctree tree;
  if (!(plant.widthBlocks > 0.f) || !(plant.heightBlocks > 0.f)) return tree;
  const int w = std::max(
      2, std::min(64, static_cast<int>(std::lround(plant.widthBlocks * 64.f))));
  const int h = std::max(
      2, std::min(64, static_cast<int>(std::lround(plant.heightBlocks * 64.f))));
  const int x0 = 32 - w / 2;
  auto put = [&](int x, int y, int z, uint8_t m) {
    if (m != 0 && x >= 0 && y >= 0 && z >= 0 && x < 64 && y < 64 && z < 64)
      tree.set(6, x, y, z, m);
  };
  for (int i = 0; i < w; ++i) {
    for (int y = 0; y < h; ++y) {
      put(x0 + i, y, 32, p.leafMaterial);  // plane 1: spans x at z = 32
      put(32, y, x0 + i, p.leafMaterial);  // plane 2: spans z at x = 32
    }
  }
  for (int y = 0; y < h / 3 + 1; ++y) {  // stem core
    put(31, y, 31, p.stemMaterial);
    put(32, y, 31, p.stemMaterial);
    put(31, y, 32, p.stemMaterial);
    put(32, y, 32, p.stemMaterial);
  }
  return tree;
}

}  // namespace

void bindTreePalette(procgen::TreeParams& p) {
  if (p.trunkMaterial == kClockwoodTrunkSlot && p.leafMaterial == 0 &&
      p.tipMaterial == 0) {
    // clockwoodWirewood: tall thin copper-brown wirewood, no leaves, no glow.
    p.trunkMaterial = palette::kCopper;
  } else if (p.trunkMaterial == kDopantTrunkSlot &&
             p.leafMaterial == kDopantLeafSlot &&
             p.tipMaterial == kDopantTipSlot) {
    // dopantCrystalTree: faceted crystal; blue/amber shard layers, white-hot apex.
    p.trunkMaterial = palette::kCrystalBlue;
    p.leafMaterial = palette::kCrystalAmber;
    p.tipMaterial = palette::kCrystalCore;
  } else if (p.trunkMaterial == kTraceTrunkSlot &&
             p.leafMaterial == kTraceLeafSlot &&
             p.tipMaterial == kTraceTipSlot) {
    // traceForestTree: copper trunk, two-colour red/blue glowing tips.
    p.trunkMaterial = palette::kCopper;
    p.leafMaterial = palette::kTraceBlue;
    p.tipMaterial = palette::kTraceRed;
  }
  // Unrecognized pattern: caller-supplied ids pass through untouched.
}

void bindShrubPalette(procgen::ShrubParams& p) {
  if (p.stemMaterial == kShrubStemSlot && p.leafMaterial == kBerryLeafSlot) {
    p.stemMaterial = palette::kHandleWood;
    p.leafMaterial = palette::kTraceRed;  // red "berry" foliage mass
  } else if (p.stemMaterial == kShrubStemSlot &&
             p.leafMaterial == kTwistedLeafSlot) {
    p.stemMaterial = palette::kCopper;
    p.leafMaterial = palette::kShellDark;
  } else if (p.stemMaterial == kTuftStemSlot &&
             p.leafMaterial == kTuftLeafSlot) {
    // Meadow/dry tufts share one placeholder pattern; their 2D identity
    // (texture + tint) comes from the seed via generateShrubCross.
    p.stemMaterial = palette::kHandleWood;
    p.leafMaterial = palette::kShellDark;
  }
  // Unrecognized pattern: caller-supplied ids pass through untouched.
}

model::Model generateTreeModel(const procgen::TreeParams& p, std::string name,
                               std::string author) {
  procgen::TreeParams q = p;
  bindTreePalette(q);
  litholattice::subdiv::SparseOctree oct = procgen::generateTree(q);
  model::Model m;
  m.name = std::move(name);
  m.author = std::move(author);
  m.maxLevel = maxLeafLevel(oct);
  m.geometry = std::move(oct);
  return m;
}

model::Model generateShrubModel(const procgen::ShrubParams& p, std::string name,
                                std::string author, ShrubCrossInfo* crossOut) {
  procgen::ShrubParams q = p;
  bindShrubPalette(q);
  model::Model m;
  m.name = std::move(name);
  m.author = std::move(author);
  if (q.style == procgen::PlantStyle::CrossedQuads) {
    const procgen::CrossQuadPlant c = procgen::generateShrubCross(q);
    if (crossOut != nullptr) {
      crossOut->widthBlocks = c.widthBlocks;
      crossOut->heightBlocks = c.heightBlocks;
      crossOut->textureIndex = c.textureIndex;
      crossOut->textureName = procgen::plantTextureNameAny(c.textureIndex);
      crossOut->tintR = c.tintR;
      crossOut->tintG = c.tintG;
      crossOut->tintB = c.tintB;
    }
    m.geometry = buildCrossStandIn(q, c);
  } else {
    m.geometry = procgen::generateShrubOctree(q);
  }
  m.maxLevel = maxLeafLevel(m.geometry);
  return m;
}

}  // namespace lithokraft::vegetation
