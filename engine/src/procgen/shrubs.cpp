// Procedural grass/shrub generator (workstream 2 of 3). See shrubs.h.
#include "litholattice/procgen/shrubs.h"

#include <array>
#include <cmath>
#include <cstring>
#include "litholattice/procgen/litho_formula.h"
#include "litholattice/procgen/litho_formula_2d.h"
#include "litholattice/procgen/seed.h"

namespace litholattice::procgen {
namespace {

int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

}  // namespace

bool validShrubParams(const ShrubParams& p) {
  if (p.sizeCells < 1 || p.sizeCells > 64) return false;
  // NaN fails both comparisons, so non-finite values are rejected too.
  if (!(p.density >= 0.0f && p.density <= 1.0f)) return false;
  if (!(p.noiseAmount >= 0.0f && p.noiseAmount <= 1.0f)) return false;
  if (p.stemMaterial == 0 || p.leafMaterial == 0) return false;  // 0 = empty/clear
  // Litho Formula cross-section params: finite and in sane ranges (the !(...)
  // form rejects NaN the same way as the float checks above).
  if (!std::isfinite(p.superM) || !std::isfinite(p.superN1) || !std::isfinite(p.superN2) ||
      !std::isfinite(p.superN3) || !std::isfinite(p.superA) || !std::isfinite(p.superB))
    return false;
  if (!(p.superM >= 0.0 && p.superM <= 16.0)) return false;
  if (!(p.superN1 >= 0.05 && p.superN1 <= 20.0)) return false;
  if (!(p.superN2 >= 0.05 && p.superN2 <= 20.0)) return false;
  if (!(p.superN3 >= 0.05 && p.superN3 <= 20.0)) return false;
  if (!(p.superA >= 0.2 && p.superA <= 5.0)) return false;
  if (!(p.superB >= 0.2 && p.superB <= 5.0)) return false;
  return true;
}

litholattice::subdiv::SparseOctree generateShrubOctree(const ShrubParams& p) {
  using litholattice::subdiv::SparseOctree;
  SparseOctree tree;
  if (!validShrubParams(p)) return tree;  // fail closed: empty octree

  // --- resolution: span `sizeCells` micro-cells using a coarser octree level
  // for big plants, so the leaf count stays bounded. `across` is the target
  // cell count across the footprint; `n` is the actual grid we fill.
  const int span = p.sizeCells;
  int across = span / 2;
  if (across < 4) across = 4;
  if (across > 16) across = 16;
  int level = 6;
  while (level > 0 && (span >> (6 - level)) > across) --level;
  const int c = 1 << (6 - level);        // micro-cells per octree cell edge
  const int n = (span + c - 1) / c;      // octree cells across (covers span)
  const int cellsPerAxis = 1 << level;
  const int cellX0 = (32 - (n * c) / 2) / c;  // centered in the block
  const int cellZ0 = cellX0;

  // --- mound shape: ellipsoid dome on the ground (y=0), two-scale lattice
  // noise displacing the surface by noiseAmount.
  const double cx = (n - 1) * 0.5, cz = cx;
  const double rx = n * 0.5, rz = rx;
  double ry = n * 0.375;
  if (ry < 2.0) ry = 2.0;
  const int jMax = int(std::ceil(ry)) + 1;

  Rng rng(deriveSeed(p.seed, deriveSeed(nameHash("shrub_octree"), uint64_t(span))));
  const uint64_t noiseSeed = deriveSeed(p.seed, nameHash("shrub_noise"));
  const float nAmp = p.noiseAmount * 0.9f;
  const int centerLo = int(cx);  // central column(s) carry the stem

  // --- Litho Formula cross-section: rNorm(theta) in 64 fixed bins, normalized
  // by the mean radius over the full circle so different (m, n) sets keep a
  // comparable mound volume. Fixed sampling -> deterministic on every run.
  // Default params (m=0, n=1, a=b=1) give r == 1 in every bin: the original
  // round ellipsoid, bit-for-bit.
  constexpr int kThetaBins = 64;
  std::array<double, kThetaBins> rNorm{};
  {
    const litho_formula::LithoParams lp{p.superA, p.superB, p.superM,
                                        p.superN1, p.superN2, p.superN3};
    const double pi = std::acos(-1.0);
    double mean = 0.0;
    for (int bIdx = 0; bIdx < kThetaBins; ++bIdx) {
      const double theta = -pi + 2.0 * pi * (bIdx + 0.5) / kThetaBins;
      rNorm[size_t(bIdx)] = litho_formula::lithoR(theta, lp);
      mean += rNorm[size_t(bIdx)];
    }
    mean /= kThetaBins;
    if (!(mean > 0.0) || !std::isfinite(mean)) mean = 1.0;  // degenerate params: round
    for (double& v : rNorm) {
      v /= mean;
      if (!(v > 1e-3) || !std::isfinite(v)) v = 1e-3;  // never divide by ~0 below
    }
  }
  const double kPiD = std::acos(-1.0);
  auto radiusAt = [&](double dxn, double dzn) {
    double theta = std::atan2(dzn, dxn);  // [-pi, pi]
    int bin = int((theta + kPiD) / (2.0 * kPiD) * kThetaBins);
    if (bin < 0) bin = 0;
    if (bin >= kThetaBins) bin = kThetaBins - 1;
    return rNorm[size_t(bin)];
  };

  int leaves = 0;
  for (int j = 0; j <= jMax; ++j) {
    const double dyn = j / ry;
    for (int k = 0; k < n; ++k) {
      const double dz = (k - cz) / rz;
      for (int i = 0; i < n; ++i) {
        const double dx = (i - cx) / rx;
        const float nse = 0.65f * signedUnitFloat(latticeHash(noiseSeed, i, j, k)) +
                          0.35f * signedUnitFloat(latticeHash(noiseSeed, i >> 1, j >> 1, k >> 1));
        // Ellipsoid test with the horizontal radius modulated by the Litho
        // Formula cross-section: (rho / rNorm(theta))^2 + dyn^2 <= 1 (+ noise).
        const double rr = radiusAt(dx, dz);
        const double qx = dx / rr, qz = dz / rr;
        const double d = qx * qx + dyn * dyn + qz * qz - 1.0 - double(nAmp) * nse;
        if (d > 0.0) continue;  // outside the mound
        const int gx = cellX0 + i, gz = cellZ0 + k;
        if (gx < 0 || gx >= cellsPerAxis || gz < 0 || gz >= cellsPerAxis || j >= cellsPerAxis)
          continue;  // defensive: never address outside the block
        const bool centerCol = (i == centerLo || i == centerLo + 1) &&
                               (k == centerLo || k == centerLo + 1);
        if (centerCol && j <= int(ry * 0.5)) {
          tree.set(level, gx, j, gz, p.stemMaterial);  // stem: always set
          continue;
        }
        if (leaves >= kMaxShrubLeaves) continue;  // safety rail, documented
        if (rng.uniform() < p.density) {
          tree.set(level, gx, j, gz, p.leafMaterial);
          ++leaves;
        }
      }
    }
  }
  return tree;
}

CrossQuadPlant generateShrubCross(const ShrubParams& p) {
  CrossQuadPlant q{0.0f, 0.0f, 0, 255, 255, 255};
  if (!validShrubParams(p)) return q;  // fail closed: zero footprint
  // NOTE: p.style is deliberately ignored — a cross plant is a cross plant.
  Rng rng(deriveSeed(p.seed, deriveSeed(nameHash("shrub_cross"), uint64_t(p.sizeCells))));
  const float s = float(p.sizeCells) / 64.0f;  // 1/64 .. 1
  q.widthBlocks = 0.25f + 0.75f * s * (0.85f + 0.3f * rng.uniform());
  q.heightBlocks = 0.30f + 0.90f * s * (0.85f + 0.3f * rng.uniform());
  q.textureIndex = uint32_t(rng.range(0, int(numPlantTextures()) - 1));
  // Litho Formula branch (2026-10-12): a non-default formula (superM > 0)
  // overrides the legacy index with the formula family's sprite. The RNG
  // stream above is consumed identically either way, so width/height/tint
  // keep the legacy draw structure; only the texture choice changes.
  if (p.superM > 0.0) q.textureIndex = formulaTextureIndexFor(p);
  // Subtle per-plant tint variation around a fresh green.
  q.tintG = uint8_t(225 + rng.range(0, 30));
  q.tintR = uint8_t(190 + rng.range(0, 45));
  q.tintB = uint8_t(180 + rng.range(0, 45));
  return q;
}

// --- Litho Formula shrub sprite generator (2026-10-12) ------------------------
// See shrubs.h for the contract (silhouette = formula outline, fill = the
// fixed set's hash-varied leafy speckle, frame mapping documented there).
namespace {

/// Even-odd point-in-polygon over the outline (formula space). The outline
/// is a single closed star-shaped polygon; ray casting is exact for it and
/// platform-deterministic (plain IEEE double arithmetic and comparisons).
bool outlineContains(const std::vector<litho_formula::Vec2>& poly, double x, double y) {
  bool inside = false;
  const size_t n = poly.size();
  for (size_t i = 0, j = n - 1; i < n; j = i++) {
    const double xi = poly[i].x, yi = poly[i].y;
    const double xj = poly[j].x, yj = poly[j].y;
    if ((yi > y) != (yj > y)) {
      const double xint = xi + (y - yi) * (xj - xi) / (yj - yi);
      if (x < xint) inside = !inside;
    }
  }
  return inside;
}

}  // namespace

std::vector<uint8_t> generateShrubSprite(const ShrubParams& p, int pixelSize) {
  if (!validShrubParams(p)) return {};  // fail closed
  if (pixelSize < 4 || pixelSize > 256) return {};
  const int S = pixelSize;
  std::vector<uint8_t> px(size_t(S) * size_t(S) * 4, 0);

  const litho_formula::LithoParams lp{p.superA, p.superB, p.superM,
                                      p.superN1, p.superN2, p.superN3};
  const std::vector<litho_formula::Vec2> outline = litho_formula::lithoOutline2D(lp, 256);
  double minX = outline[0].x, maxX = outline[0].x;
  double minY = outline[0].y, maxY = outline[0].y;
  for (const auto& v : outline) {
    minX = std::min(minX, v.x); maxX = std::max(maxX, v.x);
    minY = std::min(minY, v.y); maxY = std::max(maxY, v.y);
  }
  const double w = maxX - minX, h = maxY - minY;
  if (!(w > 0.0) || !(h > 0.0) || !std::isfinite(w) || !std::isfinite(h))
    return px;  // degenerate outline: transparent sprite (cannot occur for
                // valid params — lithoR is strictly positive there)
  // Frame mapping (shrubs.h): uniform scale fits the bbox into the frame,
  // shape centred horizontally, bbox base (minY) on the frame's bottom row.
  const double scale = std::min(double(S) / w, double(S) / h);  // px per unit
  const double cx = (minX + maxX) * 0.5;
  const uint64_t salt = deriveSeed(p.seed, nameHash("shrub_sprite"));
  // Coverage (silhouette + density thinning) is keyed on a FIXED salt: it is
  // a function of the params alone. Only the colour speckle below follows
  // p.seed, so two seeds share one alpha mask and differ in colour only.
  const uint64_t shapeSalt = nameHash("shrub_sprite_shape");
  const double stemHalfPx = std::max(1.0, S / 24.0);  // stem suggestion width

  for (int row = 0; row < S; ++row) {
    for (int col = 0; col < S; ++col) {
      // Pixel centre in formula space (row 0 = top of the sprite).
      const double u = (col + 0.5 - S * 0.5) / scale + cx;
      const double v = (S - 1 - row + 0.5) / scale + minY;
      if (!outlineContains(outline, u, v)) continue;  // outside: transparent
      uint8_t* q = px.data() + (size_t(row) * size_t(S) + size_t(col)) * 4;
      const double t = (v - minY) / h;  // 0 at the base, 1 at the top
      const float j = unitFloat(latticeHash(salt, col, row, 1)) - 0.5f;
      const float amp = 14.0f + 22.0f * p.noiseAmount;  // speckle amplitude
      // Density thinning (skip = leave transparent): a broken leafy rim in
      // the silhouette's outer band, plus sparse interior dropout that
      // vanishes as density -> 1. At density == 1 neither test can fire
      // (hashes are < 1), so the sprite is the exact silhouette.
      const double theta = std::atan2(v, u);
      const double rr = litho_formula::lithoR(theta, lp);
      const double rad = std::sqrt(u * u + v * v);
      const double fr = rr > 1e-12 ? rad / rr : 0.0;  // 0 centre, 1 edge
      if (fr > 0.80 && unitFloat(latticeHash(shapeSalt, col, row, 2)) > p.density)
        continue;
      if (fr <= 0.80 && p.density < 1.0f &&
          unitFloat(latticeHash(shapeSalt, col, row, 3)) < (1.0f - p.density) * 0.35f)
        continue;
      // Leafy fill: plant greens in the fixed set's spirit — darker at the
      // base, lighter toward the tips, hash-jittered per pixel.
      q[0] = uint8_t(clamp255(int(58.0f + 34.0f * float(t) + amp * j)));
      q[1] = uint8_t(clamp255(int(122.0f + 48.0f * float(t) + amp * j)));
      q[2] = uint8_t(clamp255(int(54.0f + 26.0f * float(t) + amp * 0.7f * j)));
      q[3] = 255;
      // Sparse bright flecks near the top (young growth catching light).
      if (t > 0.72 && unitFloat(latticeHash(salt, col, row, 4)) > 0.93f) {
        q[0] = uint8_t(clamp255(q[0] + 38));
        q[1] = uint8_t(clamp255(q[1] + 30));
        q[2] = uint8_t(clamp255(q[2] + 10));
      }
      // Stem suggestion at the base centre: a narrow brown column where the
      // shape meets the ground.
      if (t < 0.30 && std::abs(col + 0.5 - S * 0.5) <= stemHalfPx) {
        q[0] = uint8_t(clamp255(int(96.0f + 18.0f * j)));
        q[1] = uint8_t(clamp255(int(74.0f + 14.0f * j)));
        q[2] = uint8_t(clamp255(int(46.0f + 10.0f * j)));
      }
    }
  }
  return px;
}

// --- procedural plant texture set -------------------------------------------
// All three sprites are generated by fixed hash-varied algorithms: no RNG
// state, byte-identical on every run and platform. Salts are arbitrary
// fixed constants (chosen once, never changed — golden hashes in the
// self-test pin the bytes).
namespace {

constexpr uint64_t kTexSaltGrass = 0x6A55AA5512345678ull;
constexpr uint64_t kTexSaltFern = 0xFE2A11ED87654321ull;
constexpr uint64_t kTexSaltScrub = 0x5C2B8A0D0BADF00Dull;

void genGrassTuft(uint8_t* px) {
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      uint8_t* q = px + (y * 16 + x) * 4;
      const float h01 = unitFloat(latticeHash(kTexSaltGrass, x, 7, 0));
      const float taper = 1.0f - 0.45f * (std::abs(x - 7.5f) / 8.0f);  // tuft silhouette
      const int bladeH = int((5.0f + h01 * 9.0f) * taper);             // 2..14 px
      if (y < 16 - bladeH) {
        q[0] = q[1] = q[2] = q[3] = 0;
        continue;
      }
      const float t = bladeH > 1 ? float(y - (16 - bladeH)) / float(bladeH - 1) : 1.0f;
      const float j = unitFloat(latticeHash(kTexSaltGrass, x, y, 1)) - 0.5f;
      q[0] = uint8_t(clamp255(int(70.0f - 25.0f * t + 22.0f * j)));
      q[1] = uint8_t(clamp255(int(150.0f - 60.0f * t + 30.0f * j)));
      q[2] = uint8_t(clamp255(int(60.0f - 20.0f * t + 18.0f * j)));
      q[3] = 255;
    }
  }
}

void genFern(uint8_t* px) {
  constexpr double kPi = 3.14159265358979323846;
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      uint8_t* q = px + (y * 16 + x) * 4;
      q[0] = q[1] = q[2] = q[3] = 0;
      if (y >= 2 && (x == 7 || x == 8)) {  // central stem
        const float j = unitFloat(latticeHash(kTexSaltFern, x, y, 2)) - 0.5f;
        q[0] = uint8_t(clamp255(int(62 + 14 * j)));
        q[1] = uint8_t(clamp255(int(92 + 16 * j)));
        q[2] = uint8_t(clamp255(int(56 + 12 * j)));
        q[3] = 255;
        continue;
      }
      if (y < 2 || y > 14) continue;
      const double half = 6.5 * std::sin(kPi * double(y - 2) / 13.0);
      const double dx = std::abs(x - 7.5);
      if (dx <= 1.0 || dx > half) continue;
      // Serrated leaflet edge: outermost pixels dither.
      if (dx > half - 1.0 && ((x * 3 + y * 5) & 1) == 0) continue;
      const float j = unitFloat(latticeHash(kTexSaltFern, x, y, 3)) - 0.5f;
      q[0] = uint8_t(clamp255(int(58 + 20 * j)));
      q[1] = uint8_t(clamp255(int(140 + 26 * j)));
      q[2] = uint8_t(clamp255(int(88 + 20 * j)));
      q[3] = 255;
    }
  }
}

void genDryScrub(uint8_t* px) {
  // Five twigs from the base (7.5, 15) to jittered tips; distance-to-segment
  // rasterization, then sparse olive leaf blobs near the twigs.
  struct Twig { float tx, ty; };
  Twig twigs[5];
  for (int t = 0; t < 5; ++t) {
    twigs[t].tx = 2.0f + t * 3.0f + (unitFloat(latticeHash(kTexSaltScrub, t, 0, 4)) - 0.5f) * 3.0f;
    twigs[t].ty = 3.0f + unitFloat(latticeHash(kTexSaltScrub, t, 1, 4)) * 4.0f;
  }
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      uint8_t* q = px + (y * 16 + x) * 4;
      q[0] = q[1] = q[2] = q[3] = 0;
      const float px0 = x + 0.5f, py0 = y + 0.5f;
      float best = 1e9f;
      for (const Twig& tw : twigs) {
        const float ax = 7.5f, ay = 15.0f;
        const float vx = tw.tx - ax, vy = tw.ty - ay;
        const float tt = (vx * (px0 - ax) + vy * (py0 - ay)) / (vx * vx + vy * vy);
        const float c = tt < 0.0f ? 0.0f : tt > 1.0f ? 1.0f : tt;
        const float dx = px0 - (ax + vx * c), dy = py0 - (ay + vy * c);
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d < best) best = d;
      }
      const float j = unitFloat(latticeHash(kTexSaltScrub, x, y, 5)) - 0.5f;
      if (best < 0.9f) {  // twig body: olive-brown
        q[0] = uint8_t(clamp255(int(139 + 26 * j)));
        q[1] = uint8_t(clamp255(int(117 + 24 * j)));
        q[2] = uint8_t(clamp255(int(82 + 20 * j)));
        q[3] = 255;
      } else if (best < 2.6f && unitFloat(latticeHash(kTexSaltScrub, x, y, 6)) > 0.90f) {
        q[0] = uint8_t(clamp255(int(150 + 24 * j)));  // sparse dry leaf blob
        q[1] = uint8_t(clamp255(int(140 + 24 * j)));
        q[2] = uint8_t(clamp255(int(80 + 20 * j)));
        q[3] = 255;
      }
    }
  }
}

const std::array<std::array<uint8_t, kPlantTextureBytes>, 3>& plantTextureSet() {
  static const auto set = [] {
    std::array<std::array<uint8_t, kPlantTextureBytes>, 3> s{};
    genGrassTuft(s[0].data());
    genFern(s[1].data());
    genDryScrub(s[2].data());
    return s;
  }();
  return set;
}

// --- formula shrub texture family --------------------------------------------
// Canonical formula sprites (2026-10-12): fixed param sets mirroring the
// lithoPresets2D silhouettes (flower / star / leaf / blob), each rendered
// once by generateShrubSprite at 16x16 with a fixed canonical seed — the
// family is seed-invariant exactly like the legacy three above.
struct FormulaShrubEntry {
  const char* name;
  ShrubParams params;
};

const std::array<FormulaShrubEntry, 4>& formulaShrubTable() {
  static const auto table = [] {
    std::array<FormulaShrubEntry, 4> t{};
    auto mk = [](const char* name, double m, double n1, double n2, double n3,
                 double a, double b) {
      FormulaShrubEntry e;
      e.name = name;
      e.params.seed = deriveSeed(nameHash(name), 0x5EEDF02Full);
      e.params.style = PlantStyle::CrossedQuads;
      e.params.sizeCells = 12;
      e.params.density = 0.8f;
      e.params.noiseAmount = 0.35f;
      e.params.stemMaterial = 1;
      e.params.leafMaterial = 2;
      e.params.superM = m;
      e.params.superN1 = n1;
      e.params.superN2 = n2;
      e.params.superN3 = n3;
      e.params.superA = a;
      e.params.superB = b;
      return e;
    };
    t[0] = mk("procgen_formula_flower", 6.0, 2.0, 4.0, 4.0, 1.0, 1.0);
    t[1] = mk("procgen_formula_star", 5.0, 1.0, 0.5, 0.5, 1.0, 1.0);
    t[2] = mk("procgen_formula_leaf", 4.0, 0.6, 0.6, 0.6, 1.8, 1.0);
    t[3] = mk("procgen_formula_blob", 3.0, 1.5, 1.5, 1.5, 1.0, 1.0);
    return t;
  }();
  return table;
}

const std::array<std::array<uint8_t, kPlantTextureBytes>, 4>& formulaShrubSet() {
  static const auto set = [] {
    std::array<std::array<uint8_t, kPlantTextureBytes>, 4> s{};
    const auto& t = formulaShrubTable();
    for (size_t j = 0; j < t.size(); ++j) {
      const std::vector<uint8_t> sprite =
          generateShrubSprite(t[j].params, kPlantTextureSize);
      // generateShrubSprite returns exactly kPlantTextureBytes here (the
      // canonical params always validate); copy defensively regardless.
      if (sprite.size() == s[j].size())
        std::memcpy(s[j].data(), sprite.data(), s[j].size());
    }
    return s;
  }();
  return set;
}

}  // namespace

uint32_t numPlantTextures() { return 3; }

const char* plantTextureName(uint32_t i) {
  static const char* const names[] = {"procgen_grass_tuft", "procgen_fern", "procgen_dry_scrub"};
  return i < 3 ? names[i] : nullptr;
}

const uint8_t* plantTexturePixels(uint32_t i) {
  if (i >= 3) return nullptr;
  return plantTextureSet()[i].data();
}

uint32_t numFormulaShrubTextures() {
  return uint32_t(formulaShrubTable().size());
}

const char* formulaShrubTextureName(uint32_t j) {
  return j < numFormulaShrubTextures() ? formulaShrubTable()[j].name : nullptr;
}

const uint8_t* formulaShrubTexturePixels(uint32_t j) {
  if (j >= numFormulaShrubTextures()) return nullptr;
  return formulaShrubSet()[j].data();
}

const ShrubParams* formulaShrubTextureParams(uint32_t j) {
  return j < numFormulaShrubTextures() ? &formulaShrubTable()[j].params : nullptr;
}

uint32_t formulaTextureIndexFor(const ShrubParams& p) {
  const auto& t = formulaShrubTable();
  uint32_t best = 0;
  double bestDm = -1.0, bestDn = -1.0;
  for (uint32_t j = 0; j < uint32_t(t.size()); ++j) {
    const ShrubParams& e = t[j].params;
    if (e.superM == p.superM && e.superN1 == p.superN1 && e.superN2 == p.superN2 &&
        e.superN3 == p.superN3 && e.superA == p.superA && e.superB == p.superB)
      return kFirstFormulaTextureIndex + j;  // exact silhouette match
    const double dm = std::abs(e.superM - p.superM);
    const double dn = std::abs(e.superN1 - p.superN1);
    if (bestDm < 0.0 || dm < bestDm || (dm == bestDm && dn < bestDn)) {
      best = j;
      bestDm = dm;
      bestDn = dn;
    }
  }
  return kFirstFormulaTextureIndex + best;  // nearest family sprite
}

uint32_t numPlantTexturesTotal() {
  return numPlantTextures() + numFormulaShrubTextures();
}

const char* plantTextureNameAny(uint32_t i) {
  if (i < numPlantTextures()) return plantTextureName(i);
  if (i < numPlantTexturesTotal())
    return formulaShrubTextureName(i - kFirstFormulaTextureIndex);
  return nullptr;
}

const uint8_t* plantTexturePixelsAny(uint32_t i) {
  if (i < numPlantTextures()) return plantTexturePixels(i);
  if (i < numPlantTexturesTotal())
    return formulaShrubTexturePixels(i - kFirstFormulaTextureIndex);
  return nullptr;
}

// --- example plants -----------------------------------------------------------
ShrubParams presetBerryShrub() {
  ShrubParams p;
  p.seed = 0xBEEFCAFE12345678ull;
  p.style = PlantStyle::Modeled3D;
  p.sizeCells = 14;
  p.density = 0.65f;
  p.noiseAmount = 0.45f;
  p.stemMaterial = 3;
  p.leafMaterial = 4;
  // Flower/star cross-section: five lobes (m=5), soft exponent.
  p.superM = 5.0;
  p.superN1 = 1.1;
  p.superN2 = 1.0;
  p.superN3 = 1.0;
  return p;
}

ShrubParams presetTwistedShrub() {
  ShrubParams p;
  p.seed = 0xDEADBEEF87654321ull;
  p.style = PlantStyle::Modeled3D;
  p.sizeCells = 22;
  p.density = 0.35f;
  p.noiseAmount = 0.8f;
  p.stemMaterial = 3;
  p.leafMaterial = 5;
  // Three-lobe cross-section (m=3): the "twist" reads as a triangular mound.
  p.superM = 3.0;
  p.superN1 = 1.3;
  p.superN2 = 1.1;
  p.superN3 = 1.1;
  return p;
}

ShrubParams presetMeadowTuft() {
  ShrubParams p;
  p.seed = 0x0BADF00DCAFEBABEull;
  p.style = PlantStyle::CrossedQuads;
  p.sizeCells = 10;
  p.density = 0.7f;
  p.noiseAmount = 0.3f;
  p.stemMaterial = 1;
  p.leafMaterial = 2;
  return p;
}

ShrubParams presetDryTuft() {
  ShrubParams p;
  p.seed = 0xFEEDFACE0D15EA5Eull;
  p.style = PlantStyle::CrossedQuads;
  p.sizeCells = 8;
  p.density = 0.5f;
  p.noiseAmount = 0.6f;
  p.stemMaterial = 1;
  p.leafMaterial = 2;
  return p;
}

}  // namespace litholattice::procgen
