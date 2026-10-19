// BinaryQuad -> GPU vertex expansion for the PBR path (PBR phase 2, Worker 3).
//
// WHAT: PbrQuadVertex (the lean GL vertex the BinaryQuad path draws with),
//   PbrChunkMesh (the upload unit, parallel to ChunkMesh), and
//   expandBinaryQuadsToPbr (quads -> 4 verts each, resolving the 8-bit material
//   byte through the column palette to a compact registry ID).
// WHY: JD's locked coexistence (2026-10-11) - BinaryQuad and PackedVertex coexist as
//   a user-selectable mesher option. The BinaryQuad path carries material IDs natively
//   (BinaryQuad::material is the 8-bit per-voxel palette index, passthrough); this file
//   is where those bytes become the compact IDs the chunk_pbr shaders index the
//   material table with. PackedVertex is NEVER extended (phase 1's Option B note is
//   superseded); the classic chunk programs and the mesher golden hash are untouched.
// HOW: the game mesh job calls binaryMeshSubChunk (with MeshInput::mat, Worker 1's
//   gap closed here) and then this expansion, then IRenderer::uploadPbrChunkMesh.
//
// VERTEX LAYOUT (20 bytes; matches assets/shaders/chunk_pbr.vert locations):
//   loc 0: uvec3 aPos      - 1/16 block units, local to the sub-chunk (x,y,z)
//   loc 1: uint  aFace      - 0..5 (+X,-X,+Y,-Y,+Z,-Z, same order as BinaryQuad::dir)
//   loc 2: uint  aMaterial  - compact MaterialRegistry ID (flat per quad)
//   loc 3: uvec2 aLight     - flood-fill block/sky level 0..64 (flat per quad)
//
// Corner winding: the 4 verts of a quad are emitted so that triangles (0,1,2) and
// (0,2,3) are counter-clockwise seen from the face normal - the same convention as
// the classic path's shared quad index buffer (0,1,2 / 0,2,3 with GL_CCW front and
// back-face culling). Verified per direction by the headless winding test.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "litholattice/render/binary_greedy_mesher.h"
#include "litholattice/render/renderer.h"  // kMaxQuadsPerMesh

namespace litholattice {

/// Lean GPU vertex for the BinaryQuad/PBR path. Positions are 1/16 block units local
/// to the sub-chunk (the uOffset uniform carries the sub-chunk origin, same as the
/// classic path). material/light are flat per quad (the shader declares them flat).
#pragma pack(push, 1)
struct PbrQuadVertex {
  uint16_t x = 0, y = 0, z = 0;  ///< aPos: 1/16 block units, local to the sub-chunk
  uint16_t _pad0 = 0;
  uint32_t face = 0;      ///< aFace: 0..5, same order as BinaryQuad::dir
  uint32_t material = 0;  ///< aMaterial: compact MaterialRegistry ID
  uint8_t blockLight = 0, skyLight = 0;  ///< aLight: flood-fill levels 0..64
  uint8_t _pad1[2]{};
};
#pragma pack(pop)
static_assert(sizeof(PbrQuadVertex) == 20, "PBR quad vertex layout");
static_assert(offsetof(PbrQuadVertex, x) == 0, "aPos at 0");
static_assert(offsetof(PbrQuadVertex, face) == 8, "aFace at 8");
static_assert(offsetof(PbrQuadVertex, material) == 12, "aMaterial at 12");
static_assert(offsetof(PbrQuadVertex, blockLight) == 16, "aLight at 16");

/// Upload unit for the BinaryQuad path, parallel to ChunkMesh. Empty verts = remove.
struct PbrChunkMesh {
  int cx = 0, sy = 0, cz = 0;
  uint32_t epoch = 0;
  uint64_t seq = 0;
  std::vector<PbrQuadVertex> verts;  // quads (4 verts each)
};

/// Copies the interior 16^3 material bytes out of a MeshInput snapshot (which stores
/// the 18^3 padded volume) into the dense 16^3 layout binaryMeshSubChunk expects
/// ((y*16+z)*16+x, per binary_greedy_mesher.h). The 4 KiB copy keeps the mesher's
/// contract - and the six optimizations' binary mesher - untouched.
inline void extractMaterial16(const MeshInput& in, uint8_t out[16 * 16 * 16]) {
  for (int y = 0; y < 16; ++y)
    for (int z = 0; z < 16; ++z)
      for (int x = 0; x < 16; ++x)
        out[(y * 16 + z) * 16 + x] = in.mat[meshPadIndex(x + 1, y + 1, z + 1)];
}

/// Expands merged quads into 4 vertices each for the PBR draw path.
///   palette[0..paletteSize) maps the quad's 8-bit material byte to a compact
///     registry ID (the column's materialPalette); a byte >= paletteSize resolves to
///     0, the built-in default (same rule as the world-side provider).
///   blockLight/skyLight are stamped flat per quad (coarse game-supplied levels until
///     the flood fill is wired; see the game-side call site).
/// Caps at kMaxQuadsPerMesh quads like the classic mesher (logs and trims).
/// Deterministic: same quads in -> bit-identical verts out.
void expandBinaryQuadsToPbr(const std::vector<BinaryQuad>& quads, const uint16_t* palette,
                            size_t paletteSize, uint8_t blockLight, uint8_t skyLight,
                            std::vector<PbrQuadVertex>& out);

}  // namespace litholattice
