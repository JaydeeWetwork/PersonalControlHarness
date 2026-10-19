// Procedural tree generator (workstream 1 of 3, ProcGen vegetation; JD-authorized 2026-10-11).
//
// WHAT: engine-side procedural vegetation. generateTree() builds a tree as a
//   litholattice::subdiv::SparseOctree (level-6 micro-cells inside one block volume),
//   which drops straight into SubdivStore::setBlockOctree(x, y, z, std::move(tree)).
//   Game-side wrapping into .lthm Models is workstream 3; this file is the exact seam
//   it consumes (do not reshape TreeParams / generateTree without coordinator sign-off).
//
// WHY engine-side: JD wants procedural vegetation exposed by the engine, not just game
//   code -- the engine owns SparseOctree, so the generator outputs octrees directly.
//
// HOW: deterministic recursive growth. All randomness flows through
//   litholattice::procgen::Rng (xoshiro256**, seeded via the splitmix64-family mix64
//   with the classic s += 0x9E3779B97F4A7C15 step, same lineage as the splitmix64
//   stream in game/LithoKraft/src/content/model/models.cpp). Same (params, seed)
//   always yields the identical leaf stream (verify with subdiv::hashLeaves).
//   Degenerate params are clamped fail-closed: you get an empty or minimal tree,
//   never a crash, never an unbounded build (segment budget caps the recursion).
//
// Two growth modes:
//   organic    -- tapered trunk column, then recursive branching: children grow from
//                 the parent direction rotated by branchAngle + noise, with
//                 length *= lengthFalloff and a shrinking radius; leaf blobs at
//                 branch ends, glowing tip pixels at extremities.
//   crystalline-- angular shards: a stacked column of alternately rotated boxes and
//                 octahedral (bipyramid) facets, with thin side shards jutting at
//                 angles and an apex shard. No smooth branching.
//
// Biome families (presets below):
//   1. clockwoodWirewood -- tall (56-64 cells), thin trunk (radius 2), sparse long
//      branches, copper-brown trunk material, few/no leaves.
//   2. dopantCrystalTree -- crystalline; expects the caller to pass faceted palette
//      slots: trunkMaterial = blue crystal slot, leafMaterial = amber crystal slot
//      (the shard layers alternate between the two), tipMaterial = white-hot core
//      slot for the apex shard (0 = no glowing apex).
//   3. traceForestTree -- in the family of the hand-authored trace_tree.lthm:
//      copper trunk, red AND blue glowing tips. Achieved through the two-colour
//      tip rule: when both leafMaterial and tipMaterial are nonzero, extremities
//      alternate tipMaterial / leafMaterial by extremity index (documented below).
//      The preset therefore passes leafMaterial = blue glow slot, tipMaterial = red
//      glow slot, and disables leaf blobs... but blobs are keyed off leafMaterial,
//      so the preset instead relies on sparse tiny blobs reading as circuit nodes.
//      See the preset for the exact values.
#pragma once
#include <cstdint>
#include "litholattice/subdiv/octree.h"

namespace litholattice::procgen {

/// Parameters for generateTree(). Material ids are caller-supplied (the game palette);
/// 0 = air / "no such part" (a zero trunk/leaf/tip material simply paints nothing).
struct TreeParams {
  uint64_t seed = 0;
  int heightCells = 48;        ///< total height in level-6 micro-cells, clamped 1..64
  int trunkRadiusCells = 3;    ///< trunk half-width in micro-cells, clamped 1..8
  int branchLevels = 3;        ///< recursion depth, clamped 0..6
  int branchesPerNode = 3;     ///< children per branch node, clamped 1..8
  float branchAngleRad = 0.5f; ///< spread angle, clamped 0..pi (NaN -> default)
  float lengthFalloff = 0.65f; ///< child length = parent * falloff; must be in (0,1), else default
  float noiseAmount = 0.3f;    ///< 0..1 directional noise on growth (NaN -> default)
  uint8_t trunkMaterial = 1;   ///< material ids are caller-supplied (game palette)
  uint8_t leafMaterial = 2;
  uint8_t tipMaterial = 0;     ///< 0 = no glowing tips
  bool crystalline = false;    ///< true = angular/faceted crystal growth, not organic
};

/// Build the tree. Deterministic in (params, seed): identical inputs give identical
/// leaf streams (same subdiv::hashLeaves, same leaves() order/content). Params are
/// sanitized fail-closed first (see header doc); degenerate input yields an empty or
/// minimal tree, never a crash. All output cells are level 6 (64^3 micro-cells), fit
/// in one block volume (x,z in [0,64), y in [0,64)), and the tree is at most
/// heightCells tall. Organic mode: tip pixels alternate tipMaterial/leafMaterial by
/// extremity index when BOTH are nonzero (two-colour tips, e.g. red/blue); with only
/// tipMaterial set every extremity is tipMaterial.
litholattice::subdiv::SparseOctree generateTree(const TreeParams& p);

// ---- biome family presets (documented values; seed picks the individual) ----

/// Clockwood wirewood: tall (56-64 cells), thin trunk (radius 2), sparse long
/// branches, copper-brown trunk material, no leaf blobs.
TreeParams clockwoodWirewood(uint64_t seed);

/// Dopant crystal-tree: crystalline = true. Palette slots the caller must supply:
///   trunkMaterial = blue crystal slot, leafMaterial = amber crystal slot
///   (shard layers alternate blue/amber), tipMaterial = white-hot core slot
///   for the apex shard (0 disables the apex glow).
TreeParams dopantCrystalTree(uint64_t seed);

/// TraceForest circuit-tree (family of the hand-authored trace_tree.lthm):
/// copper trunk, red AND blue glowing tips via the two-colour tip rule
/// (tipMaterial = red glow slot, leafMaterial = blue glow slot; tiny sparse leaf
/// blobs read as circuit nodes). Palette slots the caller must supply:
///   trunkMaterial = copper slot, tipMaterial = red glow slot,
///   leafMaterial = blue glow slot.
TreeParams traceForestTree(uint64_t seed);

/// Headless checks for the tree generator: determinism (hashLeaves equality AND
/// leaf-stream equality), seed sensitivity, degenerate-param safety, per-family
/// non-emptiness and bounds (<= 64 cells tall, inside the block volume), and
/// crystalline-vs-organic structural difference. Prints one PASS/FAIL line per
/// check; returns the number of failures.
int runTreeGenSelfTest();

}  // namespace litholattice::procgen
