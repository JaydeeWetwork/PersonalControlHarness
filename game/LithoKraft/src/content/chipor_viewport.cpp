// ChipOR viewport implementation (workstream 4). See chipor_viewport.h for the
// design and the workstream 1/2/3 seams.
#include "lithokraft/content/chipor_viewport.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include "lithokraft/content/blocks.h"
#include "model.h"  // workstream 3: .lthm loadModel (lithokraft_model)

namespace lithokraft::chipor {
namespace {

using litholattice::subdiv::MicroCell;
using litholattice::subdiv::SparseOctree;

// Interior grid dims, derived from the box in the header.
constexpr int kGX = kInteriorHi[0] - kInteriorLo[0];  // 14
constexpr int kGY = kInteriorHi[1] - kInteriorLo[1];  // 7
constexpr int kGZ = kInteriorHi[2] - kInteriorLo[2];  // 14
constexpr int kGridN = kGX * kGY * kGZ;               // 1372

constexpr int gridIndex(int x, int y, int z) { return (y * kGZ + z) * kGX + x; }

// Quantize a level-6 model span [c0, c1) on one axis into the interior box.
// Floor on lo, ceil on hi: a non-empty model span always covers >= 1 unit.
void quantizeSpan(int c0, int c1, int lo, int dim, int& q0, int& q1) {
  q0 = lo + (c0 * dim) / kModelCells;
  q1 = lo + (c1 * dim + kModelCells - 1) / kModelCells;
  q0 = std::clamp(q0, lo, lo + dim);
  q1 = std::clamp(q1, lo, lo + dim);
}

}  // namespace

std::vector<MicroCell> demoModelCells() {
  // A tiny circuit, authored with the real SparseOctree (workstream 1) in its
  // 64^3 model space (level, x, y, z).
  // Materials: 1 = red trace, 2 = blue trace, 3 = silicon die, 4 = torch post.
  SparseOctree tree;
  auto set = [&](int level, int x, int y, int z, uint8_t material) {
    tree.set(level, x, y, z, material);
  };
  // Silicon die plate: 4 level-2 cells (x 16..47, y 0..15, z 16..47).
  for (int px = 1; px <= 2; ++px)
    for (int pz = 1; pz <= 2; ++pz) set(2, px, 0, pz, 3);
  // Red trace run along +X (x 16..31, y 16..19, z 24..27).
  for (int tx = 4; tx <= 7; ++tx) set(4, tx, 4, 6, 1);
  // Blue trace run along +X (x 32..47, y 16..19, z 36..39).
  for (int tx = 8; tx <= 11; ++tx) set(4, tx, 4, 9, 2);
  // Torch post rising off the red run (x 20..23, y 20..27, z 24..27).
  set(4, 5, 5, 6, 4);
  set(4, 5, 6, 6, 4);
  // Via pillar stub on the blue run (x 40..43, y 20..23, z 36..39).
  set(4, 10, 5, 9, 2);
  return tree.leaves();
}

std::vector<MicroCell> loadModelCells(std::string_view name) {
  // Mutex-guarded cache: the mesher calls this on worker threads and the
  // .lthm load does file I/O, so each distinct model loads exactly once.
  static std::mutex mutex;
  static std::map<std::string, std::vector<MicroCell>, std::less<>> cache;
  const std::string key(name);
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;
  }
  std::vector<MicroCell> cells;
  // Sanitise: names are file stems only (no separators, no parent refs).
  const bool sane = !key.empty() && key.size() <= 128 && key.find('/') == std::string::npos &&
                    key.find('\\') == std::string::npos && key.find("..") == std::string::npos;
  if (sane) {
#ifdef LITHOKRAFT_MODELS_DIR
    const model::LoadResult loaded = model::loadModel(std::string(LITHOKRAFT_MODELS_DIR) + "/" + key + ".lthm");
    if (loaded.error == model::LoadError::Ok) cells = loaded.model.geometry.leaves();
#endif
    // The default always shows something: fall back to the built-in demo
    // when the file is absent (e.g. a shipping build without the source tree).
    if (cells.empty() && key == kDefaultModel) cells = demoModelCells();
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    cache[key] = cells;
  }
  return cells;
}

std::vector<MiniQuad> meshMiniatureLOD(const std::vector<MicroCell>& cells) {
  // Reference implementation (workstream 2 seam): rasterize the leaves into
  // the interior dense grid, then emit unit quads with hidden-face culling.
  // Deterministic: leaves are visited in input order, grid iteration is fixed.
  // thread_local scratch: the mesher runs on worker threads (same pattern as
  // the classic mesher's opaque[] ring).
  thread_local uint8_t occ[kGridN];
  thread_local uint8_t mat[kGridN];
  std::memset(occ, 0, sizeof(occ));
  std::memset(mat, 0, sizeof(mat));
  for (const MicroCell& c : cells) {
    if (c.material == 0 || c.level > litholattice::subdiv::kMaxLevel) continue;
    const int s = kModelCells >> c.level;  // level-6 span of one cell side
    if (s <= 0) continue;
    int qx0, qx1, qy0, qy1, qz0, qz1;
    quantizeSpan(c.x * s, (c.x + 1) * s, kInteriorLo[0], kGX, qx0, qx1);
    quantizeSpan(c.y * s, (c.y + 1) * s, kInteriorLo[1], kGY, qy0, qy1);
    quantizeSpan(c.z * s, (c.z + 1) * s, kInteriorLo[2], kGZ, qz0, qz1);
    for (int y = qy0; y < qy1; ++y)
      for (int z = qz0; z < qz1; ++z)
        for (int x = qx0; x < qx1; ++x) {
          const int gi = gridIndex(x - kInteriorLo[0], y - kInteriorLo[1], z - kInteriorLo[2]);
          occ[gi] = 1;
          mat[gi] = c.material;
        }
  }
  std::vector<MiniQuad> quads;
  quads.reserve(256);
  constexpr int kDX[6] = {1, -1, 0, 0, 0, 0};
  constexpr int kDY[6] = {0, 0, 1, -1, 0, 0};
  constexpr int kDZ[6] = {0, 0, 0, 0, 1, -1};
  for (int y = 0; y < kGY; ++y)
    for (int z = 0; z < kGZ; ++z)
      for (int x = 0; x < kGX; ++x) {
        const int gi = gridIndex(x, y, z);
        if (!occ[gi]) continue;
        for (int f = 0; f < 6; ++f) {
          const int nx = x + kDX[f], ny = y + kDY[f], nz = z + kDZ[f];
          const bool hidden = nx >= 0 && nx < kGX && ny >= 0 && ny < kGY && nz >= 0 && nz < kGZ &&
                              occ[gridIndex(nx, ny, nz)];
          if (hidden) continue;
          MiniQuad q;
          q.face = uint8_t(f);
          q.material = mat[gi];
          q.lo[0] = uint8_t(kInteriorLo[0] + x);
          q.lo[1] = uint8_t(kInteriorLo[1] + y);
          q.lo[2] = uint8_t(kInteriorLo[2] + z);
          q.hi[0] = uint8_t(kInteriorLo[0] + x + 1);
          q.hi[1] = uint8_t(kInteriorLo[1] + y + 1);
          q.hi[2] = uint8_t(kInteriorLo[2] + z + 1);
          quads.push_back(q);
        }
      }
  return quads;
}

namespace {

using litholattice::FaceLook;
using litholattice::FaceMaterial;
using litholattice::FaceSlot;
using litholattice::kOpaque;
using litholattice::kShadeGlass;

// Micro-material -> face material. Prefers dedicated chipor_* textures when the
// art pass adds them; falls back to the game block whose look each micro
// material borrows. Every path is null-safe (BlockMaterials::named returns
// nullptr when absent; face() returns the missing-tile material past the
// table) so a pack without chipor art still meshes.
const FaceMaterial& microFaceMaterial(const litholattice::CustomBlock& cell, uint8_t microMat,
                                      const FaceMaterial& body) {
  const auto* mats = &cell.materials;
  const FaceMaterial* named = nullptr;
  BlockId borrow = kAir;
  switch (microMat) {
    case 1: named = mats->named("chipor_trace_red"); borrow = kRedTrace; break;
    case 2: named = mats->named("chipor_trace_blue"); borrow = kBlueTrace; break;
    case 3: named = mats->named("chipor_plate"); borrow = kWaferSilicon; break;
    case 4: named = mats->named("chipor_post"); borrow = kRedTorch; break;
    default: break;
  }
  if (named) return *named;
  if (borrow != kAir) return mats->face(borrow, FaceSlot::Top, false);
  return body;
}

}  // namespace

void meshViewportCell(const litholattice::CustomBlock& cell, litholattice::MeshWriter& out,
                      std::string_view modelName) {
  const litholattice::BlockRegistry& reg = cell.blocks;
  const auto& mats = cell.materials;
  // Body material: the chipor texture when the art pass ships it, else the
  // block's own (missing-tile) face - always safe.
  const FaceMaterial* bodyNamed = mats.named("chipor");
  const FaceMaterial& body = bodyNamed ? *bodyNamed : mats.face(cell.id, FaceSlot::Front, false);
  static constexpr int kLo[3] = {0, 0, 0};
  static constexpr int kHi[3] = {16, 16, 16};
  const bool opaqueAbove = reg.has(cell.neighbor(0, 1, 0), kOpaque);
  // Opaque body: all faces but +Y, culled against opaque neighbours. (The
  // block itself carries kOpaque so neighbours cull against the body.)
  constexpr int kBodyFace[5] = {0, 1, 3, 4, 5};  // +X, -X, -Y, +Z, -Z
  constexpr int kBodyDx[5] = {1, -1, 0, 0, 0};
  constexpr int kBodyDy[5] = {0, 0, -1, 0, 0};
  constexpr int kBodyDz[5] = {0, 0, 0, 1, -1};
  for (int i = 0; i < 5; ++i) {
    if (reg.has(cell.neighbor(kBodyDx[i], kBodyDy[i], kBodyDz[i]), kOpaque)) continue;
    out.face(cell, kBodyFace[i], kLo, kHi, body, FaceLook{});
  }
  if (opaqueAbove) return;  // top face hidden: nothing inside is visible either
  // Interior miniature: static, render-only, flat shaded (no lighting yet).
  for (const MiniQuad& q : meshMiniatureLOD(loadModelCells(modelName))) {
    const int lo[3] = {q.lo[0], q.lo[1], q.lo[2]};
    const int hi[3] = {q.hi[0], q.hi[1], q.hi[2]};
    out.face(cell, q.face, lo, hi, microFaceMaterial(cell, q.material, body), FaceLook{});
  }
  // Transparent top, last: the existing transparent shading path (kShadeGlass,
  // the same mode the glass blocks use - screen-door alpha, no sorting).
  const FaceMaterial* topNamed = mats.named("chipor_top");
  const FaceMaterial* glassNamed = mats.named("glass");
  const FaceMaterial& top =
      topNamed ? *topNamed : glassNamed ? *glassNamed : mats.face(kGlass, FaceSlot::Top, false);
  out.face(cell, 2, kLo, kHi, top, FaceLook{kShadeGlass, 0});
}

}  // namespace lithokraft::chipor
