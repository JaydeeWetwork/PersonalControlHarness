// Procedural tree generator implementation. See trees.h for the contract.
#include "litholattice/procgen/trees.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "litholattice/procgen/seed.h"

namespace litholattice::procgen {
namespace {

constexpr int kLevel = 6;            ///< micro-cell level: 64^3 cells per block volume
constexpr int kCells = 64;
constexpr float kCenter = 32.0f;     ///< tree base x/z (block-volume center)
constexpr int kMaxSegments = 1500;   ///< hard cap on organic branch segments (fail-closed)
constexpr int kMaxSideShards = 10;   ///< hard cap on crystalline side shards
constexpr float kPi = 3.14159265358979323846f;

bool badFloat(float v, float lo, float hi) { return !(v >= lo && v <= hi); }

/// Defensive copy of the params: clamp everything into its documented range.
/// Degenerate input yields an empty or minimal tree, never a crash.
TreeParams sanitize(const TreeParams& p) {
  TreeParams q = p;
  q.heightCells = std::clamp(q.heightCells, 1, 64);
  q.trunkRadiusCells = std::clamp(q.trunkRadiusCells, 1, 8);
  q.branchLevels = std::clamp(q.branchLevels, 0, 6);
  q.branchesPerNode = std::clamp(q.branchesPerNode, 1, 8);
  if (badFloat(q.branchAngleRad, 0.0f, kPi)) q.branchAngleRad = 0.5f;
  if (badFloat(q.lengthFalloff, 0.01f, 0.99f)) q.lengthFalloff = 0.65f;
  if (badFloat(q.noiseAmount, 0.0f, 1.0f)) q.noiseAmount = 0.3f;
  return q;
}

struct Vec3 {
  float x = 0, y = 0, z = 0;
  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
};

float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float len(const Vec3& v) { return std::sqrt(dot(v, v)); }
Vec3 norm(const Vec3& v) {
  const float l = len(v);
  return l > 1e-6f ? v * (1.0f / l) : Vec3{0, 1, 0};
}
/// Rodrigues' rotation of v around unit axis u by angle a.
Vec3 rotateAround(const Vec3& v, const Vec3& u, float a) {
  const float c = std::cos(a), s = std::sin(a);
  return v * c + cross(u, v) * s + u * (dot(u, v) * (1.0f - c));
}

class Painter {
 public:
  /// maxY: hard height ceiling (level-6 cells); cells at y >= maxY are skipped.
  /// Fail-closed backstop so the tree is ALWAYS at most maxY cells tall.
  explicit Painter(litholattice::subdiv::SparseOctree& t, int maxY) : tree_(t), maxY_(maxY) {}
  void cell(int x, int y, int z, uint8_t mat) {
    if (mat == 0) return;
    if (x < 0 || y < 0 || z < 0 || x >= kCells || y >= kCells || z >= kCells) return;
    if (y >= maxY_) return;
    tree_.set(kLevel, x, y, z, mat);
  }
  /// Capsule from a to b with radius r (cell centers at integer+0.5).
  void capsule(const Vec3& a, const Vec3& b, float r, uint8_t mat) {
    if (mat == 0 || r <= 0) return;
    const Vec3 ab = b - a;
    const float ab2 = dot(ab, ab);
    const float rr = r * r;
    const int x0 = std::max(0, int(std::floor(std::min(a.x, b.x) - r)));
    const int x1 = std::min(kCells - 1, int(std::ceil(std::max(a.x, b.x) + r)));
    const int y0 = std::max(0, int(std::floor(std::min(a.y, b.y) - r)));
    const int y1 = std::min(kCells - 1, int(std::ceil(std::max(a.y, b.y) + r)));
    const int z0 = std::max(0, int(std::floor(std::min(a.z, b.z) - r)));
    const int z1 = std::min(kCells - 1, int(std::ceil(std::max(a.z, b.z) + r)));
    for (int y = y0; y <= y1; ++y)
      for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
          const Vec3 p{float(x) + 0.5f, float(y) + 0.5f, float(z) + 0.5f};
          const Vec3 ap = p - a;
          float t = ab2 > 1e-9f ? dot(ap, ab) / ab2 : 0.0f;
          t = std::clamp(t, 0.0f, 1.0f);
          const Vec3 q = a + ab * t;
          const Vec3 d = p - q;
          if (dot(d, d) <= rr) cell(x, y, z, mat);
        }
  }
  /// Solid sphere (leaf blobs, crystal cores).
  void sphere(const Vec3& c, float r, uint8_t mat) { capsule(c, c, r, mat); }
  /// Octahedral facet: |dx|+|dy|+|dz| <= r around c (crystal bipyramid).
  void octahedron(const Vec3& c, float r, uint8_t mat) {
    if (mat == 0 || r <= 0) return;
    const int x0 = std::max(0, int(std::floor(c.x - r))), x1 = std::min(kCells - 1, int(std::ceil(c.x + r)));
    const int y0 = std::max(0, int(std::floor(c.y - r))), y1 = std::min(kCells - 1, int(std::ceil(c.y + r)));
    const int z0 = std::max(0, int(std::floor(c.z - r))), z1 = std::min(kCells - 1, int(std::ceil(c.z + r)));
    for (int y = y0; y <= y1; ++y)
      for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
          const float dx = std::fabs(float(x) + 0.5f - c.x);
          const float dy = std::fabs(float(y) + 0.5f - c.y);
          const float dz = std::fabs(float(z) + 0.5f - c.z);
          if (dx + dy + dz <= r) cell(x, y, z, mat);
        }
  }
  /// Box of half-width w and height h, base center (cx, y0, cz), rotated by yaw
  /// around Y (crystal shard).
  void rotatedBox(float cx, float y0, float cz, float w, float h, float yaw, uint8_t mat) {
    if (mat == 0 || w <= 0 || h <= 0) return;
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float ext = w * 1.5f;  // rotated bbox extent
    const int x0 = std::max(0, int(std::floor(cx - ext))), x1 = std::min(kCells - 1, int(std::ceil(cx + ext)));
    const int yA = std::max(0, int(std::floor(y0))), yB = std::min(kCells - 1, int(std::ceil(y0 + h)));
    const int z0 = std::max(0, int(std::floor(cz - ext))), z1 = std::min(kCells - 1, int(std::ceil(cz + ext)));
    for (int y = yA; y <= yB; ++y)
      for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
          const float lx = float(x) + 0.5f - cx, lz = float(z) + 0.5f - cz;
          const float rx = lx * cy + lz * sy, rz = -lx * sy + lz * cy;
          if (std::fabs(rx) <= w && std::fabs(rz) <= w) cell(x, y, z, mat);
        }
  }

 private:
  litholattice::subdiv::SparseOctree& tree_;
  int maxY_ = kCells;
};

Vec3 randomUnit(Rng& rng) {
  Vec3 v{rng.range(-1.f, 1.f), rng.range(-1.f, 1.f), rng.range(-1.f, 1.f)};
  if (len(v) < 1e-4f) v = {0, 1, 0};
  return norm(v);
}

// ---- organic growth ----

struct OrganicCtx {
  Painter* paint = nullptr;
  Rng* rng = nullptr;
  TreeParams p;
  int segments = 0;
  int extremities = 0;
};

void growBranch(OrganicCtx& ctx, const Vec3& pos, const Vec3& dir, float length, float radius,
                int depth) {
  if (ctx.segments >= kMaxSegments) return;
  ++ctx.segments;
  const Vec3 end = pos + dir * length;
  ctx.paint->capsule(pos, end, radius, ctx.p.trunkMaterial);
  if (depth <= 0 || length < 2.0f) {
    // Branch end: leaf blob + glowing extremity tip.
    if (ctx.p.leafMaterial != 0) {
      const float blobR = std::clamp(std::ceil(radius * 2.2f), 2.0f, 4.0f);
      ctx.paint->sphere(end, blobR, ctx.p.leafMaterial);
    }
    if (ctx.p.tipMaterial != 0) {
      const int ei = ctx.extremities++;
      // Two-colour tips: alternate tipMaterial / leafMaterial by extremity index
      // when both are nonzero (e.g. the trace-forest's red/blue tips).
      const uint8_t tipMat =
          (ei % 2 == 0 || ctx.p.leafMaterial == 0) ? ctx.p.tipMaterial : ctx.p.leafMaterial;
      ctx.paint->cell(int(std::round(end.x - 0.5f)), int(std::round(end.y - 0.5f)),
                      int(std::round(end.z - 0.5f)), tipMat);
    }
    return;
  }
  const Vec3 up{0, 1, 0};
  for (int i = 0; i < ctx.p.branchesPerNode; ++i) {
    if (ctx.segments >= kMaxSegments) return;
    // Rotation axis: random perpendicular to the parent direction.
    Vec3 axis = cross(dir, randomUnit(*ctx.rng));
    if (len(axis) < 1e-4f) axis = cross(dir, Vec3{1, 0, 0});
    axis = norm(axis);
    const float theta = ctx.p.branchAngleRad * (0.6f + 0.8f * ctx.rng->uniform()) +
                        ctx.p.noiseAmount * 1.2f * (ctx.rng->uniform() - 0.5f);
    Vec3 childDir = rotateAround(dir, axis, theta);
    childDir = norm(childDir + up * 0.30f);  // upward bias: branches reach for the sky
    const float attachT = (i % 2 == 0) ? 1.0f : 0.7f;  // alternate tip/mid attachment
    const Vec3 attach = pos + dir * (length * attachT);
    const float childLen = std::max(2.0f, length * ctx.p.lengthFalloff);
    const float childRad = std::max(0.55f, radius * 0.62f);
    growBranch(ctx, attach, childDir, childLen, childRad, depth - 1);
  }
}

void growOrganic(litholattice::subdiv::SparseOctree& tree, const TreeParams& p, Rng& rng) {
  Painter paint(tree, p.heightCells);
  OrganicCtx ctx{&paint, &rng, p, 0, 0};

  // Tapered trunk column: radius shrinks ~40% from base to top.
  int trunkH = std::max(1, p.heightCells * 45 / 100);
  if (rng.uniform() < 0.5f) trunkH = std::min(trunkH + 1, p.heightCells);
  const float r0 = float(p.trunkRadiusCells);
  for (int y = 0; y < trunkH; ++y) {
    const float r = r0 * (1.0f - 0.4f * float(y) / float(trunkH));
    paint.capsule({kCenter, float(y), kCenter}, {kCenter, float(y) + 1.0f, kCenter}, r,
                  p.trunkMaterial);
  }

  // Crown: recursive branches from the trunk top, budgeted so the tree stays
  // within heightCells (geometric series of the falloff lengths).
  if (p.branchLevels > 0 && trunkH < p.heightCells) {
    float series = 0.0f, f = 1.0f;
    for (int i = 0; i <= p.branchLevels; ++i) {
      series += f;
      f *= p.lengthFalloff;
    }
    float branchLen = float(p.heightCells - trunkH) / series * 0.9f;
    branchLen = std::clamp(branchLen, 3.0f, float(p.heightCells - trunkH));
    growBranch(ctx, {kCenter, float(trunkH), kCenter}, {0, 1, 0}, branchLen, r0 * 0.6f,
               p.branchLevels);
  } else if (p.branchLevels == 0 && p.leafMaterial != 0) {
    // No branches: a lone tuft on the trunk top.
    paint.sphere({kCenter, float(trunkH), kCenter}, 2.0f, p.leafMaterial);
  }
}

// ---- crystalline growth ----

void growCrystal(litholattice::subdiv::SparseOctree& tree, const TreeParams& p, Rng& rng) {
  Painter paint(tree, p.heightCells);
  const int shardH = std::max(2, p.heightCells / 6);
  const float r0 = float(p.trunkRadiusCells);
  float yaw = rng.range(0.0f, 2.0f * kPi);
  int sideShards = 0;
  int layer = 0;
  for (int y0 = 0; y0 < p.heightCells; y0 += shardH, ++layer) {
    const float h = std::min(float(shardH), float(p.heightCells - y0));
    const float w = std::max(0.75f, r0 * (1.0f - 0.75f * float(y0) / float(p.heightCells)));
    yaw += rng.range(0.35f, 0.95f);  // spiral twist per layer
    uint8_t mat = (layer % 2 == 0) ? p.trunkMaterial : p.leafMaterial;
    if (mat == 0 && p.trunkMaterial != 0) mat = p.trunkMaterial;  // never leave hollow gaps
    if (layer % 2 == 0) {
      paint.rotatedBox(kCenter, float(y0), kCenter, w, h, yaw, mat);
    } else {
      paint.octahedron({kCenter, float(y0) + h * 0.5f, kCenter}, std::max(w, h * 0.75f), mat);
    }
    // Thin side shards jutting at an angle, every other layer.
    if (layer % 2 == 0 && sideShards < kMaxSideShards && rng.uniform() < 0.55f) {
      const int n = 1 + rng.range(0, 1);
      for (int s = 0; s < n && sideShards < kMaxSideShards; ++s) {
        ++sideShards;
        const float syaw = rng.range(0.0f, 2.0f * kPi);
        const float tilt = rng.range(0.9f, 1.3f);  // radians from vertical
        const Vec3 d{std::cos(syaw) * std::sin(tilt), std::cos(tilt), std::sin(syaw) * std::sin(tilt)};
        const Vec3 base{kCenter + std::cos(syaw) * w, float(y0) + h * 0.5f,
                        kCenter + std::sin(syaw) * w};
        // Cap the shard length to the available headroom so side shards render
        // whole instead of hitting the height ceiling (deterministic: pure
        // function of the already-drawn stream).
        const float maxRise = float(p.heightCells) - base.y - 0.7f;  // 0.7 = shard radius
        float sl = rng.range(3.0f, 7.0f);
        const float maxSl = maxRise / std::max(0.2f, d.y);
        if (sl > maxSl) sl = maxSl;
        if (sl < 2.5f) continue;  // no headroom: skip this shard
        uint8_t smat = (layer % 2 == 0) ? p.leafMaterial : p.trunkMaterial;
        if (smat == 0) smat = p.trunkMaterial;
        paint.capsule(base, base + d * sl, 0.7f, smat);
      }
    }
  }
  // Glowing apex shard, centered so its full radius fits under the ceiling.
  if (p.tipMaterial != 0) {
    paint.octahedron({kCenter, float(p.heightCells) - 2.0f, kCenter}, 1.5f, p.tipMaterial);
  }
}

}  // namespace

litholattice::subdiv::SparseOctree generateTree(const TreeParams& p) {
  const TreeParams q = sanitize(p);
  Rng rng(deriveSeed(q.seed, nameHash("tree")));
  litholattice::subdiv::SparseOctree tree;
  if (q.crystalline)
    growCrystal(tree, q, rng);
  else
    growOrganic(tree, q, rng);
  return tree;
}

// ---- biome family presets ----

TreeParams clockwoodWirewood(uint64_t seed) {
  // Caller remap: trunkMaterial should be the copper-brown bark slot; leafMaterial
  // stays 0 (wirewood carries few/no leaves), tipMaterial 0 (no glow).
  TreeParams p;
  p.seed = seed;
  p.heightCells = int(56 + (seed % 9));  // 56..64: tall
  p.trunkRadiusCells = 2;                // thin
  p.branchLevels = 4;
  p.branchesPerNode = 2;  // sparse
  p.branchAngleRad = 0.9f;
  p.lengthFalloff = 0.78f;  // long branches
  p.noiseAmount = 0.35f;
  p.trunkMaterial = 3;  // copper-brown bark slot (caller palette)
  p.leafMaterial = 0;   // no leaf blobs
  p.tipMaterial = 0;
  p.crystalline = false;
  return p;
}

TreeParams dopantCrystalTree(uint64_t seed) {
  // Caller remap: trunkMaterial = blue crystal slot, leafMaterial = amber crystal
  // slot (shard layers alternate blue/amber), tipMaterial = white-hot core slot
  // for the apex shard (0 disables the apex glow).
  TreeParams p;
  p.seed = seed;
  p.heightCells = int(40 + (seed % 17));  // 40..56
  p.trunkRadiusCells = 3;
  p.branchLevels = 0;  // unused in crystalline mode
  p.branchesPerNode = 2;
  p.branchAngleRad = 0.5f;
  p.lengthFalloff = 0.65f;
  p.noiseAmount = 0.6f;  // stronger twist variance
  p.trunkMaterial = 4;   // blue crystal slot (caller palette)
  p.leafMaterial = 5;    // amber crystal slot (caller palette)
  p.tipMaterial = 6;     // white-hot core slot (caller palette)
  p.crystalline = true;
  return p;
}

TreeParams traceForestTree(uint64_t seed) {
  // Caller remap: trunkMaterial = copper slot, tipMaterial = red glow slot,
  // leafMaterial = blue glow slot. The two-colour tip rule alternates red/blue
  // glowing extremities by extremity index; the small sparse leaf blobs read as
  // circuit nodes on the trace canopy.
  TreeParams p;
  p.seed = seed;
  p.heightCells = int(44 + (seed % 13));  // 44..56
  p.trunkRadiusCells = 2;
  p.branchLevels = 3;
  p.branchesPerNode = 3;
  p.branchAngleRad = 0.7f;
  p.lengthFalloff = 0.6f;
  p.noiseAmount = 0.45f;
  p.trunkMaterial = 7;  // copper slot (caller palette)
  p.leafMaterial = 9;   // blue glow slot (caller palette): blobs + blue tips
  p.tipMaterial = 8;    // red glow slot (caller palette): red tips
  p.crystalline = false;
  return p;
}

}  // namespace litholattice::procgen
