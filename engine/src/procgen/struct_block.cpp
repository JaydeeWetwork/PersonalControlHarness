// Structure block capture/place helpers.
#include "litholattice/procgen/struct_block.h"

#include <algorithm>
#include <cstdint>

#include "litholattice/procgen/struct_registry.h"

namespace litholattice::procgen {
namespace {

// rotXZ lives in stages.cpp (anonymous namespace); replicate the 4-rotation here.
void rotXZ(int dx, int dz, int rot, int& rx, int& rz) {
  switch (rot & 3) {
    case 0: rx = dx; rz = dz; break;
    case 1: rx = -dz; rz = dx; break;
    case 2: rx = -dx; rz = -dz; break;
    default: rx = dz; rz = -dx; break;
  }
}

}  // namespace

std::vector<BlueprintCell> captureRegion(BlockReader reader, int32_t minX, int32_t minY,
                                         int32_t minZ, int32_t maxX, int32_t maxY,
                                         int32_t maxZ, MaterialId structureBlockMaterial) {
  std::vector<BlueprintCell> cells;
  // Bounds guard: a hostile/mistaken region (e.g. maxY == INT32_MAX) would
  // overflow the loop counter (UB) or OOM the cell vector. 128 per axis
  // (2M cells) is beyond any sane structure.
  const int64_t sx = int64_t(maxX) - int64_t(minX), sy = int64_t(maxY) - int64_t(minY),
                sz = int64_t(maxZ) - int64_t(minZ);
  if (sx < 0 || sy < 0 || sz < 0 || sx > 128 || sy > 128 || sz > 128) return cells;
  for (int32_t y = minY; y <= maxY; ++y)
    for (int32_t z = minZ; z <= maxZ; ++z)
      for (int32_t x = minX; x <= maxX; ++x) {
        const MaterialId m = reader(x, y, z);
        if (m == kAirMaterial || m == structureBlockMaterial) continue;
        BlueprintCell c;
        c.dx = int(x - minX);
        c.dy = int(y - minY);
        c.dz = int(z - minZ);
        c.material = m;
        cells.push_back(c);
      }
  // Deterministic order: (dy, dz, dx).
  std::sort(cells.begin(), cells.end(), [](const BlueprintCell& a, const BlueprintCell& b) {
    if (a.dy != b.dy) return a.dy < b.dy;
    if (a.dz != b.dz) return a.dz < b.dz;
    return a.dx < b.dx;
  });
  return cells;
}

size_t placeCells(BlockWriter writer, const std::vector<BlueprintCell>& cells, int32_t bx,
                  int32_t by, int32_t bz, int rot) {
  size_t n = 0;
  for (const auto& c : cells) {
    int rdx, rdz;
    rotXZ(c.dx, c.dz, rot, rdx, rdz);
    if (writer(bx + rdx, by + c.dy, bz + rdz, c.material)) ++n;
  }
  return n;
}

bool saveStructureAs(BlockReader reader, std::string_view name, int32_t minX, int32_t minY,
                     int32_t minZ, int32_t maxX, int32_t maxY, int32_t maxZ,
                     MaterialId structureBlockMaterial) {
  auto cells = captureRegion(reader, minX, minY, minZ, maxX, maxY, maxZ, structureBlockMaterial);
  if (cells.empty()) return false;
  // NOTE (JD 2026-10-12): SAVE captures register UNSIGNED. JD signs structures
  // later with an offline tool (Ed25519 over the fingerprint, same trust model
  // as mod signing), or not at all for personal use. Unsigned structures load
  // fine; verifyEntry() reports them as Unsigned.
  StructureEntry e;
  e.name = std::string(name);
  StructureVariant v;
  v.cells = std::move(cells);
  v.weight = 1.f;
  e.variants.push_back(std::move(v));
  return StructureRegistry::instance().registerInGame(std::move(e));
}

size_t loadStructureAt(std::string_view name, BlockWriter writer, int32_t bx, int32_t by,
                       int32_t bz, int rot) {
  const StructureEntry* e = StructureRegistry::instance().find(name);
  if (!e || e->variants.empty()) return 0;
  return placeCells(writer, e->variants.front().cells, bx, by, bz, rot);
}

}  // namespace litholattice::procgen
