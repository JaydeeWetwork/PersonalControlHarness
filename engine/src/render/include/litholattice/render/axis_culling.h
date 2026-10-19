// Axis-separated chunk meshes for backface culling.
// Splits a chunk mesh into 6 parts (one per face direction).
// At render time, only draw the 3 facing the camera → ~50% fewer vertices.
#pragma once
#include "litholattice/render/mesher.h"
#include <array>

namespace litholattice {

/// A chunk mesh split by face direction.
struct AxisSplitMesh {
  // 0:+X, 1:-X, 2:+Y, 3:-Y, 4:+Z, 5:-Z
  std::array<std::vector<PackedVertex>, 6> faces;
  int cx, sy, cz;

  /// Total vertex count across all 6.
  size_t totalVerts() const {
    size_t n = 0;
    for (auto& f : faces) n += f.size();
    return n;
  }
};

/// Split a ChunkMesh into 6 direction buckets.
/// Reads the face direction from PackedVertex (stored in the flags byte).
AxisSplitMesh splitByAxis(const ChunkMesh& mesh);

/// Which 3 face directions are visible from the camera?
/// camDir is the normalized camera forward vector.
/// Returns the indices (0-5) of the faces pointing toward the camera.
std::array<int, 3> visibleFaces(float camDirX, float camDirY, float camDirZ);

}  // namespace litholattice
