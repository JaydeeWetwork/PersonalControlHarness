// lithokraft_chipor_selftest: headless ChipOR viewport tests (no window, no GL context).
// Workstream 4 (JD-locked 2026-10-11). Exit code = failure count. LF line endings.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lithokraft/content/appearance.h"
#include "lithokraft/content/blocks.h"
#include "lithokraft/content/chipor_viewport.h"
#include "litholattice/render/block_materials.h"
#include "litholattice/render/mesher.h"
#include "litholattice/render/renderer.h"
#include "litholattice/world/mesh_input.h"

namespace {
int g_fails = 0;
void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++g_fails;
}

bool sameCell(const litholattice::subdiv::MicroCell& a, const litholattice::subdiv::MicroCell& b) {
  return a.level == b.level && a.x == b.x && a.y == b.y && a.z == b.z && a.material == b.material;
}

// One sub-chunk snapshot with a single non-air cell at (8,8,8); `above` sets the cell at (8,9,8).
litholattice::MeshInput makeInput(litholattice::BlockId id, litholattice::BlockId above) {
  litholattice::MeshInput in{};
  in.id[litholattice::meshPadIndex(8 + 1, 8 + 1, 8 + 1)] = id;
  if (above != litholattice::kAir)
    in.id[litholattice::meshPadIndex(8 + 1, 9 + 1, 8 + 1)] = above;
  return in;  // id[]/st[] zero-init: kAir = 0, empty state
}

litholattice::MeshContext makeCtx(const litholattice::BlockMaterials& mats) {
  return litholattice::MeshContext{lithokraft::blockRegistry(), mats, &lithokraft::blockAppearance()};
}

size_t meshViewportDirect(litholattice::BlockId above, std::string_view modelName,
                          const litholattice::BlockMaterials& mats) {
  litholattice::MeshInput in = makeInput(lithokraft::kChipORViewport, above);
  const litholattice::BlockRegistry& reg = lithokraft::blockRegistry();
  const litholattice::CustomBlock cell{in, reg, mats, lithokraft::kChipORViewport, 0, 8, 8, 8};
  std::vector<litholattice::PackedVertex> verts;
  litholattice::MeshWriter writer(verts);
  lithokraft::chipor::meshViewportCell(cell, writer, modelName);
  return verts.size();
}
}  // namespace

int main() {
  using namespace lithokraft;
  using namespace litholattice;
  const BlockRegistry& reg = blockRegistry();

  // 1. Registration: ids 51/52, append-only, no collision, no renumbering.
  // Size guard is registry-derived (P2b-2, review G-M3): the registry must
  // extend PAST the viewport id — append-only blocks landed after it
  // (Structure Block, Certificate Table, ...) must not break this check
  // the way the old exact-count guard (kSilicaTilledSoil+1 == 55) did.
  check(reg.size() > size_t(kChipORViewport), "registry size extends past the ChipOR viewport id (append-only)");
  check(reg.def(kChipOR).id == "lithokraft:chipor", "id 51 = lithokraft:chipor");
  check(reg.def(kChipORViewport).id == "lithokraft:chipor_viewport", "id 52 = lithokraft:chipor_viewport");
  check(std::string(reg.def(50).id) == "lithokraft:dead_memory_cell", "id 50 still dead_memory_cell (no renumber)");
  check(roleOf(kChipOR) == SimRole::ChipOR && roleOf(kChipORViewport) == SimRole::ChipOR,
        "both carry SimRole::ChipOR (inert: the sim dispatches no ChipOR case)");
  check(reg.def(kChipOR).shape == Shape::Cube, "plain ChipOR is a solid cube");
  check(reg.def(kChipORViewport).shape == Shape::Custom, "viewport is Shape::Custom (meshCustom route)");
  check((reg.def(kChipORViewport).flags & kOpaque) != 0, "viewport is kOpaque: neighbours cull against the body");

  // 2. Model loading: the checked-in .lthm matches the built-in demo, leaf for leaf.
  const std::vector<subdiv::MicroCell> fromFile = chipor::loadModelCells(chipor::kDefaultModel);
  const std::vector<subdiv::MicroCell> demo = chipor::demoModelCells();
  check(fromFile.size() == 15 && demo.size() == 15, "demo model has 15 leaves");
  bool leavesMatch = fromFile.size() == demo.size();
  for (size_t i = 0; leavesMatch && i < demo.size(); ++i) leavesMatch = sameCell(fromFile[i], demo[i]);
  check(leavesMatch, "chipor-demo.lthm loads leaf-identical to the built-in demo");

  // 3. Missing/corrupt/unsanitary model => clean empty, no crash.
  check(chipor::loadModelCells("definitely-not-a-real-model").empty(), "missing model -> empty");
  check(chipor::loadModelCells("../evil").empty(), "path traversal rejected");
  check(chipor::loadModelCells("").empty(), "empty name -> empty");
  check(chipor::meshMiniatureLOD({}).empty(), "empty leaf stream -> no quads");

  // 4. Miniature mesher: non-empty, deterministic, inside the interior box.
  const std::vector<chipor::MiniQuad> mq1 = chipor::meshMiniatureLOD(demo);
  const std::vector<chipor::MiniQuad> mq2 = chipor::meshMiniatureLOD(demo);
  check(!mq1.empty(), "miniature emits quads");
  bool det = mq1.size() == mq2.size();
  for (size_t i = 0; det && i < mq1.size(); ++i)
    det = mq1[i].face == mq2[i].face && mq1[i].material == mq2[i].material &&
          !std::memcmp(mq1[i].lo, mq2[i].lo, 3) && !std::memcmp(mq1[i].hi, mq2[i].hi, 3);
  check(det, "miniature mesh is deterministic");
  bool inBox = true;
  for (const auto& q : mq1) {
    if (q.face > 5) { inBox = false; break; }
    for (int a = 0; a < 3; ++a)
      if (q.lo[a] < chipor::kInteriorLo[a] || q.hi[a] > chipor::kInteriorHi[a] || q.lo[a] >= q.hi[a]) {
        inBox = false;
        break;
      }
  }
  check(inBox, "all miniature quads inside the interior box");

  // 5. Full mesh path: body (5 faces) + transparent top + interior micro-quads, exact count.
  const BlockMaterials mats(reg.size());
  const MeshContext ctx = makeCtx(mats);
  {
    MeshInput in = makeInput(kChipORViewport, kAir);
    const ChunkMesh m1 = meshSubChunk(in, ctx);
    const ChunkMesh m2 = meshSubChunk(in, ctx);
    const size_t expect = 4 * (5 + 1 + mq1.size());  // 5 body faces + top + one quad per micro-quad
    check(m1.verts.size() == expect, "viewport mesh = body + top + interior micro-quads (exact count)");
    check(m1.verts.size() == m2.verts.size() &&
              !std::memcmp(m1.verts.data(), m2.verts.data(),
                            m1.verts.size() * sizeof(PackedVertex)),
          "viewport mesh is bit-deterministic");
  }

  // 6. Opaque block above: top face culled AND interior skipped (nothing visible).
  check(meshViewportDirect(kWaferSilicon, chipor::kDefaultModel, mats) == 5 * 4,
        "opaque above -> body only, no top, no interior");

  // 7. Missing model at mesh time: body + transparent top, empty viewport, no crash.
  check(meshViewportDirect(kAir, "definitely-not-a-real-model", mats) == 6 * 4,
        "missing model -> body + top, no interior, no crash");

  // 8. Plain ChipOR (51): classic opaque cube, untouched by the viewport path.
  {
    MeshInput in = makeInput(kChipOR, kAir);
    const ChunkMesh m = meshSubChunk(in, ctx);
    check(m.verts.size() == 6 * 4, "plain ChipOR meshes as a 6-face cube");
  }

  std::printf("chipor_selftest: %d failures\n", g_fails);
  return g_fails ? 1 : 0;
}
