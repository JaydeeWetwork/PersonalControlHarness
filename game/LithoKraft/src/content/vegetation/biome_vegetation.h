// Biome -> vegetation mapping for the Wafer (workstream 3, 2026-10-11).
//
// WHAT: a documented, code-readable table saying which procgen vegetation
//   family grows in each biome, in which render style, and at what density.
//   Covers ALL 11 biomes: the 5 live in assets/worldgen/wafer.json (indices
//   match the WaferBiome enum in
//   game/LithoKraft/src/game/include/lithokraft/game/wafer_biomes.h) plus the
//   6 JD-approved biomes not yet in wafer.json -- those rows are flagged
//   inWaferJson=false and carry waferBiomeIndex=-1. Fab Ruins is in the
//   ORGANIC, not the Wafer (dimension="Organic").
//
// DENSITY SEMANTICS: treeDensity / shrubDensity are plain per-biome floats in
//   [0, 1]: the per-column placement probability for that biome. They are
//   shaped for later consumption via procgen::blendedDensity(): copy them
//   into a procgen::BiomeDensity with fillTreeDensity()/fillShrubDensity()
//   (live Wafer indices only) and a placement stage reads the effective
//   density with blendedDensity(densities, maps, column), which folds in the
//   biome blend weights automatically.
//
// SCOPE: mapping, plus the placement path for the CrossedQuads rows.
//   2026-10-12: the live rows (DicingPlains DryTuft 0.012, ResonatorSpires
//   DryTuft 0.020) were placed by the "shrubs_dry_tuft" groundcover feature
//   in assets/worldgen/wafer.json, rendering through the silica_grass Cross
//   block. Wiring pass B4 (2026-10-13) completed the binding: all seven
//   procgen plant textures are registered Cross blocks with real pack
//   assets (tools/plant_texture_gen.cpp), and the groundcover entries now
//   place each biome's formula-family block (DicingPlains -> formula leaf,
//   ResonatorSpires -> formula blob, per the FORMULA ROWS below) at the
//   same positions and densities as before. The Modeled3D rows (TraceForest
//   Berry, CapacitorGroves Twisted) are still NOT placed: octree shrubs do
//   not stamp into voxel chunks yet — that caller (octree -> chunk stamp,
//   or a modeled-shrub entity path) is builder work. The generator side is
//   ready: generateShrubOctree now shapes shrub bodies with Litho Formula
//   cross-sections (see shrubs.h super* params).
//
// FORMULA ROWS (2026-10-12): a row may carry Litho Formula params (superM >
//   0) in its trailing super* fields. For CrossedQuads rows those params make
//   generateShrubCross select a formula-silhouette sprite from the engine's
//   formula shrub texture family (shrubs.h) instead of one of the three
//   legacy plant textures: DicingPlains (leaf lens), ResonatorSpires (soft
//   blob), StaticMarsh (six-petal flower). The values mirror the canonical
//   family entries exactly, so the cross path's exact-match rule binds them.
//   shrubParamsForRow() below is the row -> ShrubParams plumbing: it starts
//   from the row kind's engine preset and applies the row's formula fields
//   when superM > 0 (Modeled3D presets' own formula cross-sections, which
//   the rows leave unset, are preserved).

#pragma once

#include <cstddef>
#include <cstdint>

#include <litholattice/procgen/shrubs.h>
#include <litholattice/procgen/stages.h>

namespace lithokraft::vegetation {

/// Which tree family a biome grows (None = no trees).
enum class TreePresence : uint8_t {
  None = 0,
  ClockwoodWirewood = 1,  ///< engine preset clockwoodWirewood()
  DopantCrystalTree = 2,  ///< engine preset dopantCrystalTree()
  TraceForestTree = 3,    ///< engine preset traceForestTree()
};

/// Which render style a biome's shrubs/grass take (None = no shrubs).
enum class ShrubStyle : uint8_t {
  None = 0,
  Modeled3D = 1,    ///< subdiv voxel shrub (generateShrubOctree)
  CrossedQuads = 2, ///< 2D cross sprite (generateShrubCross + texture set)
};

/// Which shrub preset reads best in the biome (flavour for the placement
/// stage; the mapping itself only fixes the style + density).
enum class ShrubKind : uint8_t {
  None = 0,
  Berry = 1,       ///< presetBerryShrub, 3D
  Twisted = 2,     ///< presetTwistedShrub, 3D
  MeadowTuft = 3,  ///< presetMeadowTuft, 2D
  DryTuft = 4,     ///< presetDryTuft, 2D
};

struct BiomeVegetation {
  const char* name;       ///< biome name, e.g. "TraceForest"
  const char* dimension;  ///< "Wafer" or "Organic"
  bool inWaferJson;       ///< false for the 6 JD-approved, not-yet-added biomes
  int waferBiomeIndex;    ///< 0..4 matching WaferBiome/wafer.json; -1 if n/a
  TreePresence tree;
  float treeDensity;      ///< per-column placement probability in [0, 1]
  ShrubKind shrub;
  ShrubStyle shrubStyle;
  float shrubDensity;     ///< per-column placement probability in [0, 1]
  const char* notes;
  // Litho Formula params for this row's shrubs (engine ShrubParams super*
  // semantics; defaults = no formula). Applied by shrubParamsForRow() when
  // superM > 0. For CrossedQuads rows they select the shrub's silhouette
  // sprite from the engine's formula texture family.
  double superM = 0.0;
  double superN1 = 1.0;
  double superN2 = 1.0;
  double superN3 = 1.0;
  double superA = 1.0;
  double superB = 1.0;
};

inline constexpr BiomeVegetation kBiomeVegetationTable[] = {
    // --- live Wafer biomes (assets/worldgen/wafer.json) -------------------
    {"TraceForest", "Wafer", true, 0, TreePresence::TraceForestTree, 0.060f,
     ShrubKind::Berry, ShrubStyle::Modeled3D, 0.100f,
     "Dense circuit-trees (copper trunks, red/blue glowing tips); berry shrubs."},
    {"DicingPlains", "Wafer", true, 1, TreePresence::None, 0.0f,
     ShrubKind::DryTuft, ShrubStyle::CrossedQuads, 0.012f,
     "Sparse first-spawn territory; scrub tufts along the die-grid scribe "
     "lines. Formula sprite: dry leaf lens.",
     4.0, 0.6, 0.6, 0.6, 1.8, 1.0},
    {"CapacitorGroves", "Wafer", true, 2, TreePresence::ClockwoodWirewood,
     0.045f, ShrubKind::Twisted, ShrubStyle::Modeled3D, 0.060f,
     "Maze-like groves of tall thin wirewood; twisted copper scrub."},
    {"ResonatorSpires", "Wafer", true, 3, TreePresence::None, 0.0f,
     ShrubKind::DryTuft, ShrubStyle::CrossedQuads, 0.020f,
     "The quartz spires are the feature; only sparse dry tufts. Formula "
     "sprite: soft three-lobe blob.",
     3.0, 1.5, 1.5, 1.5, 1.0, 1.0},
    {"DeadZones", "Wafer", true, 4, TreePresence::None, 0.0f, ShrubKind::None,
     ShrubStyle::None, 0.0f, "Grey, unpowered, no glow: nothing grows."},
    // --- JD-approved, not yet in wafer.json --------------------------------
    {"EtchedPlains", "Wafer", false, -1, TreePresence::None, 0.0f,
     ShrubKind::DryTuft, ShrubStyle::CrossedQuads, 0.025f,
     "Etched flats; wind-combed dry scrub. Index TBD when added to wafer.json."},
    {"ClockwoodForest", "Wafer", false, -1, TreePresence::ClockwoodWirewood,
     0.085f, ShrubKind::Berry, ShrubStyle::Modeled3D, 0.075f,
     "Densest wirewood stands on the Wafer. Index TBD when added to wafer.json."},
    {"DopantGroves", "Wafer", false, -1, TreePresence::DopantCrystalTree,
     0.075f, ShrubKind::Twisted, ShrubStyle::Modeled3D, 0.065f,
     "Faceted crystal groves. Index TBD when added to wafer.json."},
    {"OxideDunes", "Wafer", false, -1, TreePresence::None, 0.0f,
     ShrubKind::DryTuft, ShrubStyle::CrossedQuads, 0.030f,
     "Dune grass tufts; no trees on shifting oxide. Index TBD."},
    {"StaticMarsh", "Wafer", false, -1, TreePresence::None, 0.0f,
     ShrubKind::MeadowTuft, ShrubStyle::CrossedQuads, 0.110f,
     "Reed beds; 2D crossed quads read better over water. Formula sprite: "
     "six-petal flower.",
     6.0, 2.0, 4.0, 4.0, 1.0, 1.0},
    {"FabRuins", "Organic", false, -1, TreePresence::None, 0.0f,
     ShrubKind::DryTuft, ShrubStyle::CrossedQuads, 0.020f,
     "Weeds in the ruins. NOTE: Organic dimension, not the Wafer."},
};

inline constexpr int kBiomeVegetationCount = 11;

/// Fill a BiomeDensity with the table's tree densities at the live Wafer
/// biome indices (0..4); all other entries are 0. Later, when the 6 new
/// biomes land in wafer.json, their indices get entries here too.
inline void fillTreeDensity(litholattice::procgen::BiomeDensity& d) {
  d.density.fill(0.f);
  for (const BiomeVegetation& row : kBiomeVegetationTable)
    if (row.waferBiomeIndex >= 0)
      d.density[size_t(row.waferBiomeIndex)] = row.treeDensity;
}

/// Same for shrub densities.
inline void fillShrubDensity(litholattice::procgen::BiomeDensity& d) {
  d.density.fill(0.f);
  for (const BiomeVegetation& row : kBiomeVegetationTable)
    if (row.waferBiomeIndex >= 0)
      d.density[size_t(row.waferBiomeIndex)] = row.shrubDensity;
}

/// Build the engine ShrubParams for one placement of a row's shrub: the row
/// kind's engine preset (Berry/Twisted/MeadowTuft/DryTuft), re-seeded with
/// `seed`, restyled to the row's shrubStyle, with the row's Litho Formula
/// fields applied when the row carries them (superM > 0). Rows without
/// formula fields keep their preset's super* values (the Modeled3D presets'
/// cross-sections; the tuft presets' round default). ShrubKind::None yields
/// a default (formula-free, Modeled3D) params with only the seed set — callers
/// must not place shrubs for such rows (their shrubDensity is 0).
inline litholattice::procgen::ShrubParams shrubParamsForRow(
    const BiomeVegetation& row, uint64_t seed) {
  namespace pg = litholattice::procgen;
  pg::ShrubParams p;
  switch (row.shrub) {
    case ShrubKind::Berry: p = pg::presetBerryShrub(); break;
    case ShrubKind::Twisted: p = pg::presetTwistedShrub(); break;
    case ShrubKind::MeadowTuft: p = pg::presetMeadowTuft(); break;
    case ShrubKind::DryTuft: p = pg::presetDryTuft(); break;
    case ShrubKind::None: break;
  }
  p.seed = seed;
  if (row.shrubStyle == ShrubStyle::Modeled3D)
    p.style = pg::PlantStyle::Modeled3D;
  else if (row.shrubStyle == ShrubStyle::CrossedQuads)
    p.style = pg::PlantStyle::CrossedQuads;
  if (row.superM > 0.0) {
    p.superM = row.superM;
    p.superN1 = row.superN1;
    p.superN2 = row.superN2;
    p.superN3 = row.superN3;
    p.superA = row.superA;
    p.superB = row.superB;
  }
  return p;
}

}  // namespace lithokraft::vegetation
