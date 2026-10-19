// texture_generator.cpp: clean-room procedural texture generation.
//
// Noise maths (implemented from scratch here):
//   * Value noise: each integer lattice point gets a pseudo-random value in
//     [0,1] from a seeded permutation table; points between lattice points
//     are smootherstep-interpolated.
//   * Gradient (Perlin-style) noise: each lattice point gets one of 8 unit
//     gradient vectors; the value is the interpolated dot product of the
//     corner gradients with the corner offset vectors.
//   * Voronoi: each integer cell gets a pseudo-random feature point; the
//     field is the distance to the nearest (F1) or the F2-F1 gap (edges).
//   * FBM: octaves of the above summed with lacunarity/gain falloff.
//
// Tiling: lattice/cell coordinates are wrapped modulo the octave's integer
// period BEFORE hashing, and voronoi neighbour cells wrap too, so every
// field is exactly periodic over the tile. Non-integer lacunarity is fine:
// each octave's period is llround(scale * lacunarity^o), an integer, and the
// sample position is scaled by period/scale to match.
//
// Determinism: seeding is integer-only (splitmix64 builds the permutation
// table from the seed at generation start). Float maths uses only +,-,*,/
// and sqrt/pow from libm; identical for a given binary, which is the
// documented determinism contract.
#include "litholattice/texture/texture_generator.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>
#include <mutex>
#include <utility>

namespace litholattice {
namespace {

// ---------------------------------------------------------------- hashing

uint64_t splitmix64(uint64_t& s) {
  uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

// Seeded permutation table (the fast path: built once per generation, then
// every lattice hash is a few byte lookups).
struct Tables {
  uint8_t perm[512];
};

Tables buildTables(uint64_t seed) {
  Tables t;
  uint8_t p[256];
  for (int i = 0; i < 256; ++i) p[i] = static_cast<uint8_t>(i);
  uint64_t s = seed ^ 0xA0761D6478BD642FULL;
  for (int i = 255; i > 0; --i) {
    const int j = static_cast<int>(splitmix64(s) % static_cast<uint64_t>(i + 1));
    std::swap(p[i], p[j]);
  }
  for (int i = 0; i < 512; ++i) t.perm[i] = p[i & 255];
  return t;
}

// Hash of an already-wrapped lattice coordinate. Result in [0,255].
inline uint8_t latticeHash(const Tables& t, int x, int y, int salt) {
  uint32_t h = t.perm[x & 255];
  h = t.perm[(h + static_cast<uint32_t>(y)) & 255];
  h = t.perm[(h + ((x >> 8) & 255) + ((y >> 8) & 255) * 57u) & 255];
  h = t.perm[(h + static_cast<uint32_t>(salt)) & 255];
  return static_cast<uint8_t>(h);
}

inline int wrapCoord(int c, int period) {
  const int r = c % period;
  return r < 0 ? r + period : r;
}

inline float smootherstep(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

constexpr float kInvSqrt2 = 0.70710678f;
constexpr float kGrad[8][2] = {{1, 0}, {kInvSqrt2, kInvSqrt2}, {0, 1}, {-kInvSqrt2, kInvSqrt2},
                               {-1, 0}, {-kInvSqrt2, -kInvSqrt2}, {0, -1}, {kInvSqrt2, -kInvSqrt2}};

// Memoised lattice hashes for one octave (optimised path only). Same values
// as latticeHash -- this is a pure memo table, keyed by wrapped coords. The
// storage is a view into the generation context's reusable scratch buffer.
struct LatticeMemo {
  int period = 0;
  const uint8_t* hashes = nullptr;  // period*period entries, null when not built
  // Callers (rawNoise) pass already-wrapped coordinates in [0, period), so
  // the lookup is a bare array read -- no modulo work per corner.
  uint8_t get(const Tables& t, int x, int y, int salt) const {
    if (hashes && salt == saltUsed) return hashes[size_t(y) * size_t(period) + size_t(x)];
    return latticeHash(t, x, y, salt);
  }
  int saltUsed = 0;
};

// Raw single-octave noise at cell-space position (x, y). Value: [0,1].
// Gradient: approximately [-1,1].
float rawNoise(const Tables& t, const LatticeMemo* memo, NoiseKind kind, float x, float y, int period, int salt) {
  const int ix = static_cast<int>(std::floor(x));
  const int iy = static_cast<int>(std::floor(y));
  const float fx = x - float(ix);
  const float fy = y - float(iy);
  const int x0 = wrapCoord(ix, period), x1 = wrapCoord(ix + 1, period);
  const int y0 = wrapCoord(iy, period), y1 = wrapCoord(iy + 1, period);
  auto hash = [&](int cx, int cy) {
    return memo ? memo->get(t, cx, cy, salt) : latticeHash(t, cx, cy, salt);
  };
  const float sx = smootherstep(fx), sy = smootherstep(fy);
  if (kind == NoiseKind::Value) {
    const float v00 = hash(x0, y0) * (1.0f / 255.0f);
    const float v10 = hash(x1, y0) * (1.0f / 255.0f);
    const float v01 = hash(x0, y1) * (1.0f / 255.0f);
    const float v11 = hash(x1, y1) * (1.0f / 255.0f);
    return lerp(lerp(v00, v10, sx), lerp(v01, v11, sx), sy);
  }
  auto dot = [&](int cx, int cy, float ox, float oy) {
    const float* g = kGrad[hash(cx, cy) & 7];
    return g[0] * ox + g[1] * oy;
  };
  const float d00 = dot(x0, y0, fx, fy);
  const float d10 = dot(x1, y0, fx - 1.0f, fy);
  const float d01 = dot(x0, y1, fx, fy - 1.0f);
  const float d11 = dot(x1, y1, fx - 1.0f, fy - 1.0f);
  return lerp(lerp(d00, d10, sx), lerp(d01, d11, sx), sy) * kInvSqrt2;
}

// FBM octave parameters, computed identically by both evaluation paths
// (iterative lacunarity multiplication, integer-rounded periods).
struct Octave {
  int period;
  float amp;
};
std::vector<Octave> octaveParams(const TextureNode& n) {
  std::vector<Octave> out;
  float cur = n.scale;
  float amp = 1.0f;
  for (int o = 0; o < n.octaves; ++o) {
    // Defence in depth (P3-3, Appendix E M2): parseRecipe bands lacunarity,
    // but a programmatic recipe must not reach llround(±inf) or an
    // out-of-range int cast either. Periods above 2^24 are meaningless for
    // a <= 4096px field; clamp there instead of invoking UB.
    int period = 1 << 24;
    if (std::isfinite(cur) && cur < float(1 << 24))
      period = std::max(1, static_cast<int>(std::llround(cur)));
    out.push_back({period, amp});
    cur *= n.lacunarity;
    amp *= n.gain;
  }
  return out;
}

// ------------------------------------------------------------- generation

struct Ctx {
  Tables tables;
  int size = 0;
  bool optimized = false;
  std::vector<float> warpU, warpV;                 // accumulated domain warp (tile units)
  std::vector<std::vector<float>> stack;           // field stack
  std::vector<std::vector<float>> pool;            // scratch buffer reuse (optimised path)
  std::vector<float> scratchU, scratchV;           // per-pixel scaled coords (optimised path)
  std::vector<uint8_t> memoScratch;                // lattice memo storage (optimised path)
  Image image;
  bool hasImage = false;
  std::vector<float> lastField;                    // field as of the last palette (for glow)

  // Builds the lattice memo for one octave into memoScratch (optimised path).
  LatticeMemo buildMemo(int period, int salt) {
    LatticeMemo m;
    m.period = period;
    m.saltUsed = salt;
    if (period >= 1 && period <= 256) {
      memoScratch.resize(size_t(period) * size_t(period));
      for (int y = 0; y < period; ++y)
        for (int x = 0; x < period; ++x)
          memoScratch[size_t(y) * size_t(period) + size_t(x)] = latticeHash(tables, x, y, salt);
      m.hashes = memoScratch.data();
    }
    return m;
  }

  size_t count() const { return size_t(size) * size_t(size); }
  std::vector<float> acquire() {
    if (optimized && !pool.empty()) {
      std::vector<float> b = std::move(pool.back());
      pool.pop_back();
      b.assign(count(), 0.0f);
      return b;
    }
    return std::vector<float>(count(), 0.0f);
  }
  void release(std::vector<float>&& b) {
    if (optimized && pool.size() < 4) pool.push_back(std::move(b));
  }
  void ensureStack() {
    if (stack.empty()) stack.push_back(acquire());
  }
};

float warpX(const Ctx& c, size_t i) { return c.warpU[i]; }
float warpY(const Ctx& c, size_t i) { return c.warpV[i]; }

// Noise FBM field for `node` (kind taken from the node unless forced).
void evalNoise(Ctx& c, const TextureNode& node, NoiseKind kind, int salt, std::vector<float>& out) {
  const std::vector<Octave> octs = octaveParams(node);
  const int sz = c.size;
  if (c.optimized) {
    // Octave-major: hoist the per-pixel scaled coordinates (identical float
    // ops to the reference path: ((u0 * scale) * ratio) per octave), build
    // the per-octave lattice memo once, then accumulate octave by octave.
    const size_t n = c.count();
    std::vector<float>& us = c.scratchU;
    std::vector<float>& vs = c.scratchV;
    us.resize(n);
    vs.resize(n);
    for (int y = 0; y < sz; ++y) {
      const float vBase = (float(y) + 0.5f) / float(sz);
      for (int x = 0; x < sz; ++x) {
        const size_t i = size_t(y) * size_t(sz) + size_t(x);
        us[i] = ((float(x) + 0.5f) / float(sz) + warpX(c, i)) * node.scale;
        vs[i] = (vBase + warpY(c, i)) * node.scale;
      }
    }
    for (const Octave& o : octs) {
      const LatticeMemo memo = c.buildMemo(o.period, salt);
      const float ratio = float(o.period) / node.scale;
      for (size_t i = 0; i < n; ++i)
        out[i] += o.amp * rawNoise(c.tables, &memo, kind, us[i] * ratio, vs[i] * ratio, o.period, salt);
    }
  } else {
    // Reference path: per pixel, per octave, direct lattice hashing.
    for (int y = 0; y < sz; ++y) {
      const float vBase = (float(y) + 0.5f) / float(sz);
      for (int x = 0; x < sz; ++x) {
        const size_t i = size_t(y) * size_t(sz) + size_t(x);
        const float u0 = (float(x) + 0.5f) / float(sz) + warpX(c, i);
        const float v0 = vBase + warpY(c, i);
        float sum = 0.0f;
        for (const Octave& o : octs) {
          const float ratio = float(o.period) / node.scale;
          sum += o.amp * rawNoise(c.tables, nullptr, kind, u0 * node.scale * ratio, v0 * node.scale * ratio,
                                  o.period, salt);
        }
        out[i] = sum;
      }
    }
    return;  // `out` holds the raw sum; finishNoise normalises it
  }
}

// Applies the FBM normalisation/mapping shared by both paths.
void finishNoise(const TextureNode& node, NoiseKind kind, std::vector<float>& f) {
  const std::vector<Octave> octs = octaveParams(node);
  float norm = 0.0f;
  for (const Octave& o : octs) norm += o.amp;
  if (norm <= 0.0f) norm = 1.0f;
  const float inv = 1.0f / norm;
  for (float& v : f) {
    v *= inv;
    if (kind == NoiseKind::Gradient) v = clampf(0.5f + 0.5f * v, 0.0f, 1.0f);
    else v = clampf(v, 0.0f, 1.0f);
  }
}

void pushNoise(Ctx& c, const TextureNode& node, int nodeIndex) {
  std::vector<float> f = c.acquire();
  const int salt = node.seedOffset * 31 + nodeIndex * 101 + 7;
  evalNoise(c, node, node.noiseKind, salt, f);
  finishNoise(node, node.noiseKind, f);
  c.stack.push_back(std::move(f));
}

void evalVoronoi(Ctx& c, const TextureNode& node, int nodeIndex, std::vector<float>& out) {
  const int period = std::max(1, static_cast<int>(std::llround(node.scale)));
  const int saltA = node.seedOffset * 31 + nodeIndex * 101 + 7001;
  const int saltB = saltA + 4099;
  // Optimised path: memoise every cell's feature point once per node.
  std::vector<float> points;  // 2 * period * period
  if (c.optimized && period <= 256) {
    points.resize(size_t(period) * size_t(period) * 2);
    for (int cy = 0; cy < period; ++cy)
      for (int cx = 0; cx < period; ++cx) {
        points[(size_t(cy) * size_t(period) + size_t(cx)) * 2 + 0] = latticeHash(c.tables, cx, cy, saltA) * (1.0f / 255.0f);
        points[(size_t(cy) * size_t(period) + size_t(cx)) * 2 + 1] = latticeHash(c.tables, cx, cy, saltB) * (1.0f / 255.0f);
      }
  }
  auto feature = [&](int wx, int wy, float* fx, float* fy) {
    if (!points.empty()) {
      const float* p = &points[(size_t(wy) * size_t(period) + size_t(wx)) * 2];
      *fx = p[0];
      *fy = p[1];
    } else {
      *fx = latticeHash(c.tables, wx, wy, saltA) * (1.0f / 255.0f);
      *fy = latticeHash(c.tables, wx, wy, saltB) * (1.0f / 255.0f);
    }
  };
  const int sz = c.size;
  for (int y = 0; y < sz; ++y) {
    const float vBase = (float(y) + 0.5f) / float(sz);
    for (int x = 0; x < sz; ++x) {
      const size_t i = size_t(y) * size_t(sz) + size_t(x);
      const float px = ((float(x) + 0.5f) / float(sz) + warpX(c, i)) * float(period);
      const float py = (vBase + warpY(c, i)) * float(period);
      const int cx = static_cast<int>(std::floor(px));
      const int cy = static_cast<int>(std::floor(py));
      float f1 = 1e30f, f2 = 1e30f;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int cellX = cx + dx, cellY = cy + dy;
          float fx = 0, fy = 0;
          feature(wrapCoord(cellX, period), wrapCoord(cellY, period), &fx, &fy);
          const float ox = float(cellX) + fx - px;
          const float oy = float(cellY) + fy - py;
          const float d = ox * ox + oy * oy;
          if (d < f1) { f2 = f1; f1 = d; }
          else if (d < f2) { f2 = d; }
        }
      }
      f1 = std::sqrt(f1);
      f2 = std::sqrt(f2);
      out[i] = node.voronoiMode == VoronoiMode::F1 ? clampf(f1 * kInvSqrt2, 0.0f, 1.0f)
                                                   : clampf((f2 - f1) * 2.0f, 0.0f, 1.0f);
    }
  }
}

void evalGrid(Ctx& c, const TextureNode& node, std::vector<float>& out) {
  const int sz = c.size;
  const float w = std::max(node.lineWidth, 1e-6f);
  for (int y = 0; y < sz; ++y) {
    const float vBase = (float(y) + 0.5f) / float(sz);
    for (int x = 0; x < sz; ++x) {
      const size_t i = size_t(y) * size_t(sz) + size_t(x);
      const float gx = ((float(x) + 0.5f) / float(sz) + warpX(c, i)) * float(node.cells);
      const float gy = (vBase + warpY(c, i)) * float(node.cells);
      const float fx = gx - std::floor(gx), fy = gy - std::floor(gy);
      const float dx = std::min(fx, 1.0f - fx), dy = std::min(fy, 1.0f - fy);
      const float d = std::min(dx, dy);
      out[i] = std::pow(clampf(1.0f - d / w, 0.0f, 1.0f), node.sharpness);
    }
  }
}

uint8_t toByte(float v) {
  // NaN fails every clampf comparison and would reach lround unchanged
  // (UB); map any non-finite / NaN field value to 0 (P3-3, Appendix E M2).
  if (!(v > 0.0f)) v = 0.0f;
  return static_cast<uint8_t>(std::lround(clampf(v, 0.0f, 1.0f) * 255.0f));
}

TextureColor sampleGradient(std::vector<GradientStop> stops, float v) {
  if (stops.empty()) {
    const uint8_t g = toByte(v);
    return {g, g, g, 255};
  }
  std::stable_sort(stops.begin(), stops.end(),
                   [](const GradientStop& a, const GradientStop& b) { return a.pos < b.pos; });
  v = clampf(v, 0.0f, 1.0f);
  if (v <= stops.front().pos) return stops.front().color;
  if (v >= stops.back().pos) return stops.back().color;
  for (size_t i = 1; i < stops.size(); ++i) {
    if (v <= stops[i].pos) {
      const float span = stops[i].pos - stops[i - 1].pos;
      const float t = span > 0.0f ? (v - stops[i - 1].pos) / span : 0.0f;
      const TextureColor& a = stops[i - 1].color;
      const TextureColor& b = stops[i].color;
      auto ch = [&](uint8_t ca, uint8_t cb) {
        return static_cast<uint8_t>(std::lround(lerp(float(ca), float(cb), t)));
      };
      return {ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), ch(a.a, b.a)};
    }
  }
  return stops.back().color;
}

void applyPalette(Ctx& c, const std::vector<GradientStop>& stops) {
  c.ensureStack();
  const std::vector<float>& f = c.stack.back();
  std::vector<uint8_t> px(c.count() * 4);
  for (size_t i = 0; i < c.count(); ++i) {
    const TextureColor col = sampleGradient(stops, f[i]);
    px[i * 4 + 0] = col.r;
    px[i * 4 + 1] = col.g;
    px[i * 4 + 2] = col.b;
    px[i * 4 + 3] = col.a;
  }
  c.image = Image(c.size, c.size, std::move(px));
  c.hasImage = true;
  c.lastField = f;
}

void applyGlow(Ctx& c, const TextureNode& node) {
  if (!c.hasImage) applyPalette(c, {});
  std::vector<uint8_t> px = c.image.pixels();  // copy out (image replaced below)
  const float range = std::max(1.0f - node.threshold, 1e-6f);
  for (size_t i = 0; i < c.count(); ++i) {
    const float f = c.lastField[i];
    if (f <= node.threshold) continue;
    const float t = clampf(node.intensity * (f - node.threshold) / range, 0.0f, 1.0f);
    uint8_t* p = &px[i * 4];
    p[0] = static_cast<uint8_t>(std::lround(lerp(float(p[0]), float(node.glowColor.r), t)));
    p[1] = static_cast<uint8_t>(std::lround(lerp(float(p[1]), float(node.glowColor.g), t)));
    p[2] = static_cast<uint8_t>(std::lround(lerp(float(p[2]), float(node.glowColor.b), t)));
  }
  c.image = Image(c.size, c.size, std::move(px));
}

void applyDither(Ctx& c, const TextureNode& node) {
  if (!c.hasImage) applyPalette(c, {});
  static constexpr int kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  std::vector<uint8_t> px = c.image.pixels();
  for (int y = 0; y < c.size; ++y) {
    for (int x = 0; x < c.size; ++x) {
      const size_t i = size_t(y) * size_t(c.size) + size_t(x);
      const float off = ((float(kBayer[y & 3][x & 3]) + 0.5f) / 16.0f - 0.5f) * node.amount;
      uint8_t* p = &px[i * 4];
      for (int ch = 0; ch < 3; ++ch)
        p[ch] = static_cast<uint8_t>(std::lround(clampf(float(p[ch]) + off, 0.0f, 255.0f)));
    }
  }
  c.image = Image(c.size, c.size, std::move(px));
}

Image generateImpl(const TextureRecipe& recipe, uint64_t seed, int size, bool optimized) {
  if (size <= 0) return Image();
  size = std::min(size, 4096);
  Ctx c;
  c.tables = buildTables(seed);
  c.size = size;
  c.optimized = optimized;
  c.warpU.assign(c.count(), 0.0f);
  c.warpV.assign(c.count(), 0.0f);

  int nodeIndex = 0;
  for (const TextureNode& node : recipe.nodes) {
    switch (node.type) {
      case TextureNodeType::Noise:
        pushNoise(c, node, nodeIndex);
        break;
      case TextureNodeType::Voronoi: {
        std::vector<float> f = c.acquire();
        evalVoronoi(c, node, nodeIndex, f);
        c.stack.push_back(std::move(f));
        break;
      }
      case TextureNodeType::Grid: {
        std::vector<float> f = c.acquire();
        evalGrid(c, node, f);
        c.stack.push_back(std::move(f));
        break;
      }
      case TextureNodeType::Constant: {
        std::vector<float> f = c.acquire();
        std::fill(f.begin(), f.end(), node.value);
        c.stack.push_back(std::move(f));
        break;
      }
      case TextureNodeType::Warp: {
        // Two value-noise FBM fields (x and y offsets) sampled at the
        // current coordinates, accumulated into the warp buffers.
        std::vector<float> fx = c.acquire();
        std::vector<float> fy = c.acquire();
        const int salt = node.seedOffset * 31 + nodeIndex * 101 + 7;
        evalNoise(c, node, NoiseKind::Value, salt + 11, fx);
        finishNoise(node, NoiseKind::Value, fx);
        evalNoise(c, node, NoiseKind::Value, salt + 23, fy);
        finishNoise(node, NoiseKind::Value, fy);
        for (size_t i = 0; i < c.count(); ++i) {
          c.warpU[i] += (fx[i] * 2.0f - 1.0f) * node.strength;
          c.warpV[i] += (fy[i] * 2.0f - 1.0f) * node.strength;
        }
        c.release(std::move(fx));
        c.release(std::move(fy));
        break;
      }
      case TextureNodeType::Ridged: {
        c.ensureStack();
        for (float& v : c.stack.back()) v = 1.0f - std::fabs(2.0f * v - 1.0f);
        break;
      }
      case TextureNodeType::Billow: {
        c.ensureStack();
        for (float& v : c.stack.back()) {
          const float n = 2.0f * v - 1.0f;
          v = n * n;
        }
        break;
      }
      case TextureNodeType::Curve: {
        c.ensureStack();
        for (float& v : c.stack.back()) {
          float x = v;
          if (node.invert) x = 1.0f - x;
          x = std::pow(clampf(x, 0.0f, 1.0f), node.gamma);
          x = (x - 0.5f) * node.contrast + 0.5f;
          if (node.posterize >= 2) {
            const float steps = float(node.posterize);
            x = std::floor(x * (steps - 1.0f) + 0.5f) / (steps - 1.0f);
          } else if (node.posterize == 1) {
            x = 0.0f;
          }
          v = clampf(x, node.clampMin, node.clampMax);
        }
        break;
      }
      case TextureNodeType::Blend: {
        if (c.stack.size() >= 2) {
          std::vector<float> top = std::move(c.stack.back());
          c.stack.pop_back();
          std::vector<float>& base = c.stack.back();
          const float m = clampf(node.mix, 0.0f, 1.0f);
          for (size_t i = 0; i < c.count(); ++i) {
            const float a = base[i], b = top[i];
            float combined = a;
            switch (node.blendMode) {
              case BlendMode::Multiply: combined = a * b; break;
              case BlendMode::Add: combined = a + b; break;
              case BlendMode::Min: combined = std::min(a, b); break;
              case BlendMode::Max: combined = std::max(a, b); break;
              case BlendMode::Screen: combined = 1.0f - (1.0f - a) * (1.0f - b); break;
            }
            base[i] = lerp(a, combined, m);
          }
          c.release(std::move(top));
        }
        break;
      }
      case TextureNodeType::Palette:
        applyPalette(c, node.stops);
        break;
      case TextureNodeType::Glow:
        applyGlow(c, node);
        break;
      case TextureNodeType::Dither:
        applyDither(c, node);
        break;
    }
    ++nodeIndex;
  }
  if (!c.hasImage) applyPalette(c, {});  // default greyscale mapping
  return c.image;
}

// ------------------------------------------------------------------- cache

struct CacheEntry {
  uint64_t recipeHash;
  uint64_t seed;
  int size;
  Image image;
};

uint64_t fnv1a(std::string_view s, uint64_t h) {
  for (const char ch : s) {
    h ^= static_cast<uint8_t>(ch);
    h *= 1099511628211ULL;
  }
  return h;
}

std::list<CacheEntry>& cacheEntries() {
  static std::list<CacheEntry> entries;
  return entries;
}
std::mutex& cacheMutex() {
  static std::mutex m;
  return m;
}

}  // namespace

Image generateTexture(const TextureRecipe& recipe, uint64_t seed, int size) {
  return generateImpl(recipe, seed, size, /*optimized=*/true);
}

Image generateTextureReference(const TextureRecipe& recipe, uint64_t seed, int size) {
  return generateImpl(recipe, seed, size, /*optimized=*/false);
}

const Image& generateTextureCached(const TextureRecipe& recipe, uint64_t seed, int size) {
  // Key on the CLAMPED size (P3-3, Appendix E L1): generateTexture clamps
  // to 4096, so size=4096 and size=8192 previously cached the same image
  // twice under different keys.
  const int keySize = std::min(size, 4096);
  const std::string text = recipeToText(recipe);
  uint64_t h = fnv1a(text, 1469598103934665603ULL);
  uint64_t sm = h ^ seed;
  h ^= splitmix64(sm);
  h ^= static_cast<uint64_t>(static_cast<uint32_t>(keySize)) + 0x9E3779B97F4A7C15ULL;
  std::lock_guard<std::mutex> lock(cacheMutex());
  std::list<CacheEntry>& entries = cacheEntries();
  for (auto it = entries.begin(); it != entries.end(); ++it) {
    if (it->recipeHash == h && it->seed == seed && it->size == keySize) {
      entries.splice(entries.begin(), entries, it);  // LRU: mark most-recent
      return entries.front().image;
    }
  }
  entries.push_front(CacheEntry{h, seed, keySize, generateTexture(recipe, seed, keySize)});
  while (entries.size() > 64) entries.pop_back();
  return entries.front().image;
}

size_t textureCacheSize() {
  std::lock_guard<std::mutex> lock(cacheMutex());
  return cacheEntries().size();
}

float textureValueNoise(float x, float y, int period, uint64_t seed) {
  const Tables t = buildTables(seed);
  return rawNoise(t, nullptr, NoiseKind::Value, x, y, std::max(1, period), 0);
}

float textureGradientNoise(float x, float y, int period, uint64_t seed) {
  const Tables t = buildTables(seed);
  return rawNoise(t, nullptr, NoiseKind::Gradient, x, y, std::max(1, period), 0);
}

}  // namespace litholattice
