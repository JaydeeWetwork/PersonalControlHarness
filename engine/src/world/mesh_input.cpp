#include "litholattice/world/mesh_input.h"

#include <vector>

namespace litholattice {

void snapshotForMeshingInto(const ChunkMap& w, const BlockRegistry& reg, int cx, int sy, int cz,
                            uint32_t epoch, uint64_t seq, BlockId belowWorld, MeshInput& out) {
  MeshInput* in = &out;
  in->cx = cx; in->sy = sy; in->cz = cz; in->epoch = epoch; in->seq = seq;
  in->matPaletteSize = 0;  // recycled buffers may carry a stale palette size
  if (const ChunkColumn* col = w.column(cx, cz)) {  // PBR phase 2 (Worker 3): the palette
    in->matPaletteSize = col->materialPaletteSize;  // rides the snapshot so the BinaryQuad
    for (size_t i = 0; i < col->materialPaletteSize && i < 256; ++i)  // path can resolve
      in->matPalette[i] = col->materialPalette[i];  // material bytes without the live map
  }
  for (int dy = -1; dy <= 1; ++dy)
    for (int dz = -1; dz <= 1; ++dz)
      for (int dx = -1; dx <= 1; ++dx) {
        const SubChunk* s = w.sub(cx + dx, sy + dy, cz + dz);
        const bool under = (sy + dy) < 0;
        // Resolve the StableId palette to BlockIds once per sub-chunk; voxels
        // below are O(1) raw-index lookups. At 64-bit width the index IS the
        // StableId, so those resolve individually (unreachable in practice).
        std::vector<BlockId> lut;
        const bool w64 = s && s->blocks.width() == VoxelPalette::Width::W64;
        if (s && !w64) s->blocks.resolveBlockIds(reg, lut);
        const int x0 = dx < 0 ? 15 : 0, x1 = dx > 0 ? 0 : 15;
        const int y0 = dy < 0 ? 15 : 0, y1 = dy > 0 ? 0 : 15;
        const int z0 = dz < 0 ? 15 : 0, z1 = dz > 0 ? 0 : 15;
        for (int y = y0; y <= y1; ++y)
          for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
              const int i = meshPadIndex(x + 1 + dx * 16, y + 1 + dy * 16, z + 1 + dz * 16);
              const int si = SubChunk::index(x, y, z);
              BlockId b;
              if (under || !s) {
                b = under ? belowWorld : kAir;  // below the world: the fill block
              } else if (w64) {
                const auto id = reg.findByStableId(s->blocks.at(si));
                b = id ? *id : kAir;
              } else {
                b = lut[s->blocks.rawIndex(si)];
              }
              in->id[i] = b;
              in->st[i] = s ? s->state[si] : 0;
              in->mat[i] = s ? s->materials[si] : 0;
            }
      }
}

std::shared_ptr<MeshInput> snapshotForMeshing(const ChunkMap& w, const BlockRegistry& reg, int cx, int sy,
                                              int cz, uint32_t epoch, uint64_t seq, BlockId belowWorld) {
  auto in = std::make_shared<MeshInput>();
  snapshotForMeshingInto(w, reg, cx, sy, cz, epoch, seq, belowWorld, *in);
  return in;
}

}  // namespace litholattice
