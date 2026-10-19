#include "litholattice/render/mesher.h"
#include "litholattice/render/block_materials.h"
#include "litholattice/render/block_appearance.h"
#include "litholattice/core/log.h"

namespace litholattice {
namespace {

constexpr int P = kMeshPad;
inline int pidx(int x, int y, int z) { return meshPadIndex(x, y, z); }

struct Face { int n[3], u[3], v[3]; };
// u x v == n, so corners (0,0),(1,0),(1,1),(0,1) wind CCW seen from outside.
constexpr Face kFaces[6] = {
  {{ 1, 0, 0}, {0, 1, 0}, {0, 0, 1}},  // +X (east)
  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},  // -X (west)
  {{ 0, 1, 0}, {0, 0, 1}, {1, 0, 0}},  // +Y
  {{ 0,-1, 0}, {1, 0, 0}, {0, 0, 1}},  // -Y
  {{ 0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  // +Z (south)
  {{ 0, 0,-1}, {0, 1, 0}, {1, 0, 0}},  // -Z (north)
};
constexpr int kCorner[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
constexpr int kFaceDir[6] = {1, 3, -1, -1, 2, 0};  // world face -> horizontal dir (N0 E1 S2 W3)

struct Look {
  uint8_t layer = 0, value = 0, mode = kShadeTex, rot = 0;
  UvRect uv;
  uint32_t slot = 0;  ///< Step 1: strength-buffer slot (trace faces)
};

Look lookOf(const FaceMaterial& m, uint8_t mode, uint8_t value = 0, uint8_t rot = 0) {
  return Look{m.layer, value, mode, rot, m.uv};
}

// Maps a whole-tile coordinate (0..16) into the face's UV rect.
uint8_t inRect(int t, uint8_t lo, uint8_t hi) { return uint8_t(lo + t * (hi - lo) / 16); }

// Texture UV (1/16 units) for a corner at block-local p (0..16) on face f, rotated r quarter turns.
void faceUV(int f, const int p[3], int r, int& u, int& v) {
  switch (f) {
    case 0: u = 16 - p[2]; v = 16 - p[1]; break;
    case 1: u = p[2]; v = 16 - p[1]; break;
    case 4: u = p[0]; v = 16 - p[1]; break;
    case 5: u = 16 - p[0]; v = 16 - p[1]; break;
    case 2: u = p[0]; v = p[2]; break;
    default: u = p[0]; v = 16 - p[2]; break;
  }
  for (int i = 0; i < (r & 3); ++i) { const int t = u; u = v; v = 16 - t; }
}

void emitFace(std::vector<PackedVertex>& out, int f, const int mn[3], const int mx[3], const int blk[3],
              const int ao[4], const Look& lk) {
  const Face& F = kFaces[f];
  PackedVertex q[4];
  for (int c = 0; c < 4; ++c) {
    int pos[3], local[3];
    for (int a = 0; a < 3; ++a) {
      if (F.n[a]) pos[a] = F.n[a] > 0 ? mx[a] : mn[a];
      else if (F.u[a]) pos[a] = kCorner[c][0] ? mx[a] : mn[a];
      else pos[a] = kCorner[c][1] ? mx[a] : mn[a];
      local[a] = pos[a] - blk[a];
    }
    int u, v; faceUV(f, local, lk.rot, u, v);
    q[c] = {uint16_t(pos[0]), uint16_t(pos[1]), uint16_t(pos[2]), inRect(u, lk.uv.u0, lk.uv.u1), inRect(v, lk.uv.v0, lk.uv.v1),
            uint8_t(f | (ao[c] << 3) | (lk.rot << 5)), lk.layer, lk.value, lk.mode, lk.slot};
  }
  const int start = (ao[0] + ao[2] < ao[1] + ao[3]) ? 1 : 0;  // flip diagonal for AO
  for (int c = 0; c < 4; ++c) out.push_back(q[(start + c) & 3]);
}

// Cross sprite (Shape::Cross): two crossed vertical quads, both sides.
void emitCross(std::vector<PackedVertex>& out, const int b[3], const Look& lk) {
  const int d[2][4] = {{0, 0, 16, 16}, {0, 16, 16, 0}};  // x0 z0 x1 z1
  for (auto& q : d)
    for (int side = 0; side < 2; ++side) {
      PackedVertex v[4];
      const int xs[4] = {q[0], q[2], q[2], q[0]}, zs[4] = {q[1], q[3], q[3], q[1]}, ys[4] = {0, 0, 16, 16};
      for (int c = 0; c < 4; ++c)
        v[c] = {uint16_t(b[0] + xs[c]), uint16_t(b[1] + ys[c]), uint16_t(b[2] + zs[c]), uint8_t(c == 1 || c == 2 ? 16 : 0),
                uint8_t(c >= 2 ? 0 : 16), uint8_t(2 | (3 << 3)), lk.layer, lk.value, lk.mode, lk.slot};
      if (side) { out.push_back(v[0]); out.push_back(v[1]); out.push_back(v[2]); out.push_back(v[3]); }
      else { out.push_back(v[3]); out.push_back(v[2]); out.push_back(v[1]); out.push_back(v[0]); }
    }
}

const IBlockAppearance kPlainAppearance{};  // defaults only: no custom shapes, plain faces

}  // namespace

void MeshWriter::face(const CustomBlock& cell, int f, const int lo[3], const int hi[3], const FaceMaterial& material,
                      FaceLook look, int rot) {
  static const int full[4] = {3, 3, 3, 3};
  const int blk[3] = {cell.x * 16, cell.y * 16, cell.z * 16};
  const int mn[3] = {blk[0] + lo[0], blk[1] + lo[1], blk[2] + lo[2]}, mx[3] = {blk[0] + hi[0], blk[1] + hi[1], blk[2] + hi[2]};
  emitFace(out_, f, mn, mx, blk, full, lookOf(material, look.mode, look.value, uint8_t(rot)));
}

namespace {

// One meshing pass over the snapshot. meshSubChunk (below) wraps this with
// the definitions-epoch guard.
ChunkMesh meshSubChunkPass(const MeshInput& in, const MeshContext& ctx) {
  ChunkMesh out; out.cx = in.cx; out.sy = in.sy; out.cz = in.cz; out.epoch = in.epoch; out.seq = in.seq;
  const BlockRegistry& reg = ctx.blocks;
  const BlockMaterials& mats = ctx.materials;
  const IBlockAppearance& look = ctx.appearance ? *ctx.appearance : kPlainAppearance;
  MeshWriter writer(out.verts);
  static thread_local uint8_t opaque[kMeshPadVolume];
  for (int i = 0; i < kMeshPadVolume; ++i) opaque[i] = uint8_t(reg.has(in.id[i], kOpaque));
  static const int full[4] = {3, 3, 3, 3};

  out.verts.reserve(4096);
  for (int y = 0; y < 16; ++y)
    for (int z = 0; z < 16; ++z)
      for (int x = 0; x < 16; ++x) {
        const int ci = pidx(x + 1, y + 1, z + 1);
        const BlockId b = in.id[ci];
        if (b == kAir) continue;
        const CellState st = in.st[ci];
        const int mn[3] = {x * 16, y * 16, z * 16};
        const BlockDef def = reg.def(b);
        if ((def.flags & kAttachable) && stAttach(st)) {
          if (const BoxModel* bm = look.boxModel(def)) {  // attached: the oriented box model
            const CustomBlock cell{in, reg, mats, b, st, x, y, z};
            const bool on = look.active(def, st);
            for (const BoxModel::Box& box : bm->orientedBoxes(stAttach(st))) {
              const FaceMaterial& fm = mats.face(b, bm->materials[size_t(box.material)].slot, on);
              FaceLook fl = look.faceLookAt(def, st, FaceSlot::Top, on, in.cx * 16 + x, in.sy * 16 + y,
                                             in.cz * 16 + z);
              if ((def.flags & kTransparent) && fl.mode == kShadeTex) fl.mode = kShadeCutout;  // sprite art: alpha-test
              for (int f = 0; f < 6; ++f) writer.face(cell, f, box.min, box.max, fm, fl);
            }
            continue;
          }
        }
        if (def.shape == Shape::Custom) {  // the game builds it (e.g. connected strips)
          look.meshCustom(CustomBlock{in, reg, mats, b, st, x, y, z}, writer);
          continue;
        }
        if (def.shape == Shape::Cross) {  // sprite: the active-state top face, alpha-tested
          emitCross(out.verts, mn, lookOf(mats.face(b, FaceSlot::Top, look.active(def, st)), kShadeCutout));
          continue;
        }
        if (def.shape != Shape::Cube && def.shape != Shape::Slab) continue;
        const bool slab = def.shape == Shape::Slab;
        const int mx[3] = {mn[0] + 16, mn[1] + (slab ? 8 : 16), mn[2] + 16};
        const bool dir = def.flags & kDirectional;
        const int facing = dir ? stFacing(st) : 0;
        const bool active = look.active(def, st);
        const CustomBlock cellC{in, reg, mats, b, st, x, y, z};
        for (int f = 0; f < 6; ++f) {
          const Face& F = kFaces[f];
          const int fx = x + 1 + F.n[0], fy = y + 1 + F.n[1], fz = z + 1 + F.n[2];
          if (opaque[pidx(fx, fy, fz)] && !(slab && f == 2)) continue;  // hidden by neighbour
          if ((def.flags & kTransparent) && in.id[pidx(fx, fy, fz)] == b) continue;  // glass against glass
          FaceSlot slot;
          if (f == 2) slot = FaceSlot::Top;
          else if (f == 3) slot = FaceSlot::Bottom;
          else {
            const int d = kFaceDir[f];
            slot = d == facing ? FaceSlot::Front : d == dirBack(facing) ? FaceSlot::Back : d == dirRight(facing) ? FaceSlot::Right : FaceSlot::Left;
          }
          const std::optional<FaceMaterial> over = look.faceMaterial(cellC, slot, active);
          const FaceMaterial fm = over ? *over : mats.face(b, slot, active);
          Look lk = lookOf(fm, kShadeTex, 0, uint8_t(f >= 2 && f <= 3 ? facing : 0));
          if (fm.animated()) { lk.mode = kShadeAnimated; lk.value = fm.animation; }
          else {
            const FaceLook fl = look.faceLookAt(def, st, slot, active, in.cx * 16 + x, in.sy * 16 + y,
                                                   in.cz * 16 + z);
            lk.mode = fl.mode;
            // Step 1 (partial): trace faces write the strength-buffer slot for future
            // GPU use, but keep the baked strength in value until the shader
            // actually reads the buffer. Setting value=0 here renders black.
            if (ctx.strengths && (fl.mode == kShadeStripTint0 || fl.mode == kShadeStripTint1)) {
              // B-M4 (P3-2): the lookup key is the WORLD packPos key — the
              // same key the signal sim wrote the slot with. The old key
              // packed sub-chunk-LOCAL (x, y, z), so it could only miss or
              // collide with an unrelated cell. slotOf returns kNoSlot
              // (not 0) when the cell has no slot yet; the future shader
              // treats kNoSlot as "use the baked value".
              const uint64_t key = uint64_t(packPos(in.cx * 16 + x, in.sy * 16 + y, in.cz * 16 + z));
              lk.value = fl.value;  // baked strength (visible now)
              lk.slot = ctx.strengths->slotOf(key);  // buffer slot (for future shader)
            } else {
              lk.value = fl.value;
            }
          }
          int ao[4];
          for (int c = 0; c < 4; ++c) {
            const int su = kCorner[c][0] ? 1 : -1, sv = kCorner[c][1] ? 1 : -1;
            auto at = [&](int a, int bb) {
              return opaque[pidx(fx + a * F.u[0] + bb * F.v[0], fy + a * F.u[1] + bb * F.v[1],
                                 fz + a * F.u[2] + bb * F.v[2])];
            };
            const int s1 = at(su, 0), s2 = at(0, sv), cr = at(su, sv);
            ao[c] = (s1 && s2) ? 0 : 3 - (s1 + s2 + cr);
          }
          emitFace(out.verts, f, mn, mx, mn, slab && f == 2 ? full : ao, lk);
        }
        look.meshExtra(cellC, writer);
      }
  if (out.verts.size() > size_t(kMaxQuadsPerMesh) * 4) {  // cannot happen for 16^3 cubes; guards custom shapes
    logError("mesher: sub-chunk (%d,%d,%d) has %zu quads, trimmed to %u", in.cx, in.sy, in.cz, out.verts.size() / 4,
             kMaxQuadsPerMesh);
    out.verts.resize(size_t(kMaxQuadsPerMesh) * 4);
  }
  // Wafer ground pulse: the column's weight-blended params ride the mesh, so the
  // backends can push them per draw call (biome weights are worldgen-static, so
  // baking at mesh time costs nothing per frame). Null provider = zero params.
  if (ctx.pulseParams) ctx.pulseParams(in.cx, in.cz, out.pulseP0, out.pulseP1);
  // Wafer ground traces (JD 2026-10-11): same pattern as the pulse - the
  // column's weight-blended trace params ride the mesh for per-draw push.
  // Null provider = zero params (no traces).
  if (ctx.traceParams) ctx.traceParams(in.cx, in.cz, out.traceP0, out.traceP1);
  return out;
}

}  // namespace

ChunkMesh meshSubChunk(const MeshInput& in, const MeshContext& ctx) {
  // Definitions-epoch guard (P3-3, Appendix D M4): the voxel data is an
  // immutable snapshot (MeshInput), but block DEFINITIONS are read live
  // from the registry on worker threads, and refreshStaged() can swap them
  // between ticks. Individual def reads are already torn-safe (the
  // registry's shared lock + by-value def()), but a refresh landing
  // MID-MESH would bake a mix of old and new defs into one mesh. Capture
  // the registry's definitionEpoch before and after the pass; on a change,
  // re-mesh ONCE against the now-stable set (refresh happens between
  // ticks, so the second pass sees one consistent generation). A second
  // change means refresh is running concurrently with meshing — outside
  // the between-ticks contract — so log loudly and keep the second pass
  // rather than loop forever.
  const uint64_t epoch0 = ctx.blocks.content().definitionEpoch();
  ChunkMesh out = meshSubChunkPass(in, ctx);
  const uint64_t epoch1 = ctx.blocks.content().definitionEpoch();
  if (epoch1 != epoch0) {
    out = meshSubChunkPass(in, ctx);
    const uint64_t epoch2 = ctx.blocks.content().definitionEpoch();
    if (epoch2 != epoch1)
      logError("mesher: block definitions changed twice during one mesh of (%d,%d,%d) "
               "(epochs %llu -> %llu -> %llu); refreshStaged must run between ticks only",
               in.cx, in.sy, in.cz, (unsigned long long)epoch0, (unsigned long long)epoch1,
               (unsigned long long)epoch2);
  }
  return out;
}

}  // namespace litholattice
