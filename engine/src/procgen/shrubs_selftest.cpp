// Self-test for the procgen shrub/grass generator. Conventions follow
// procgen_selftest.cpp: a local Checker printing "  [PASS]/[FAIL]" lines.
#include "litholattice/procgen/shrubs.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include "litholattice/procgen/litho_formula.h"
#include "litholattice/procgen/litho_formula_2d.h"
#include "litholattice/subdiv/octree.h"

namespace litholattice::procgen {
namespace {

class Checker {
 public:
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }

 private:
  int fails_ = 0;
};

uint64_t fnv1a64(const uint8_t* data, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ data[i]) * 0x100000001B3ull;
  return h;
}

uint64_t textureHash(uint32_t i) {
  const uint8_t* px = plantTexturePixels(i);
  if (!px) return 0;
  return fnv1a64(px, kPlantTextureBytes);
}

bool crossEqual(const CrossQuadPlant& a, const CrossQuadPlant& b) {
  // Field-by-field (not memcmp): the struct has a padding byte the generator
  // never writes, and identical deterministic computations yield bitwise-equal
  // floats, so == is exact here.
  return a.widthBlocks == b.widthBlocks && a.heightBlocks == b.heightBlocks &&
         a.textureIndex == b.textureIndex && a.tintR == b.tintR &&
         a.tintG == b.tintG && a.tintB == b.tintB;
}

// Golden values: pinned on first green run (2026-10-11); any later change in
// the generator or texture algorithms trips these, which is the point.
// Re-pinned 2026-10-12: berry/twisted leaf hashes changed INTENTIONALLY — the
// presets gained Litho Formula cross-sections (berry m=5, twisted m=3).
constexpr uint64_t kGoldenBerryLeaves = 0xa770941504515a3full;
constexpr uint64_t kGoldenTwistedLeaves = 0x585fad24fd42e9b2ull;
constexpr uint64_t kGoldenTexGrass = 0xe7067443e4f9b1c6ull;
constexpr uint64_t kGoldenTexFern = 0x9a1e2ffb6d9082e5ull;
constexpr uint64_t kGoldenTexScrub = 0x1ea237c1888a21a1ull;

}  // namespace

int runShrubsSelfTest() {
  Checker check;

  // --- 3D determinism: same params -> identical leaf stream -----------------
  {
    const ShrubParams p = presetBerryShrub();
    const uint64_t h1 = subdiv::hashLeaves(generateShrubOctree(p));
    const uint64_t h2 = subdiv::hashLeaves(generateShrubOctree(p));
    check(h1 == h2, "3D determinism: same seed+params -> identical leaves");
    std::printf("    GOLDEN berry_leaves=%016llx\n", (unsigned long long)h1);
    const ShrubParams q = presetTwistedShrub();
    const uint64_t h3 = subdiv::hashLeaves(generateShrubOctree(q));
    std::printf("    GOLDEN twisted_leaves=%016llx\n", (unsigned long long)h3);
    if (kGoldenBerryLeaves != 0)
      check(h1 == kGoldenBerryLeaves, "3D golden: berry shrub leaf hash pinned");
    if (kGoldenTwistedLeaves != 0)
      check(h3 == kGoldenTwistedLeaves, "3D golden: twisted shrub leaf hash pinned");
  }

  // --- 3D shape sanity: nonempty, bounded, stem+leaves present ---------------
  {
    auto t = generateShrubOctree(presetBerryShrub());
    check(!t.empty(), "3D preset: berry shrub is nonempty");
    check(t.leafCount() <= size_t(kMaxShrubLeaves), "3D preset: leaf count within rail");
    check(t.leafCount() > 10, "3D preset: berry shrub has a real leaf count");
    bool sawStem = false, sawLeaf = false;
    for (const auto& c : t.leaves()) {
      sawStem = sawStem || c.material == presetBerryShrub().stemMaterial;
      sawLeaf = sawLeaf || c.material == presetBerryShrub().leafMaterial;
    }
    check(sawStem && sawLeaf, "3D preset: stem and leaf materials both present");
  }

  // --- 2D determinism + style-ignored ----------------------------------------
  {
    const ShrubParams p = presetMeadowTuft();
    const CrossQuadPlant a = generateShrubCross(p);
    const CrossQuadPlant b = generateShrubCross(p);
    check(crossEqual(a, b), "2D determinism: same seed+params -> identical plant");
    ShrubParams q = p;
    q.style = PlantStyle::Modeled3D;  // style field must not affect the 2D path
    check(crossEqual(a, generateShrubCross(q)), "2D path ignores the style flag");
    check(a.widthBlocks > 0 && a.heightBlocks > 0, "2D preset: positive footprint");
    check(a.textureIndex < numPlantTextures(), "2D preset: textureIndex in range");
    const ShrubParams r = presetDryTuft();
    const CrossQuadPlant d = generateShrubCross(r);
    check(d.widthBlocks > 0 && d.heightBlocks > 0 && d.textureIndex < numPlantTextures(),
          "2D preset: dry tuft valid");
  }

  // --- different seeds differ (both paths) ------------------------------------
  {
    ShrubParams p = presetBerryShrub();
    ShrubParams q = p;
    q.seed ^= 0x9E3779B97F4A7C15ull;
    check(subdiv::hashLeaves(generateShrubOctree(p)) != subdiv::hashLeaves(generateShrubOctree(q)),
          "3D: different seeds -> different shrubs");
    ShrubParams m = presetMeadowTuft();
    ShrubParams n = m;
    n.seed += 1;
    check(!crossEqual(generateShrubCross(m), generateShrubCross(n)),
          "2D: different seeds -> different plants");
  }

  // --- Litho Formula cross-sections -------------------------------------------
  {
    // Determinism with formula params engaged (same params+seed -> identical).
    ShrubParams star;
    star.seed = 0x57A2ull;
    star.sizeCells = 22;
    star.density = 0.9f;
    star.noiseAmount = 0.25f;
    star.stemMaterial = 3;
    star.leafMaterial = 4;
    star.superM = 4.0;  // star cross-section
    star.superN1 = 1.2;
    star.superN2 = 1.0;
    star.superN3 = 1.0;
    const uint64_t hs1 = subdiv::hashLeaves(generateShrubOctree(star));
    const uint64_t hs2 = subdiv::hashLeaves(generateShrubOctree(star));
    check(hs1 == hs2, "formula: same params+seed -> identical octree (star m=4)");
    check(validShrubParams(star), "formula: star params validate");

    // Distinct param sets -> distinct shape signatures (hash and/or leaf count).
    ShrubParams round = star;
    round.superM = 0.0;
    round.superN1 = round.superN2 = round.superN3 = 1.0;
    ShrubParams flower = star;
    flower.superM = 5.0;
    flower.superN1 = 1.1;
    const auto tRound = generateShrubOctree(round);
    const auto tStar = generateShrubOctree(star);
    const auto tFlower = generateShrubOctree(flower);
    const uint64_t hRound = subdiv::hashLeaves(tRound);
    const uint64_t hFlower = subdiv::hashLeaves(tFlower);
    check(hRound != hs1 && hRound != hFlower && hs1 != hFlower,
          "formula: round / star m=4 / flower m=5 silhouettes all differ (hash signatures)");
    check(tRound.leafCount() != tStar.leafCount() || tRound.leafCount() != tFlower.leafCount() ||
              tStar.leafCount() != tFlower.leafCount(),
          "formula: distinct param sets differ in cell-count signature");
    // Presets carry distinct formula silhouettes too.
    check(subdiv::hashLeaves(generateShrubOctree(presetBerryShrub())) !=
              subdiv::hashLeaves(generateShrubOctree(presetTwistedShrub())),
          "formula: berry (m=5) and twisted (m=3) presets differ");
    // Invalid formula params fail closed.
    ShrubParams x = star;
    x.superM = -1.0;
    check(!validShrubParams(x) && generateShrubOctree(x).empty(), "formula fail-closed: m<0");
    x = star; x.superM = 17.0;
    check(!validShrubParams(x) && generateShrubOctree(x).empty(), "formula fail-closed: m>16");
    x = star; x.superN1 = std::numeric_limits<double>::quiet_NaN();
    check(!validShrubParams(x) && generateShrubOctree(x).empty(), "formula fail-closed: n1=NaN");
    x = star; x.superN2 = 0.0;
    check(!validShrubParams(x) && generateShrubOctree(x).empty(), "formula fail-closed: n2=0");
    x = star; x.superA = 0.0;
    check(!validShrubParams(x) && generateShrubOctree(x).empty(), "formula fail-closed: a=0");
  }

  // --- degenerate params fail closed ------------------------------------------
  {
    auto bad = presetBerryShrub();
    auto expectEmpty3D = [&](const ShrubParams& x, const char* what) {
      check(generateShrubOctree(x).empty(), std::string("3D fail-closed: ") + what);
    };
    auto expectZero2D = [&](const ShrubParams& x, const char* what) {
      const CrossQuadPlant c = generateShrubCross(x);
      check(c.widthBlocks == 0.0f && c.heightBlocks == 0.0f,
            std::string("2D fail-closed: ") + what);
    };
    ShrubParams x = bad;
    x.sizeCells = 0; expectEmpty3D(x, "sizeCells=0"); expectZero2D(x, "sizeCells=0");
    x = bad; x.sizeCells = 65; expectEmpty3D(x, "sizeCells=65"); expectZero2D(x, "sizeCells=65");
    x = bad; x.sizeCells = -3; expectEmpty3D(x, "sizeCells=-3"); expectZero2D(x, "sizeCells=-3");
    x = bad; x.density = 1.5f; expectEmpty3D(x, "density>1"); expectZero2D(x, "density>1");
    x = bad; x.density = -0.1f; expectEmpty3D(x, "density<0"); expectZero2D(x, "density<0");
    x = bad; x.density = std::numeric_limits<float>::quiet_NaN();
    expectEmpty3D(x, "density=NaN"); expectZero2D(x, "density=NaN");
    x = bad; x.noiseAmount = 2.0f; expectEmpty3D(x, "noiseAmount>1"); expectZero2D(x, "noiseAmount>1");
    x = bad; x.noiseAmount = std::numeric_limits<float>::quiet_NaN();
    expectEmpty3D(x, "noiseAmount=NaN"); expectZero2D(x, "noiseAmount=NaN");
    x = bad; x.stemMaterial = 0; expectEmpty3D(x, "stemMaterial=0"); expectZero2D(x, "stemMaterial=0");
    x = bad; x.leafMaterial = 0; expectEmpty3D(x, "leafMaterial=0"); expectZero2D(x, "leafMaterial=0");
    check(!validShrubParams(x), "validShrubParams rejects the last bad case");
    check(validShrubParams(bad), "validShrubParams accepts the berry preset");
    check(validShrubParams(presetMeadowTuft()), "validShrubParams accepts the meadow preset");
  }

  // --- leaf-count rail at max size/density -------------------------------------
  {
    ShrubParams p = presetTwistedShrub();
    p.sizeCells = 64;
    p.density = 1.0f;
    p.noiseAmount = 1.0f;
    auto t = generateShrubOctree(p);
    check(!t.empty(), "3D max: sizeCells=64/density=1 still produces a shrub");
    check(t.leafCount() <= size_t(kMaxShrubLeaves), "3D max: leaf count respects the rail");
    check(t.memoryBytes() < 4 * 1024 * 1024, "3D max: sparse memory stays sane (<4MB)");
    std::printf("    max-size shrub: leaves=%zu bytes=%zu\n", t.leafCount(), t.memoryBytes());
  }

  // --- texture set: layout, names, determinism, goldens ------------------------
  {
    check(numPlantTextures() == 3, "texture set has 3 entries");
    check(plantTexturePixels(3) == nullptr, "texture accessor fails closed out of range");
    check(plantTextureName(3) == nullptr, "texture name fails closed out of range");
    bool namesOk = true, alphaOk = true, nonzeroOk = true;
    for (uint32_t i = 0; i < numPlantTextures(); ++i) {
      const char* name = plantTextureName(i);
      namesOk = namesOk && name && name[0] != '\0';
      const uint8_t* px = plantTexturePixels(i);
      if (!px) { nonzeroOk = false; continue; }
      bool anyOpaque = false, anyClear = false;
      for (int k = 0; k < kPlantTextureBytes; k += 4) {
        const uint8_t a = px[k + 3];
        anyOpaque = anyOpaque || a == 255;
        anyClear = anyClear || a == 0;
        alphaOk = alphaOk && (a == 0 || a == 255);  // cutout: binary alpha only
      }
      nonzeroOk = nonzeroOk && anyOpaque && anyClear;
      // Deterministic across calls (generation is a pure function; the golden
      // pins it across runs).
      check(textureHash(i) == fnv1a64(px, kPlantTextureBytes), "texture bytes stable");
    }
    check(namesOk, "texture names are stable non-empty strings");
    check(alphaOk, "texture alpha is binary (cutout-safe)");
    check(nonzeroOk, "each texture has both opaque and transparent pixels");
    std::printf("    names: %s %s %s\n", plantTextureName(0), plantTextureName(1),
                plantTextureName(2));
    const uint64_t hg = textureHash(0), hf = textureHash(1), hs = textureHash(2);
    std::printf("    GOLDEN tex_grass=%016llx tex_fern=%016llx tex_scrub=%016llx\n",
                (unsigned long long)hg, (unsigned long long)hf, (unsigned long long)hs);
    if (kGoldenTexGrass != 0) {
      check(hg == kGoldenTexGrass, "texture golden: grass tuft bytes pinned");
      check(hf == kGoldenTexFern, "texture golden: fern bytes pinned");
      check(hs == kGoldenTexScrub, "texture golden: dry scrub bytes pinned");
    }
    check(hg != hf && hf != hs && hg != hs, "the three textures are distinct");
  }

  // --- formula shrub sprites (2026-10-12) -------------------------------------
  {
    // Family shape: appended after the legacy three, indices untouched.
    check(numFormulaShrubTextures() == 4, "formula family has 4 entries");
    check(kFirstFormulaTextureIndex == numPlantTextures(),
          "formula indices start right after the legacy set");
    check(numPlantTexturesTotal() == numPlantTextures() + 4,
          "total addressable textures == legacy + formula");
    bool famOk = true;
    uint64_t famHash[4] = {0, 0, 0, 0};
    for (uint32_t j = 0; j < numFormulaShrubTextures(); ++j) {
      const char* name = formulaShrubTextureName(j);
      const uint8_t* px = formulaShrubTexturePixels(j);
      const ShrubParams* fp = formulaShrubTextureParams(j);
      famOk = famOk && name && name[0] != '\0' && px && fp && validShrubParams(*fp);
      if (!px || !fp) continue;
      famHash[j] = fnv1a64(px, kPlantTextureBytes);
      bool anyOpaque = false, anyClear = false, binary = true;
      for (int k = 0; k < kPlantTextureBytes; k += 4) {
        const uint8_t a = px[k + 3];
        anyOpaque = anyOpaque || a == 255;
        anyClear = anyClear || a == 0;
        binary = binary && (a == 0 || a == 255);
      }
      famOk = famOk && anyOpaque && anyClear && binary;
      // The family sprite IS generateShrubSprite output at 16px.
      const std::vector<uint8_t> gen = generateShrubSprite(*fp, kPlantTextureSize);
      check(gen.size() == size_t(kPlantTextureBytes) &&
                std::memcmp(gen.data(), px, kPlantTextureBytes) == 0,
            std::string("formula family sprite ") + name +
                " == generateShrubSprite(params, 16)");
    }
    check(famOk, "formula family: names/params/pixels valid, binary alpha, mixed coverage");
    check(famHash[0] != famHash[1] && famHash[0] != famHash[2] && famHash[0] != famHash[3] &&
              famHash[1] != famHash[2] && famHash[1] != famHash[3] && famHash[2] != famHash[3],
          "formula family sprites are pairwise distinct");
    check(formulaShrubTextureName(4) == nullptr && formulaShrubTexturePixels(4) == nullptr &&
              formulaShrubTextureParams(4) == nullptr,
          "formula family accessors fail closed out of range");
    // Unified accessors agree with both halves and fail closed.
    bool anyOk = true;
    for (uint32_t i = 0; i < numPlantTexturesTotal(); ++i) {
      const char* expectName = i < numPlantTextures()
                                   ? plantTextureName(i)
                                   : formulaShrubTextureName(i - kFirstFormulaTextureIndex);
      const uint8_t* expectPx = i < numPlantTextures()
                                    ? plantTexturePixels(i)
                                    : formulaShrubTexturePixels(i - kFirstFormulaTextureIndex);
      anyOk = anyOk && plantTextureNameAny(i) == expectName &&
              plantTexturePixelsAny(i) == expectPx;
    }
    check(anyOk, "plantTexture*Any resolve legacy and formula ranges identically");
    check(plantTextureNameAny(numPlantTexturesTotal()) == nullptr &&
              plantTexturePixelsAny(99) == nullptr,
          "plantTexture*Any fail closed out of range");

    // Index selection: exact family matches bind exactly; other formula
    // params quantize to the nearest family silhouette (by m, then n1).
    for (uint32_t j = 0; j < numFormulaShrubTextures(); ++j) {
      ShrubParams p = *formulaShrubTextureParams(j);
      p.seed = 42;  // seed must not affect the silhouette choice
      check(formulaTextureIndexFor(p) == kFirstFormulaTextureIndex + j,
            "formulaTextureIndexFor: exact family params -> their own index");
      const CrossQuadPlant c = generateShrubCross(p);
      check(c.textureIndex == kFirstFormulaTextureIndex + j,
            "generateShrubCross: formula params select the formula sprite");
      check(crossEqual(c, generateShrubCross(p)),
            "generateShrubCross: formula selection is deterministic");
    }
    {
      ShrubParams p = *formulaShrubTextureParams(0);  // flower values, then bend m
      p.superM = 7.0;                                  // nearest family m is 6 (flower)
      check(formulaTextureIndexFor(p) == kFirstFormulaTextureIndex + 0,
            "formulaTextureIndexFor: m=7 quantizes to the m=6 flower sprite");
      ShrubParams d = presetMeadowTuft();  // m=4 with diamond n, a=b=1: not the
      d.superM = 4.0;                      // leaf entry's params, but m matches it
      d.superN1 = d.superN2 = d.superN3 = 0.5;
      check(formulaTextureIndexFor(d) == kFirstFormulaTextureIndex + 2,
            "formulaTextureIndexFor: m=4 (non-exact) quantizes to the leaf sprite");
      ShrubParams plain = presetMeadowTuft();  // m == 0: legacy path untouched
      check(generateShrubCross(plain).textureIndex < numPlantTextures(),
            "generateShrubCross: m=0 params keep legacy texture indices");
    }

    // Generator contract: fail closed, deterministic, seed = speckle only.
    {
      ShrubParams bad = *formulaShrubTextureParams(0);
      bad.sizeCells = 0;
      check(generateShrubSprite(bad, 16).empty(), "sprite fail-closed: invalid params");
      const ShrubParams good = *formulaShrubTextureParams(0);
      check(generateShrubSprite(good, 3).empty(), "sprite fail-closed: pixelSize < 4");
      check(generateShrubSprite(good, 257).empty(), "sprite fail-closed: pixelSize > 256");
      const std::vector<uint8_t> a = generateShrubSprite(good, 32);
      const std::vector<uint8_t> b = generateShrubSprite(good, 32);
      check(!a.empty() && a == b, "sprite determinism: same (seed, params, size) -> identical bytes");
      ShrubParams other = good;
      other.seed ^= 0xABCDEF1234567890ull;
      const std::vector<uint8_t> c = generateShrubSprite(other, 32);
      bool maskSame = true, rgbDiffers = false;
      for (size_t k = 0; k + 3 < a.size(); k += 4) {
        maskSame = maskSame && (a[k + 3] == c[k + 3]);
        rgbDiffers = rgbDiffers || a[k] != c[k] || a[k + 1] != c[k + 1] || a[k + 2] != c[k + 2];
      }
      check(maskSame, "sprite seed variation: silhouette (alpha mask) is seed-invariant");
      check(rgbDiffers, "sprite seed variation: interior speckle changes with the seed");
      bool anyOpaque = false, anyClear = false, binary = true;
      for (int row = 0; row < 32; ++row)
        for (int col = 0; col < 32; ++col) {
          const uint8_t al = a[(size_t(row) * 32 + col) * 4 + 3];
          anyOpaque = anyOpaque || al == 255;
          anyClear = anyClear || al == 0;
          binary = binary && (al == 0 || al == 255);
        }
      check(anyOpaque && anyClear && binary, "sprite: binary alpha with mixed coverage");
      // Base placement: at density == 1 (no thinning) the silhouette reaches
      // the frame's bottom row — the documented mapping puts the outline's
      // base on the ground line. (At density < 1 the rim thinning may
      // legitimately eat the tip row's few pixels.)
      ShrubParams grounded = good;
      grounded.density = 1.0f;
      const std::vector<uint8_t> g = generateShrubSprite(grounded, 32);
      bool baseOpaque = false;
      for (int col = 0; col < 32; ++col)
        baseOpaque = baseOpaque || g[(size_t(31) * 32 + col) * 4 + 3] == 255;
      check(baseOpaque, "sprite: the shape's base sits on the frame's bottom row");
    }

    // Formula identity: at density == 1 (no thinning can fire) the sprite's
    // alpha mask must equal the radial silhouette rad <= lithoR(theta),
    // sampled per pixel through the documented frame mapping. The generator
    // rasterizes the lithoOutline2D polygon instead, so this cross-checks
    // two independent routes to the same curve. They may disagree only
    // within ~1.5 px of the true edge (pixel-centre quantization; the
    // 256-gon chord deviation is orders of magnitude smaller).
    {
      ShrubParams p = *formulaShrubTextureParams(0);  // flower
      p.density = 1.0f;
      constexpr int S = 48;
      const std::vector<uint8_t> spr = generateShrubSprite(p, S);
      check(spr.size() == size_t(S) * S * 4, "identity: sprite has the requested size");
      const litho_formula::LithoParams lp{p.superA, p.superB, p.superM,
                                          p.superN1, p.superN2, p.superN3};
      const auto outline = litho_formula::lithoOutline2D(lp, 256);
      double minX = outline[0].x, maxX = outline[0].x;
      double minY = outline[0].y, maxY = outline[0].y;
      for (const auto& v : outline) {
        minX = std::min(minX, v.x); maxX = std::max(maxX, v.x);
        minY = std::min(minY, v.y); maxY = std::max(maxY, v.y);
      }
      const double scale = std::min(S / (maxX - minX), S / (maxY - minY));
      const double cx = (minX + maxX) * 0.5;
      const double band = 1.5 * std::sqrt(2.0) / scale;  // 1.5 px, formula units
      int disagreements = 0, outsideBand = 0;
      for (int row = 0; row < S; ++row)
        for (int col = 0; col < S; ++col) {
          const double u = (col + 0.5 - S * 0.5) / scale + cx;
          const double v = (S - 1 - row + 0.5) / scale + minY;
          const double r = litho_formula::lithoR(std::atan2(v, u), lp);
          const double rad = std::sqrt(u * u + v * v);
          const bool radialIn = rad <= r;
          const bool spriteIn = spr[(size_t(row) * S + col) * 4 + 3] > 0;
          if (radialIn != spriteIn) {
            ++disagreements;
            if (std::abs(rad - r) > band) ++outsideBand;
          }
        }
      check(outsideBand == 0,
            "identity: sprite silhouette matches lithoR radial test outside a 1.5px edge band");
      check(disagreements < S * S / 20,
            "identity: edge disagreements are a thin rim (<5% of pixels)");
      std::printf("    identity: %d/%d pixels in the edge band\n", disagreements, S * S);
    }
  }

  return check.fails();
}

}  // namespace litholattice::procgen
