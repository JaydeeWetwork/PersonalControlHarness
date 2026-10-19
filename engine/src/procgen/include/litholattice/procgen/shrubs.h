// Procedural grass/shrub generator (JD-authorized 2026-10-11, ProcGen vegetation
// workstream 2 of 3). Lives in the ENGINE; the game (workstream 3) consumes it.
//
// DUAL OUTPUT (JD's explicit call — "we like options"): every plant carries a
// `style` flag selecting one of two render paths:
//
//   (a) Modeled3D    — a small noisy shrub mound baked into a subdiv::SparseOctree
//                      (subdivision geometry; the mesher renders micro-cells).
//   (b) CrossedQuads — a Minecraft-style cross sprite: width/height/tint plus an
//                      index into the procedural plant texture set below. The
//                      engine already renders this as Shape::Cross (two crossed
//                      vertical quads, alpha-tested); see "2D RENDER BINDING".
//
// DETERMINISM: every entry point is a pure function of (params, seed). The
// engine RNG is litholattice::procgen::Rng (seed.h: xoshiro256** seeded through
// SplitMix64) — integer-only, identical on every platform and thread.
//
// FAIL CLOSED: invalid params (sizeCells outside 1..64, density/noiseAmount
// non-finite or outside 0..1, zero stem/leaf material) yield an EMPTY octree /
// a ZEROED CrossQuadPlant. Never throws, never reads out of bounds.
//
// 2D RENDER BINDING (how a game block wires textureIndex -> rendered cross quads):
//   1. Register one BlockDef per plant texture with shape = Shape::Cross and
//      tex = plantTextureName(i), e.g. tex = "procgen_grass_tuft". (Silica grass
//      is the in-repo precedent: BlockId 53, Shape::Cross, tex "silica_grass" —
//      game/LithoKraft/src/content/blocks.cpp, silica.h.)
//   2. Upload plantTexturePixels(i) (16x16 RGBA8, row-major, row 0 = top,
//      alpha 0 = cutout) into the texture pack under that tex name, exactly as
//      any other block texture. BlockMaterials::face(block, FaceSlot::Top,
//      active) then resolves it to a texture-array layer + UV rect.
//   3. The mesher's emitCross() (engine/src/render/mesher.cpp) draws the two
//      crossed vertical quads sampling that layer with kShadeCutout
//      (alpha-tested). Per-plant tint (CrossQuadPlant::tintR/G/B) and footprint
//      (widthBlocks/heightBlocks) are applied by the game's block appearance /
//      custom geometry for the block — CrossQuadPlant is the parameter bundle
//      the game feeds that path.
//   Workstream 3 owns the game-side registry entries; this header only supplies
//   the pixel data, the stable names, and the per-plant parameters.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include "litholattice/subdiv/octree.h"

namespace litholattice::procgen {

/// Which render path a plant takes. The 3D path reads this; the 2D path
/// (generateShrubCross) ignores it — a cross plant is a cross plant.
enum class PlantStyle : uint8_t { Modeled3D = 0, CrossedQuads = 1 };

struct ShrubParams {
  uint64_t seed = 0;
  PlantStyle style = PlantStyle::Modeled3D;
  int sizeCells = 12;        ///< overall size in level-6 micro-cells, 1..64
  float density = 0.6f;      ///< 0..1 fill of the plant volume
  float noiseAmount = 0.4f;  ///< 0..1 shape noise
  uint8_t stemMaterial = 1;  ///< nonzero palette/material index
  uint8_t leafMaterial = 2;  ///< nonzero palette/material index
  // --- Litho Formula (superformula) horizontal cross-section (3D path) -------
  // The mound's horizontal radius at angle theta is modulated by the 2D Litho
  // Formula r(theta) (litho_formula.h), mean-normalized so the overall volume
  // stays comparable across parameter sets: (rho / rNorm(theta))^2 + dyn^2 <= 1
  // replaces the plain ellipsoid test. m = 0 with n1=n2=n3=1, a=b=1 yields
  // r == 1 for all theta — the original round mound, bit-for-bit. Presets pick
  // per-kind silhouettes: berry = 5-lobe flower/star, twisted = 3-lobe.
  // Validation (fail closed): all finite, m in [0, 16], n1/n2/n3 in [0.05, 20],
  // a/b in [0.2, 5]. Since 2026-10-12 the 2D cross path reads these too: with
  // superM > 0 the cross plant's sprite is a Litho Formula silhouette
  // (generateShrubSprite / the formula texture family below). Both paths
  // share the same validation, like the other fields either ignores.
  double superM = 0.0;   ///< lobe count m (0 = round)
  double superN1 = 1.0;  ///< superformula exponents
  double superN2 = 1.0;
  double superN3 = 1.0;
  double superA = 1.0;   ///< superformula axis scales
  double superB = 1.0;
};

struct CrossQuadPlant {
  float widthBlocks = 0.5f;   ///< footprint in blocks
  float heightBlocks = 0.5f;
  uint32_t textureIndex = 0;  ///< index into the plant texture set (see below)
  uint8_t tintR = 255, tintG = 255, tintB = 255;
};

/// 3D path: noisy shrub mound in a SparseOctree over one block's volume,
/// centered horizontally, base at micro-cell y=0, spanning `sizeCells`
/// level-6 micro-cells. A stem column (stemMaterial) rises through a lumpy
/// leaf mound (leafMaterial) whose horizontal cross-section is the Litho
/// Formula r(theta) from ShrubParams' super* fields (mean-normalized);
/// `density` thins the leaves, `noiseAmount` displaces the mound surface.
/// Coarser octree levels are used for large
/// plants so the leaf count stays bounded (kMaxShrubLeaves, few hundred
/// typical). Invalid params -> empty octree.
litholattice::subdiv::SparseOctree generateShrubOctree(const ShrubParams& p);

/// 2D path: width/height/tint derived deterministically from (seed, sizeCells);
/// textureIndex uniform over the plant texture set — unless the params carry
/// a Litho Formula (superM > 0), in which case textureIndex addresses the
/// formula sprite family instead (see formulaTextureIndexFor below). The
/// `style` field is ignored. Invalid params -> zeroed plant (zero footprint
/// renders nothing).
CrossQuadPlant generateShrubCross(const ShrubParams& p);

/// True when the params pass defensive validation (the contract both
/// generators enforce before doing any work).
bool validShrubParams(const ShrubParams& p);

/// Hard cap on leaf cells in one generated shrub (safety rail for
/// sizeCells=64, density=1). Stems are exempt.
inline constexpr int kMaxShrubLeaves = 1024;

// --- procedural plant texture set (2D path) --------------------------------
// Three deterministic 16x16 RGBA8 sprites, generated by a fixed algorithm
// (hash-varied, no RNG state): byte-identical on every run and platform.
// Layout: row-major, row 0 = TOP of the sprite, 4 bytes per pixel (R,G,B,A);
// alpha 0 = fully transparent (the cutout shader discards it).
inline constexpr int kPlantTextureSize = 16;          ///< width/height in px
inline constexpr int kPlantTextureBytes = 16 * 16 * 4;  ///< RGBA8 bytes per texture

/// Number of textures in the set (3: grass tuft, fern, dry scrub).
uint32_t numPlantTextures();
/// Stable texture-pack name for texture `i` ("procgen_grass_tuft", ...).
/// The game registers BlockDef::tex with this name (see 2D RENDER BINDING).
const char* plantTextureName(uint32_t i);
/// Pointer to kPlantTextureBytes of RGBA8 pixels for texture `i`;
/// nullptr when `i` is out of range. Pointer stays valid for process lifetime.
const uint8_t* plantTexturePixels(uint32_t i);

// --- Litho Formula shrub sprites (2D path, JD directive 2026-10-12) ----------
// The super* fields stop being 3D-only: generateShrubSprite() rasterizes a
// plant sprite whose SILHOUETTE is the 2D Litho Formula outline
// (litho_formula_2d.h, built from the params' super* fields) and whose
// interior is filled with the same hash-varied leafy speckle as the fixed
// set above (plant greens, darker at the base, hash jitter, density-thinned
// rim), clipped to the silhouette; outside is transparent. A stem-brown
// suggestion is painted at the base centre. Deterministic like the rest of
// this header: a pure function of (params, pixelSize) — hashes are
// latticeHash over fixed salts, never RNG state, so the same (seed, params,
// size) is byte-identical on every run and platform. Coverage (silhouette +
// density thinning) is a function of the params alone; p.seed varies ONLY
// the colour speckle, never the alpha mask. Invalid params (validShrubParams fails) or a
// pixelSize outside [4, 256] yield an EMPTY vector (fail closed).
//
// FRAME MAPPING: the formula outline is centred on the origin; it is scaled
// uniformly so its bounding box fits the pixelSize x pixelSize frame,
// centred horizontally, with the bounding box's minimum-y (the shape's base)
// mapped to the frame's bottom row — the sprite stands on the ground line
// like the fixed textures do.
std::vector<uint8_t> generateShrubSprite(const ShrubParams& p, int pixelSize);

// The formula sprites join CrossQuadPlant::textureIndex addressing as an
// extension of the fixed set: indices [0, numPlantTextures()) stay the three
// legacy textures with their bytes untouched; formula sprites occupy
// [kFirstFormulaTextureIndex, kFirstFormulaTextureIndex +
// numFormulaShrubTextures()). The family below is a small canonical table
// (mirroring lithoPresets2D's flower/star/leaf/blob), each entry generated
// once by generateShrubSprite at kPlantTextureSize from fixed canonical
// params (fixed seed: the SET is seed-invariant, exactly like the legacy
// three — per-plant seed variation still arrives via index/tint/size).
inline constexpr uint32_t kFirstFormulaTextureIndex = 3;  ///< == legacy count
/// Number of canonical formula shrub textures in the family (4).
uint32_t numFormulaShrubTextures();
/// Stable texture-pack name for formula texture `j` ("procgen_formula_flower",
/// ...); nullptr when `j` is out of range.
const char* formulaShrubTextureName(uint32_t j);
/// Pointer to kPlantTextureBytes of RGBA8 pixels for formula texture `j`;
/// nullptr when out of range. Valid for process lifetime.
const uint8_t* formulaShrubTexturePixels(uint32_t j);
/// The canonical ShrubParams formula texture `j` was generated from;
/// nullptr when out of range. Valid for process lifetime.
const ShrubParams* formulaShrubTextureParams(uint32_t j);
/// The textureIndex generateShrubCross assigns a formula plant (p.superM > 0):
/// the family entry whose super* fields match p's exactly when one exists,
/// otherwise the entry with the nearest superM (ties: nearest superN1, then
/// lowest index) — the cross path quantizes arbitrary formula params to the
/// nearest family sprite; exact sprites for arbitrary params are available
/// from generateShrubSprite(). Only meaningful when p.superM > 0.
uint32_t formulaTextureIndexFor(const ShrubParams& p);

/// Total addressable textures: legacy set + formula family.
uint32_t numPlantTexturesTotal();
/// Unified accessors over [0, numPlantTexturesTotal()): indices below
/// kFirstFormulaTextureIndex resolve to the legacy set, the rest to the
/// formula family. nullptr when out of range. This is the binding point for
/// consumers (e.g. the game's ShrubCrossInfo) that resolve a CrossQuadPlant's
/// textureIndex without caring which half of the set it came from.
const char* plantTextureNameAny(uint32_t i);
const uint8_t* plantTexturePixelsAny(uint32_t i);

// --- example plants (presets) ------------------------------------------------
// Fixed seeds, ready to generate. The self-test instantiates all four and
// reports their generated stats (leaf counts / cross params).
ShrubParams presetBerryShrub();   ///< 3D: compact berry bush
ShrubParams presetTwistedShrub(); ///< 3D: tall sparse twisted shrub
ShrubParams presetMeadowTuft();   ///< 2D: lush meadow grass tuft
ShrubParams presetDryTuft();      ///< 2D: dry scrub tuft

/// Headless self-test (shrubs_selftest.cpp): prints one PASS / FAIL line per
/// check; returns the number of failures.
int runShrubsSelfTest();

}  // namespace litholattice::procgen
