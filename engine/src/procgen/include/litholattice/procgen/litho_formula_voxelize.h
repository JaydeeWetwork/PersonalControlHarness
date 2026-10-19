// Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
// concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
// from the mathematical definition; no third-party code referenced. Underlying patent
// expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
// ambition of No Man's Sky (Hello Games).

// Voxelizer for the Litho Formula: samples the parametric surface on a grid,
// fills the interior (parity fill on the sampled shell, majority vote over the
// three axes), and emits filled micro-cells. Voxel-native from the start — the
// analytic surface is never meshed; everything downstream (octree, .lthm,
// subdivision mesher) is LithoLattice's own.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "litholattice/procgen/litho_formula.h"
#include "litholattice/subdiv/octree.h"

namespace litholattice::procgen::litho_formula {

/// Filled cells of one voxelized Litho Formula shape.
/// Coordinates are integer cell indices in [0, resolution) on each axis;
/// the shape is centered in the grid with (at least) a one-cell border.
/// Cells are listed in ascending (z, y, x) order (x fastest), with no
/// duplicates. Deterministic: identical parameters always yield the
/// identical list.
struct VoxelResult {
  int resolution = 0;
  std::vector<std::array<int, 3>> filled;
};

/// Sample the (pTheta, pPhi) shape and fill its interior on a
/// resolution^3 grid. `resolution` must be a power of two in {8, 16, 32, 64}
/// (matching the SparseOctree level range 3..6). Any other value is rejected:
/// the rejection is reported to stderr and an empty VoxelResult with
/// `resolution == 0` is returned.
///
/// Pipeline: (1) the parametric surface is sampled on a uniform grid --
/// theta in [-pi, pi], phi in [-pi/2, pi/2] -- at 8 samples per voxel edge
/// along theta and 4 along phi (equal angular density, >= 2x voxel density:
/// dense enough that neighbouring samples never span a full voxel, so no
/// surface region goes unsampled); the sampled bounding radius is mapped to
/// [0.5, resolution - 0.5] with one uniform scale, preserving aspect.
/// Formula (x, y, z) maps to grid (x, z, y), so the polar axis becomes grid
/// y (game up). (2) For each (x, y) column, the continuous min/max sample
/// z-coordinate is recorded. (3) Interior fill is a parity scan along z: a
/// cell is filled when its center lies within its column's sampled z-span.
/// Using the continuous span -- rather than whole binary shell cells --
/// keeps the volume bias near zero (a binary shell overcounts the surface
/// by ~0.5 cell per side), and bounding each column's fill by its own
/// samples means the fill can never leak past the shape. (4) The filled
/// cells are returned.
///
/// The returned `filled` list is non-empty for any non-degenerate parameter
/// set, bounded within the grid, free of NaN-derived coordinates, and holds
/// a single 6-connected component (isolated sampling speckle is discarded).
/// `material` is reserved for future use: VoxelResult carries no material
/// channel, so callers needing a specific material should use the
/// three-argument emitToOctree overload below.
VoxelResult voxelizeLithoShape(const LithoParams& pTheta, const LithoParams& pPhi,
                               int resolution, uint8_t material = 1);

/// Emit a VoxelResult into a SparseOctree at level log2(resolution)
/// (resolution 64 -> level 6) with material 1. VoxelResults with a
/// resolution outside {8, 16, 32, 64} (e.g. rejected results) emit nothing.
void emitToOctree(const VoxelResult& result, litholattice::subdiv::SparseOctree& tree);

/// Emit a VoxelResult into a SparseOctree at level log2(resolution) with an
/// explicit material id.
void emitToOctree(const VoxelResult& result, litholattice::subdiv::SparseOctree& tree,
                  uint8_t material);

}  // namespace litholattice::procgen::litho_formula
