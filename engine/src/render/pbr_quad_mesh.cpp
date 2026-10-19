// BinaryQuad -> PbrQuadVertex expansion (PBR phase 2, Worker 3). See pbr_quad_mesh.h.
#include "litholattice/render/pbr_quad_mesh.h"
#include "litholattice/core/log.h"

namespace litholattice {
namespace {

// Winding fix-up per face direction: the natural corner order (min corner, +t0, +t0+t1,
// +t1) is CCW-from-outside for some directions and CW for others (axis handedness).
// true = emit (v0, v3, v2, v1) instead of (v0, v1, v2, v3). Derived in the Worker 3
// report; pinned by the headless winding test (cross((v1-v0),(v2-v0)) . normal > 0).
constexpr bool kReverseWinding[6] = {
    false,  // +X: natural order is already CCW from +X
    true,   // -X
    true,   // +Y
    false,  // -Y
    false,  // +Z
    true,   // -Z
};

// Tangent axes per face axis: X faces -> (Y, Z); Y faces -> (X, Z); Z faces -> (X, Y).
// Matches BinaryQuad's documented convention.
constexpr int kTangent0[3] = {1, 0, 0};
constexpr int kTangent1[3] = {2, 2, 1};

}  // namespace

void expandBinaryQuadsToPbr(const std::vector<BinaryQuad>& quads, const uint16_t* palette,
                            size_t paletteSize, uint8_t blockLight, uint8_t skyLight,
                            std::vector<PbrQuadVertex>& out) {
  size_t n = quads.size();
  if (n > kMaxQuadsPerMesh) {
    logError("pbr-mesh: sub-chunk has %zu quads, trimmed to %u", n, kMaxQuadsPerMesh);
    n = kMaxQuadsPerMesh;
  }
  out.clear();
  out.reserve(n * 4);
  for (size_t qi = 0; qi < n; ++qi) {
    const BinaryQuad& q = quads[qi];
    const int a = q.dir / 2;  // face axis: 0 = X, 1 = Y, 2 = Z
    const int t0 = kTangent0[a], t1 = kTangent1[a];
    // The stored (x, y, z) is already the min corner ON the face plane: for +dir
    // faces the plane sits one voxel past the emitter voxel, for -dir faces on the
    // emitter's min side (same rule as expandBinaryQuad in binary_greedy_mesher.h).
    int base[3] = {q.x, q.y, q.z};
    // Four corners in voxels: min, min+w*t0, min+w*t0+h*t1, min+h*t1.
    int c[4][3];
    for (int k = 0; k < 4; ++k) {
      c[k][0] = base[0];
      c[k][1] = base[1];
      c[k][2] = base[2];
    }
    c[1][t0] += q.w;
    c[2][t0] += q.w;
    c[2][t1] += q.h;
    c[3][t1] += q.h;
    const uint32_t mat =
        (palette && q.material < paletteSize) ? palette[q.material] : 0u;
    const int order[4] = {0, 1, 2, 3};
    const int rev[4] = {0, 3, 2, 1};
    const int* idx = kReverseWinding[q.dir] ? rev : order;
    for (int k = 0; k < 4; ++k) {
      PbrQuadVertex v;
      v.x = uint16_t(c[idx[k]][0] * 16);
      v.y = uint16_t(c[idx[k]][1] * 16);
      v.z = uint16_t(c[idx[k]][2] * 16);
      v.face = q.dir;
      v.material = mat;
      v.blockLight = blockLight;
      v.skyLight = skyLight;
      out.push_back(v);
    }
  }
}

}  // namespace litholattice
