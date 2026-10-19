// Procedural rig generation: voxel model -> Rig.
//
// See riggen.h for the pipeline overview. All passes are deterministic:
// fixed x/y/z scan orders, sorted neighbor visits, and tie-breaking by voxel
// index everywhere. No hash-map iteration, no RNG.
//
// Extraction strategy:
//   1. Chamfer distance transform (medialness weight).
//   2. Weighted PCA (weight = distance value) -> spine axis (longest medial path).
//   3. Slice along the axis -> 1-4 spine bones; top segment -> head bone.
//   4. Voxels far from the axis cluster into limbs (26-connectivity); each limb
//      gets 2-3 bones along its own PCA axis.
//   5. Skinning: per-bone Dijkstra flood-fill through the volume (K-best).

#include "litholattice/anim/riggen.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <queue>
#include <string>
#include <tuple>
#include <vector>

namespace litholattice::anim {
namespace {

// ---- local Vec3 helpers (math.h has no operator-) ----
inline Vec3 vsub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 vadd(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 vmul(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float vlen(Vec3 a) { return std::sqrt(dot(a, a)); }

/// Limbs thinner than this (in voxels) stay rigid: no bone is minted for them.
constexpr int kMinLimbVoxels = 8;

/// A voxel counts as "limb" if farther than this from the spine axis.
constexpr float kLimbDistVox = 2.0f;

/// Paired-call state: extractRig() retains the grid + skinning field so
/// computeSkinning() can flood-fill. Thread-local.
struct ExtractionCache {
  VoxelGrid grid;
  struct Slot { uint8_t bone = 0; float dist = 0.f; };
  std::vector<std::vector<Slot>> voxelSkin;  // by flat voxel idx; empty = unfilled
  bool armed = false;
};
thread_local ExtractionCache g_cache;

// ---- voxel grid helpers ----
inline int flat(int x, int y, int z, int s) { return (z * s + y) * s + x; }
inline bool inGrid(int x, int y, int z, int s) {
  return x >= 0 && y >= 0 && z >= 0 && x < s && y < s && z < s;
}

struct Off { int x, y, z, cost; };
std::vector<Off> neighbor26() {
  std::vector<Off> o;
  for (int dz = -1; dz <= 1; ++dz)
    for (int dy = -1; dy <= 1; ++dy)
      for (int dx = -1; dx <= 1; ++dx) {
        if (!dx && !dy && !dz) continue;
        const int m = (dx != 0) + (dy != 0) + (dz != 0);
        o.push_back({dx, dy, dz, m == 1 ? 10 : (m == 2 ? 14 : 17)});
      }
  return o;
}

struct Grid {
  int s = 0;
  std::vector<uint8_t> occ;
  std::vector<int> dist;  // chamfer * 10 to nearest empty; 0 = empty
  int idx(int x, int y, int z) const { return flat(x, y, z, s); }
};

Vec3 voxelCenter(int flatIdx, int s) {
  const int x = flatIdx % s, y = (flatIdx / s) % s, z = flatIdx / (s * s);
  return {float(x) + 0.5f, float(y) + 0.5f, float(z) + 0.5f};
}

void distanceTransform(Grid& g) {
  const int n = g.s * g.s * g.s;
  const int INF = std::numeric_limits<int>::max() / 4;
  g.dist.assign(size_t(n), 0);
  for (int i = 0; i < n; ++i)
    if (g.occ[size_t(i)]) g.dist[size_t(i)] = INF;
  const auto nb = neighbor26();
  for (int z = 0; z < g.s; ++z)
    for (int y = 0; y < g.s; ++y)
      for (int x = 0; x < g.s; ++x) {
        const int i = g.idx(x, y, z);
        if (!g.occ[size_t(i)]) continue;
        int best = g.dist[size_t(i)];
        for (const auto& o : nb) {
          if (o.z > 0 || (o.z == 0 && (o.y > 0 || (o.y == 0 && o.x > 0)))) continue;
          const int nx = x + o.x, ny = y + o.y, nz = z + o.z;
          if (!inGrid(nx, ny, nz, g.s)) continue;
          const int nd = g.dist[size_t(g.idx(nx, ny, nz))];
          if (nd + o.cost < best) best = nd + o.cost;
        }
        g.dist[size_t(i)] = best;
      }
  for (int z = g.s - 1; z >= 0; --z)
    for (int y = g.s - 1; y >= 0; --y)
      for (int x = g.s - 1; x >= 0; --x) {
        const int i = g.idx(x, y, z);
        if (!g.occ[size_t(i)]) continue;
        int best = g.dist[size_t(i)];
        for (const auto& o : nb) {
          if (o.z < 0 || (o.z == 0 && (o.y < 0 || (o.y == 0 && o.x < 0)))) continue;
          const int nx = x + o.x, ny = y + o.y, nz = z + o.z;
          if (!inGrid(nx, ny, nz, g.s)) continue;
          const int nd = g.dist[size_t(g.idx(nx, ny, nz))];
          if (nd + o.cost < best) best = nd + o.cost;
        }
        g.dist[size_t(i)] = best;
      }
}

// 3x3 symmetric eigendecomposition via Jacobi rotations. Returns eigenvectors
// (columns) sorted by descending eigenvalue. Deterministic.
void jacobiEigen(float a[3][3], Vec3& e1, Vec3& e2, Vec3& e3) {
  float v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int sweep = 0; sweep < 24; ++sweep) {
    // find largest off-diagonal
    int p = 0, q = 1;
    float mx = std::abs(a[0][1]);
    if (std::abs(a[0][2]) > mx) { mx = std::abs(a[0][2]); p = 0; q = 2; }
    if (std::abs(a[1][2]) > mx) { mx = std::abs(a[1][2]); p = 1; q = 2; }
    if (mx < 1e-9f) break;
    const float app = a[p][p], aqq = a[q][q], apq = a[p][q];
    const float phi = 0.5f * std::atan2(2.f * apq, aqq - app);
    const float c = std::cos(phi), s = std::sin(phi);
    for (int k = 0; k < 3; ++k) {
      const float akp = a[k][p], akq = a[k][q];
      a[k][p] = c * akp - s * akq;
      a[k][q] = s * akp + c * akq;
    }
    for (int k = 0; k < 3; ++k) {
      const float apk = a[p][k], aqk = a[q][k];
      a[p][k] = c * apk - s * aqk;
      a[q][k] = s * apk + c * aqk;
    }
    for (int k = 0; k < 3; ++k) {
      const float vkp = v[k][p], vkq = v[k][q];
      v[k][p] = c * vkp - s * vkq;
      v[k][q] = s * vkp + c * vkq;
    }
  }
  // sort by descending eigenvalue; tie-break by axis order for determinism
  int ord[3] = {0, 1, 2};
  std::sort(ord, ord + 3, [&](int i, int j) {
    if (a[i][i] != a[j][j]) return a[i][i] > a[j][j];
    return i < j;
  });
  e1 = {v[0][ord[0]], v[1][ord[0]], v[2][ord[0]]};
  e2 = {v[0][ord[1]], v[1][ord[1]], v[2][ord[1]]};
  e3 = {v[0][ord[2]], v[1][ord[2]], v[2][ord[2]]};
  // canonical sign: largest-abs component positive
  for (Vec3* e : {&e1, &e2, &e3}) {
    float ax = std::abs(e->x), ay = std::abs(e->y), az = std::abs(e->z);
    float m = ax; int mi = 0;
    if (ay > m) { m = ay; mi = 1; }
    if (az > m) { m = az; mi = 2; }
    const float sgn = (mi == 0 ? e->x : (mi == 1 ? e->y : e->z)) >= 0 ? 1.f : -1.f;
    e->x *= sgn; e->y *= sgn; e->z *= sgn;
  }
}

struct PcaResult {
  Vec3 mean;
  Vec3 axis;  // first principal component (unit)
};

// Weighted PCA of voxel centers. Deterministic.
PcaResult weightedPca(const std::vector<int>& voxels, const std::vector<float>& weights, int s) {
  PcaResult r;
  double sw = 0, mx = 0, my = 0, mz = 0;
  for (size_t i = 0; i < voxels.size(); ++i) {
    const Vec3 p = voxelCenter(voxels[i], s);
    const double w = weights[i];
    sw += w; mx += w * p.x; my += w * p.y; mz += w * p.z;
  }
  r.mean = {float(mx / sw), float(my / sw), float(mz / sw)};
  float cov[3][3] = {};
  for (size_t i = 0; i < voxels.size(); ++i) {
    const Vec3 p = voxelCenter(voxels[i], s);
    const float w = weights[i];
    const float dx = p.x - r.mean.x, dy = p.y - r.mean.y, dz = p.z - r.mean.z;
    cov[0][0] += w * dx * dx; cov[0][1] += w * dx * dy; cov[0][2] += w * dx * dz;
    cov[1][1] += w * dy * dy; cov[1][2] += w * dy * dz;
    cov[2][2] += w * dz * dz;
  }
  cov[1][0] = cov[0][1]; cov[2][0] = cov[0][2]; cov[2][1] = cov[1][2];
  Vec3 e2, e3;
  jacobiEigen(cov, r.axis, e2, e3);
  return r;
}

}  // namespace

std::optional<Rig> extractRig(const VoxelGrid& grid) {
  g_cache = ExtractionCache{};
  const int s = grid.size;
  if (s < 1 || s > 64 || grid.cells.empty()) return std::nullopt;
  Grid g;
  g.s = s;
  const int n = s * s * s;
  g.occ.assign(size_t(n), 0);
  for (const auto& c : grid.cells) {
    if (!inGrid(c.x, c.y, c.z, s)) return std::nullopt;
    g.occ[size_t(g.idx(c.x, c.y, c.z))] = 1;
  }
  distanceTransform(g);

  // Filled voxel list (scan order) + dist weights.
  std::vector<int> voxels;
  std::vector<float> weights;
  for (int i = 0; i < n; ++i)
    if (g.occ[size_t(i)]) {
      voxels.push_back(i);
      weights.push_back(float(g.dist[size_t(i)]) / 10.f + 0.5f);
    }
  if (voxels.empty()) return std::nullopt;

  // Spine axis: weighted PCA (medial voxels pull the axis to the core).
  PcaResult pca = weightedPca(voxels, weights, s);
  Vec3 axis = pca.axis;
  // Prefer "up" = +Y when the axis is mostly vertical (deterministic facing).
  if (std::abs(axis.y) >= std::abs(axis.x) && std::abs(axis.y) >= std::abs(axis.z) && axis.y < 0)
    axis = vmul(axis, -1.f);

  // Project voxels onto the axis; find extent.
  float tMin = std::numeric_limits<float>::max(), tMax = -std::numeric_limits<float>::max();
  std::vector<float> proj(voxels.size());
  for (size_t i = 0; i < voxels.size(); ++i) {
    const float t = dot(vsub(voxelCenter(voxels[i], s), pca.mean), axis);
    proj[i] = t;
    if (t < tMin) tMin = t;
    if (t > tMax) tMax = t;
  }
  const float axisLen = tMax - tMin;
  if (axisLen < 1e-6f) {
    // Degenerate (single voxel): one bone.
    Rig rig;
    Bone b;
    b.name = "spine";
    b.parent = -1;
    b.head = b.tail = voxelCenter(voxels[0], s);
    rig.bones.push_back(b);
    if (!rig.valid()) return std::nullopt;
    // skinning: single bone
    g_cache.voxelSkin.assign(size_t(n), {});
    for (int v : voxels) g_cache.voxelSkin[size_t(v)].push_back({0, 0.f});
    g_cache.grid = grid;
    g_cache.armed = true;
    return rig;
  }

  // Distance of each voxel from the spine axis (line through mean).
  auto distToAxis = [&](int vi) {
    const Vec3 p = vsub(voxelCenter(voxels[size_t(vi)], s), pca.mean);
    const float t = dot(p, axis);
    return vlen(vsub(p, vmul(axis, t)));
  };

  // Limb voxels: far from axis, OR below/above the torso y-range (legs). The
  // torso y-range is the contiguous interval with the widest cross-section;
  // this keeps legs (below torso) from merging with arms (beside torso).
  struct Limb { std::vector<int> voxels; };  // indices into `voxels`
  std::vector<Limb> limbs;
  {
    // Torso t-interval: histogram along axis, peak = max count, expand while
    // count > 50% of peak.
    float tTorsoLo = tMin, tTorsoHi = tMax;
    {
      const int nbins = int(axisLen) + 1;
      std::vector<int> hist(size_t(nbins), 0);
      for (float t : proj) {
        int b = int(t - tMin);
        if (b < 0) b = 0;
        if (b >= nbins) b = nbins - 1;
        ++hist[size_t(b)];
      }
      int peak = 0;
      for (int i = 1; i < nbins; ++i)
        if (hist[size_t(i)] > hist[size_t(peak)]) peak = i;
      const int thresh = hist[size_t(peak)] / 2;
      int lo = peak, hi = peak;
      while (lo > 0 && hist[size_t(lo - 1)] > thresh) --lo;
      while (hi < nbins - 1 && hist[size_t(hi + 1)] > thresh) ++hi;
      tTorsoLo = tMin + float(lo);
      tTorsoHi = tMin + float(hi + 1);
    }
    std::vector<char> isLimb(voxels.size(), 0);
    std::vector<int> lut(size_t(n), -1);
    for (size_t i = 0; i < voxels.size(); ++i) lut[size_t(voxels[i])] = int(i);
    for (size_t i = 0; i < voxels.size(); ++i) {
      const float d = distToAxis(int(i));
      const float t = proj[i];
      const bool belowTorso = t < tTorsoLo - 1.0f;
      const bool aboveTorso = t > tTorsoHi + 1.0f;
      const bool inTorso = !belowTorso && !aboveTorso;
      // Below/above torso: modest distance suffices (legs, tail).
      // Within torso y-range: require larger distance (arms, not torso edge).
      if (belowTorso || aboveTorso) {
        if (d > kLimbDistVox) isLimb[i] = 1;
      } else if (inTorso) {
        if (d > kLimbDistVox * 2.0f) isLimb[i] = 1;
      }
    }

    std::vector<char> seen(voxels.size(), 0);
    const auto nb = neighbor26();
    for (size_t i = 0; i < voxels.size(); ++i) {
      if (!isLimb[i] || seen[i]) continue;
      Limb limb;
      std::queue<int> q;
      q.push(int(i));
      seen[i] = 1;
      while (!q.empty()) {
        const int u = q.front(); q.pop();
        limb.voxels.push_back(u);
        const int vu = voxels[size_t(u)];
        const int x = vu % s, y = (vu / s) % s, z = vu / (s * s);
        for (const auto& o : nb) {
          const int nx = x + o.x, ny = y + o.y, nz = z + o.z;
          if (!inGrid(nx, ny, nz, s)) continue;
          const int w = lut[size_t(flat(nx, ny, nz, s))];
          if (w < 0 || !isLimb[size_t(w)] || seen[size_t(w)]) continue;
          seen[size_t(w)] = 1;
          q.push(w);
        }
      }
      if (int(limb.voxels.size()) >= kMinLimbVoxels) limbs.push_back(std::move(limb));
    }
    // Trim: remove voxels inside the torso bounding box (prevents leg bones
    // from jutting into the chest). The torso box is the AABB of core voxels
    // (distToAxis < kLimbDistVox), expanded by 1 voxel.
    float bbMin[3] = {1e9f, 1e9f, 1e9f}, bbMax[3] = {-1e9f, -1e9f, -1e9f};
    for (size_t i = 0; i < voxels.size(); ++i) {
      if (distToAxis(int(i)) >= kLimbDistVox) continue;
      const Vec3 p = voxelCenter(voxels[i], s);
      bbMin[0] = std::min(bbMin[0], p.x); bbMax[0] = std::max(bbMax[0], p.x);
      bbMin[1] = std::min(bbMin[1], p.y); bbMax[1] = std::max(bbMax[1], p.y);
      bbMin[2] = std::min(bbMin[2], p.z); bbMax[2] = std::max(bbMax[2], p.z);
    }
    for (auto& l : limbs) {
      std::vector<int> kept;
      for (int vi : l.voxels) {
        const Vec3 p = voxelCenter(voxels[size_t(vi)], s);
        const bool inside = p.x >= bbMin[0] - 1.f && p.x <= bbMax[0] + 1.f &&
                            p.y >= bbMin[1] - 1.f && p.y <= bbMax[1] + 1.f &&
                            p.z >= bbMin[2] - 1.f && p.z <= bbMax[2] + 1.f;
        if (!inside) kept.push_back(vi);
      }
      if (int(kept.size()) >= kMinLimbVoxels) l.voxels.swap(kept);
    }
    // Drop limbs that became too small.
    {
      std::vector<Limb> kept;
      for (auto& l : limbs)
        if (int(l.voxels.size()) >= kMinLimbVoxels) kept.push_back(std::move(l));
      limbs.swap(kept);
    }
    // Deterministic order: sort by tip (farthest voxel from axis, then coords).
    std::sort(limbs.begin(), limbs.end(), [&](const Limb& a, const Limb& b) {
      auto tipKey = [&](const Limb& l) {
        int best = l.voxels[0];
        float bd = -1;
        for (int vi : l.voxels) {
          const float d = distToAxis(vi);
          if (d > bd) { bd = d; best = vi; }
        }
        const Vec3 p = voxelCenter(voxels[size_t(best)], s);
        return std::make_tuple(bd, p.x, p.y, p.z);
      };
      return tipKey(a) < tipKey(b);
    });
  }

  // ---- bone placement ----
  Rig rig;
  auto addBone = [&](std::string name, int parent, Vec3 head, Vec3 tail) -> int {
    Bone b;
    b.name = std::move(name);
    b.parent = parent;
    b.head = head;
    b.tail = tail;
    rig.bones.push_back(std::move(b));
    return int(rig.bones.size()) - 1;
  };
  auto axisPoint = [&](float t) { return vadd(pca.mean, vmul(axis, t)); };

  // Head detection: voxels near the top of the axis that are NOT in any limb.
  // If the top cap (t > tMax - headLen) has enough voxels, carve a head bone.
  std::vector<char> inLimb(voxels.size(), 0);
  for (const auto& l : limbs)
    for (int vi : l.voxels) inLimb[size_t(vi)] = 1;
  bool hasHead = false;
  float headT = tMax;
  {
    // Find the highest limb attachment (max t among limb voxels).
    float maxLimbT = -std::numeric_limits<float>::max();
    for (const auto& l : limbs)
      for (int vi : l.voxels) maxLimbT = std::max(maxLimbT, proj[size_t(vi)]);
    // Count non-limb voxels above maxLimbT.
    int topCount = 0;
    for (size_t i = 0; i < voxels.size(); ++i)
      if (!inLimb[i] && proj[i] > maxLimbT) ++topCount;
    if (topCount >= 12 && tMax - maxLimbT >= 2.5f) {
      hasHead = true;
      headT = maxLimbT;
    }
  }

  // Spine bones along [tMin, headT or tMax].
  const float spineTop = hasHead ? headT : tMax;
  const float spineLen = spineTop - tMin;
  int nSpine = 1;
  if (spineLen >= 14) nSpine = 4;
  else if (spineLen >= 9) nSpine = 3;
  else if (spineLen >= 4) nSpine = 2;
  std::vector<int> spineBones;
  {
    int parent = -1;
    for (int i = 0; i < nSpine; ++i) {
      const float t0 = tMin + spineLen * float(i) / float(nSpine);
      const float t1 = tMin + spineLen * float(i + 1) / float(nSpine);
      const std::string nm = (i == 0) ? "spine" : ("spine_" + std::to_string(i));
      const int bi = addBone(nm, parent, axisPoint(t0), axisPoint(t1));
      spineBones.push_back(bi);
      parent = bi;
    }
    if (hasHead) {
      addBone("head", spineBones.back(), axisPoint(headT), axisPoint(tMax));
    }
  }

  // Limb bones: each limb gets 2-3 bones along its own PCA axis.
  // Enforce the bone cap by dropping smallest limbs first.
  {
    struct LimbPlan { size_t idx; int bones; };
    std::vector<LimbPlan> plans;
    for (size_t i = 0; i < limbs.size(); ++i) {
      // limb length along its PCA axis
      std::vector<float> w(limbs[i].voxels.size(), 1.f);
      std::vector<int> vv;
      for (int vi : limbs[i].voxels) vv.push_back(voxels[size_t(vi)]);
      PcaResult lp = weightedPca(vv, w, s);
      float lmin = 1e9f, lmax = -1e9f;
      for (int v : vv) {
        const float t = dot(vsub(voxelCenter(v, s), lp.mean), lp.axis);
        lmin = std::min(lmin, t); lmax = std::max(lmax, t);
      }
      plans.push_back({i, (lmax - lmin) >= 7.f ? 3 : 2});
    }
    int est = int(rig.bones.size());
    for (auto& p : plans) est += p.bones;
    if (est > kMaxBones) {
      std::vector<size_t> order(plans.size());
      for (size_t i = 0; i < order.size(); ++i) order[i] = i;
      std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return limbs[plans[a].idx].voxels.size() < limbs[plans[b].idx].voxels.size();
      });
      std::vector<char> drop(plans.size(), 0);
      for (size_t oi : order) {
        if (est <= kMaxBones) break;
        est -= plans[oi].bones;
        drop[oi] = 1;
      }
      std::vector<LimbPlan> kept;
      for (size_t i = 0; i < plans.size(); ++i)
        if (!drop[i]) kept.push_back(plans[i]);
      plans.swap(kept);
      // rebuild limbs in kept order (already sorted by tip)
      std::vector<Limb> keptLimbs;
      for (auto& p : plans) keptLimbs.push_back(std::move(limbs[p.idx]));
      limbs.swap(keptLimbs);
      for (size_t i = 0; i < plans.size(); ++i) plans[i].idx = i;
    }
    for (size_t li = 0; li < limbs.size(); ++li) {
      const Limb& limb = limbs[li];
      std::vector<float> w(limb.voxels.size(), 1.f);
      std::vector<int> vv;
      for (int vi : limb.voxels) vv.push_back(voxels[size_t(vi)]);
      PcaResult lp = weightedPca(vv, w, s);
      // Order the limb axis: from attachment (nearest spine) to tip.
      float lmin = 1e9f, lmax = -1e9f;
      for (int v : vv) {
        const float t = dot(vsub(voxelCenter(v, s), lp.mean), lp.axis);
        lmin = std::min(lmin, t); lmax = std::max(lmax, t);
      }
      // Attachment = point nearest the spine axis.
      float attachT = 0;
      {
        float bd = 1e9f;
        for (int v : vv) {
          const Vec3 p = voxelCenter(v, s);
          const Vec3 q = vsub(p, pca.mean);
          const float d = vlen(vsub(q, vmul(axis, dot(q, axis))));
          if (d < bd) { bd = d; attachT = dot(vsub(p, lp.mean), lp.axis); }
        }
      }
      Vec3 lax = lp.axis;
      if ((attachT - (lmin + lmax) * 0.5f) > 0) lax = vmul(lax, -1.f);  // attachment at low t
      auto lpoint = [&](float t) { return vadd(lp.mean, vmul(lax, t)); };
      // Parent: spine bone nearest the attachment point.
      int parent = spineBones[0];
      {
        float bd = 1e9f;
        for (size_t bi = 0; bi < spineBones.size(); ++bi) {
          const Vec3 h = rig.bones[size_t(spineBones[bi])].head;
          const float d = std::abs(dot(vsub(lpoint(attachT), h), axis));
          if (d < bd) { bd = d; parent = spineBones[bi]; }
        }
      }
      const std::string base = "limb_" + std::to_string(li);
      const int nb = plans[li].bones;
      // Bones span [lmin, lmax] along lax (attachment at the low end by construction).
      const float lo = lmin, hi = lmax;
      auto place = [&](const std::string& nm, int par, float a, float b) {
        return addBone(nm, par, vadd(lp.mean, vmul(lax, a)), vadd(lp.mean, vmul(lax, b)));
      };
      if (nb >= 3) {
        const float m1 = lo + (hi - lo) / 3.f, m2 = lo + 2.f * (hi - lo) / 3.f;
        const int up = place(base + "_upper", parent, lo, m1);
        const int mid = place(base + "_mid", up, m1, m2);
        place(base + "_lower", mid, m2, hi);
      } else {
        const float m1 = (lo + hi) * 0.5f;
        const int up = place(base + "_upper", parent, lo, m1);
        place(base + "_lower", up, m1, hi);
      }
    }
  }

  if (!rig.valid() || int(rig.bones.size()) > kMaxBones) return std::nullopt;

  // ---- skinning: per-bone Dijkstra flood-fill through the volume ----
  {
    // Bone label per voxel: nearest bone by (axis distance, then Euclidean).
    // Seed: voxels near each bone's segment.
    const int B = int(rig.bones.size());
    std::vector<int> voxelBone(size_t(n), -1);
    for (size_t i = 0; i < voxels.size(); ++i) {
      const Vec3 p = voxelCenter(voxels[i], s);
      int best = 0;
      float bd = std::numeric_limits<float>::max();
      for (int b = 0; b < B; ++b) {
        const Bone& bone = rig.bones[size_t(b)];
        // distance to segment head->tail
        const Vec3 ab = vsub(bone.tail, bone.head);
        const float t = std::clamp(dot(vsub(p, bone.head), ab) / std::max(dot(ab, ab), 1e-9f), 0.f, 1.f);
        const float d = vlen(vsub(p, vadd(bone.head, vmul(ab, t))));
        if (d < bd) { bd = d; best = b; }
      }
      voxelBone[size_t(voxels[i])] = best;
    }
    const int K = kMaxInfluences;
    // 6-connectivity for the flood-fill (4x fewer visits than 26; still fills
    // through the volume, never across empty space).
    const int d6[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    struct DItem { float d; int v; };
    struct DCmp {
      bool operator()(const DItem& a, const DItem& b) const {
        if (a.d != b.d) return a.d > b.d;
        return a.v > b.v;
      }
    };
    std::vector<std::vector<ExtractionCache::Slot>> best;
    best.resize(size_t(n));
    std::vector<float> dist;
    dist.resize(size_t(n));
    std::vector<char> done;
    done.resize(size_t(n));
    for (int b = 0; b < B; ++b) {
      std::fill(dist.begin(), dist.end(), std::numeric_limits<float>::infinity());
      std::fill(done.begin(), done.end(), 0);
      std::priority_queue<DItem, std::vector<DItem>, DCmp> pq;
      for (int v : voxels)
        if (voxelBone[size_t(v)] == b && dist[size_t(v)] > 0.f) {
          dist[size_t(v)] = 0.f;
          pq.push({0.f, v});
        }
      if (pq.empty()) continue;
      while (!pq.empty()) {
        const DItem cur = pq.top(); pq.pop();
        if (done[size_t(cur.v)]) continue;
        done[size_t(cur.v)] = 1;
        const int x = cur.v % s, y = (cur.v / s) % s, z = cur.v / (s * s);
        for (auto& o : d6) {
          const int nx = x + o[0], ny = y + o[1], nz = z + o[2];
          if (!inGrid(nx, ny, nz, s)) continue;
          const int ni = flat(nx, ny, nz, s);
          if (!g.occ[size_t(ni)] || done[size_t(ni)]) continue;
          const float nd = cur.d + 1.f;
          if (nd < dist[size_t(ni)]) { dist[size_t(ni)] = nd; pq.push({nd, ni}); }
        }
      }
      for (int v : voxels) {
        if (dist[size_t(v)] == std::numeric_limits<float>::infinity()) continue;
        auto& bl = best[size_t(v)];
        const ExtractionCache::Slot ns{uint8_t(b), dist[size_t(v)]};
        if (int(bl.size()) < K) {
          bl.push_back(ns);
        } else {
          size_t wi = 0;
          for (size_t i = 1; i < bl.size(); ++i)
            if (bl[i].dist > bl[wi].dist) wi = i;
          if (ns.dist < bl[wi].dist) bl[wi] = ns;
        }
      }
    }
    g_cache.voxelSkin = std::move(best);
  }

  g_cache.grid = grid;
  g_cache.armed = true;
  return rig;
}

void computeSkinning(Rig& rig, const std::vector<Vec3>& vertices) {
  rig.influences.clear();
  rig.influences.resize(vertices.size());
  if (!g_cache.armed || g_cache.voxelSkin.empty()) return;
  const int s = g_cache.grid.size;
  for (size_t vi = 0; vi < vertices.size(); ++vi) {
    const Vec3& p = vertices[vi];
    int x = std::clamp(int(std::floor(p.x)), 0, s - 1);
    int y = std::clamp(int(std::floor(p.y)), 0, s - 1);
    int z = std::clamp(int(std::floor(p.z)), 0, s - 1);
    int idx = flat(x, y, z, s);
    if (g_cache.voxelSkin[size_t(idx)].empty()) {
      int found = -1;
      for (int r = 1; r < s && found < 0; ++r)
        for (int dz = -r; dz <= r && found < 0; ++dz)
          for (int dy = -r; dy <= r && found < 0; ++dy)
            for (int dx = -r; dx <= r && found < 0; ++dx) {
              if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != r) continue;
              const int nx = x + dx, ny = y + dy, nz = z + dz;
              if (!inGrid(nx, ny, nz, s)) continue;
              const int ni = flat(nx, ny, nz, s);
              if (!g_cache.voxelSkin[size_t(ni)].empty()) { found = ni; break; }
            }
      if (found >= 0) idx = found;
    }
    const auto& slots = g_cache.voxelSkin[size_t(idx)];
    auto& inf = rig.influences[vi];
    if (slots.empty()) {
      inf.push_back({0, 1.f});
      continue;
    }
    float sum = 0.f;
    for (const auto& sl : slots) {
      const float w = 1.f / ((1.f + sl.dist) * (1.f + sl.dist));
      inf.push_back({sl.bone, w});
      sum += w;
    }
    if (sum > 0.f)
      for (auto& in : inf) in.weight /= sum;
    std::sort(inf.begin(), inf.end(),
              [](const Influence& a, const Influence& b) { return a.bone < b.bone; });
  }
}

}  // namespace litholattice::anim
