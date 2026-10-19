// Game-side ProcGen vegetation API (workstream 3 of 3, JD-authorized 2026-10-11).
//
// WHAT: the "exposed" vegetation API players and mods will eventually call. It
//   consumes the engine core (workstreams 1+2:
//   engine/src/procgen/include/litholattice/procgen/trees.h and shrubs.h),
//   binds the engine's caller-supplied material slots to the GAME palette
//   (lithokraft::model::authored::palette -- ids are stable, never renumber),
//   and returns ready-to-save lithokraft::model::Model values.
//
// DETERMINISM CONTRACT: the same (params, seed) always yields the same plant --
//   identical leaf stream (verify with litholattice::subdiv::hashLeaves) and,
//   after saveModel, a byte-identical .lthm file. No wall-clock, no thread
//   identity, no hash-map iteration anywhere in this path.
//
// MATERIAL BINDING: the engine family presets leave placeholder slot ids in
//   the material fields ("Caller remap" comments in trees.cpp / shrubs.cpp).
//   bindTreePalette()/bindShrubPalette() translate those placeholders to real
//   game palette ids; generateTreeModel()/generateShrubModel() apply the
//   binding automatically on a copy of the caller's params. Params that did
//   NOT come from a family preset (unrecognized slot pattern) keep their
//   caller-supplied ids untouched -- hand-rolled params should therefore use
//   real palette ids (or 0), never the placeholder slots 3..9.

#pragma once

#include <cstdint>
#include <string>

#include <litholattice/procgen/shrubs.h>
#include <litholattice/procgen/trees.h>

#include "model.h"

namespace lithokraft::vegetation {

namespace procgen = litholattice::procgen;

/// Remap an engine tree preset's placeholder material slots to the game
/// palette (in place). Recognized patterns (see trees.cpp "Caller remap"):
///   clockwoodWirewood (trunk=3, leaf=0, tip=0) -> trunk = kCopper
///   dopantCrystalTree (trunk=4, leaf=5, tip=6) -> trunk = kCrystalBlue,
///       leaf = kCrystalAmber, tip = kCrystalCore
///   traceForestTree   (trunk=7, leaf=9, tip=8) -> trunk = kCopper,
///       leaf = kTraceBlue, tip = kTraceRed (two-colour red/blue tips)
/// Any other slot pattern is left untouched.
void bindTreePalette(procgen::TreeParams& p);

/// Remap an engine shrub preset's placeholder material slots to the game
/// palette (in place). Recognized patterns (see shrubs.cpp presets):
///   presetBerryShrub   (stem=3, leaf=4) -> stem = kHandleWood, leaf = kTraceRed
///   presetTwistedShrub (stem=3, leaf=5) -> stem = kCopper,     leaf = kShellDark
///   presetMeadowTuft / presetDryTuft (stem=1, leaf=2) -> stem = kHandleWood,
///       leaf = kShellDark (neutral stand-in; the 2D path's colour is the tint)
/// Any other slot pattern is left untouched.
void bindShrubPalette(procgen::ShrubParams& p);

/// Generate a tree Model: bindTreePalette() on a copy of `p`, then
/// procgen::generateTree(). Ready for saveModel(). `author` may be empty.
model::Model generateTreeModel(const procgen::TreeParams& p, std::string name,
                              std::string author = "");

/// The 2D render bundle for a crossed-quad shrub: the parameter set the
/// game's block appearance feeds the Shape::Cross path (see shrubs.h "2D
/// RENDER BINDING"). Filled by generateShrubModel() when the params select
/// PlantStyle::CrossedQuads and `crossOut` is non-null.
struct ShrubCrossInfo {
  float widthBlocks = 0.f;    ///< footprint in blocks (from CrossQuadPlant)
  float heightBlocks = 0.f;
  uint32_t textureIndex = 0;  ///< index into the procedural texture set
  const char* textureName = nullptr;  ///< plantTextureNameAny(textureIndex)
  uint8_t tintR = 255, tintG = 255, tintB = 255;
};

/// Generate a shrub Model: bindShrubPalette() on a copy of `p`, then the
/// engine generator matching p.style.
///
///   Modeled3D   -> procgen::generateShrubOctree(); `crossOut` untouched.
///   CrossedQuads -> procgen::generateShrubCross(); `crossOut` (when non-null)
///                  receives the 2D bundle, and the Model geometry holds a thin
///                  crossed-plane voxel stand-in (two 1-cell-thick vertical
///                  planes forming an X, leaf material, stem-material core) so
///                  the .lthm stays meaningful as a 3D fallback / placement
///                  marker. The authoritative 2D representation is the
///                  ShrubCrossInfo bundle, not the voxels.
model::Model generateShrubModel(const procgen::ShrubParams& p, std::string name,
                                std::string author = "",
                                ShrubCrossInfo* crossOut = nullptr);

}  // namespace lithokraft::vegetation
