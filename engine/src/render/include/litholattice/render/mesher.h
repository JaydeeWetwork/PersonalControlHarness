// Voxel mesher: MeshInput snapshot (world) -> ChunkMesh (render). Pure function, runs on
// worker threads. Shapes come from the game's BlockRegistry, texture layers from BlockMaterials,
// and everything game-specific (custom shapes, shader modes) from its IBlockAppearance.
#pragma once
#include <functional>
#include "litholattice/render/renderer.h"
#include "litholattice/world/mesh_input.h"
#include "litholattice/render/trace_strength_buffer.h"

namespace litholattice {
class BlockMaterials;
class IBlockAppearance;

/// Wafer ground-pulse params for one chunk column, as two shader-ready vec4s:
/// p0 = (freqHz, sharpness, amplitude, 0), p1 = (tintR, tintG, tintB, 0).
/// The game fills these from its biome weight vector (see ground_pulse.h).
using PulseParamsFn = std::function<void(int chunkX, int chunkZ, float p0[4], float p1[4])>;

/// Wafer ground-trace params for one chunk column, as two shader-ready vec4s:
/// t0 = (density, speed, amplitude, dirMix), t1 = (tintR, tintG, tintB, 0).
/// The game fills these from the same biome weight vector as the pulse
/// (see ground_traces.h).
using TraceParamsFn = std::function<void(int chunkX, int chunkZ, float t0[4], float t1[4])>;

/// What the mesher reads besides the snapshot. All of it is read-only and shared by workers.
///
/// DEFINITIONS CONTRACT (P3-3, Appendix D M4): unlike the voxel data —
/// which arrives as the immutable MeshInput snapshot — block definitions
/// are read LIVE from `blocks` on worker threads. That is safe per-read
/// (the registry hands out defs by value under a shared lock), and
/// meshSubChunk guards the one remaining hazard, a refreshStaged() landing
/// mid-mesh, with a definitionEpoch before/after check + one re-mesh.
/// `materials` and `appearance` carry no epoch: callers must not mutate
/// them while meshes are in flight (they are built at load and treated as
/// immutable thereafter — the same standing contract as before).
struct MeshContext {
  const BlockRegistry& blocks;
  const BlockMaterials& materials;
  const IBlockAppearance* appearance = nullptr;  ///< nullptr: plain textured shapes
  const TraceStrengthBuffer* strengths = nullptr;  ///< Step 1: slot per trace cell (null = bake strength)
  PulseParamsFn pulseParams = nullptr;  ///< null = meshes carry zero pulse params (no pulse)
  TraceParamsFn traceParams = nullptr;  ///< null = meshes carry zero trace params (no traces)
};

/// Builds the face-culled, textured mesh of one sub-chunk. Epoch-guarded:
/// if block definitions change mid-pass, the mesh is rebuilt once against
/// the new set (see MeshContext contract above).
ChunkMesh meshSubChunk(const MeshInput& in, const MeshContext& ctx);
}  // namespace litholattice
