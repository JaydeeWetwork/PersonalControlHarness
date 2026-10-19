// texture_selftest.cpp: headless checks for the texture module.
//
// Covers the contract: determinism, seamless tiling (exact periodicity of
// the field primitives + seam continuity), non-blank output for the bundled
// recipes, recipe text round-trip, cache identity, parse error reporting,
// and a naive-vs-optimised benchmark (both paths must produce identical
// pixels; both timings are printed).
#include "litholattice/texture/texture_selftest.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "litholattice/texture/texture_generator.h"
#include "litholattice/texture/texture_recipe.h"

namespace litholattice {
namespace {

class Checker {
 public:
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] texture: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }

 private:
  int fails_ = 0;
};

const char* kRecipes[] = {
    // "stone": gradient FBM + curve + grey palette.
    "noise kind=gradient scale=4 octaves=4 lacunarity=2 gain=0.5\n"
    "curve contrast=1.6 gamma=1.1\n"
    "palette stops=0:#20242c,0.55:#6b7280,1:#d7dce3\n",
    // "circuit": voronoi cracks + scribe grid, max-blended, emissive glow.
    "voronoi scale=6 mode=edge\n"
    "grid cells=8 width=0.12 sharpness=1.5\n"
    "blend mode=max mix=1\n"
    "palette stops=0:#0b1020,0.7:#123a5e,1:#37e0c8\n"
    "glow threshold=0.7 intensity=0.9 color=#aef7e8\n",
    // "warped ridges": domain warp + ridged value FBM + posterize + dither.
    "warp strength=0.18 scale=3 octaves=3\n"
    "noise kind=value scale=5 octaves=5 lacunarity=2 gain=0.5\n"
    "ridged\n"
    "curve posterize=6 contrast=1.2\n"
    "palette stops=0:#1a0f2e,0.5:#7a3cff,1:#ffb347\n"
    "dither amount=10\n",
    // "billow clouds": billow transform of a low-frequency value FBM.
    "noise kind=value scale=3 octaves=3\n"
    "billow\n"
    "palette stops=0:#10131a,0.6:#4a5a78,1:#e8eef7\n",
};
constexpr const char* kRecipeNames[] = {"stone", "circuit", "warped ridges", "billow clouds"};

bool parseOrFail(const char* text, TextureRecipe* out) {
  std::string err;
  return parseRecipe(text, out, &err);
}

double pixelVariance(const Image& img) {
  const std::vector<uint8_t>& px = img.pixels();
  if (px.empty()) return 0.0;
  double mean[4] = {0, 0, 0, 0};
  const size_t n = px.size() / 4;
  for (size_t i = 0; i < n; ++i)
    for (int ch = 0; ch < 4; ++ch) mean[ch] += px[i * 4 + ch];
  for (double& m : mean) m /= double(n);
  double var = 0.0;
  for (size_t i = 0; i < n; ++i)
    for (int ch = 0; ch < 3; ++ch) {
      const double d = double(px[i * 4 + ch]) - mean[ch];
      var += d * d;
    }
  return var / double(n * 3);
}

void testDeterminismAndVariance(Checker& c) {
  for (size_t r = 0; r < 4; ++r) {
    TextureRecipe recipe;
    c(parseOrFail(kRecipes[r], &recipe), std::string("bundled recipe parses: ") + kRecipeNames[r]);
    const Image a = generateTexture(recipe, 1234, 64);
    const Image b = generateTexture(recipe, 1234, 64);
    c(a.width() == 64 && a.height() == 64 && a == b,
      std::string("determinism: two generations byte-identical (") + kRecipeNames[r] + ")");
    c(pixelVariance(a) > 100.0,
      std::string("non-blank: pixel variance above threshold (") + kRecipeNames[r] + ")");
    const Image other = generateTexture(recipe, 999, 64);
    c(!(other == a), std::string("different seed gives different pixels (") + kRecipeNames[r] + ")");
  }
}

void testTiling(Checker& c) {
  // Exact periodicity of the field primitives (the wrap property every
  // generator is built on): f(p) == f(p + period) bit-for-bit. Sample
  // coordinates are dyadic fractions so x + period is exact in float --
  // the wrap itself lives in the integer lattice hashing, which is what
  // generation relies on (it only ever samples positions in [0, period),
  // and a neighbouring tile samples the identical positions).
  bool exact = true;
  for (int period : {1, 3, 4, 7, 16}) {
    for (float x : {0.0f, 0.5f, 2.25f, 5.75f}) {
      for (float y : {0.0f, 1.5f, 4.75f}) {
        exact = exact && textureValueNoise(x, y, period, 42) == textureValueNoise(x + float(period), y, period, 42);
        exact = exact && textureValueNoise(x, y, period, 42) == textureValueNoise(x, y + float(period), period, 42);
        exact = exact && textureGradientNoise(x, y, period, 7) == textureGradientNoise(x + float(period), y, period, 7);
        exact = exact && textureGradientNoise(x, y, period, 7) == textureGradientNoise(x, y - float(period), period, 7);
      }
    }
  }
  c(exact, "tiling: value/gradient noise are exactly periodic over the tile period");
  // Seam continuity: sampling just left of the seam and just right of it
  // (wrapped) must be close -- no visible cliff at the tile border.
  bool continuous = true;
  const float eps = 0.002f;
  for (int period : {4, 8}) {
    for (float y : {0.5f, 2.25f, 6.75f}) {
      const float left = textureGradientNoise(float(period) - eps, y, period, 99);
      const float right = textureGradientNoise(-eps, y, period, 99);
      continuous = continuous && std::fabs(left - right) < 0.05f;
      const float lv = textureValueNoise(float(period) - eps, y, period, 99);
      const float rv = textureValueNoise(-eps, y, period, 99);
      continuous = continuous && std::fabs(lv - rv) < 0.05f;
    }
  }
  c(continuous, "tiling: field is continuous across the left/right seam");
  // Image-level: opposite edges of a generated tile must not be a hard
  // cliff -- the mean absolute step across the seam (wrapping) is comparable
  // to the mean step between interior neighbours.
  TextureRecipe recipe;
  if (parseOrFail(kRecipes[0], &recipe)) {
    const Image img = generateTexture(recipe, 5, 64);
    double interior = 0.0, seam = 0.0;
    int ni = 0, ns = 0;
    for (int y = 0; y < 64; ++y) {
      for (int x = 0; x < 63; ++x) {
        interior += std::abs(int(img.pixel(x, y)[0]) - int(img.pixel(x + 1, y)[0]));
        ++ni;
      }
      seam += std::abs(int(img.pixel(63, y)[0]) - int(img.pixel(0, y)[0]));
      ++ns;
    }
    c(seam / ns < 3.0 * (interior / ni) + 8.0, "tiling: image seam step is comparable to interior steps");
  }
}

void testRoundTrip(Checker& c) {
  for (size_t r = 0; r < 4; ++r) {
    TextureRecipe a, b;
    c(parseOrFail(kRecipes[r], &a), std::string("round-trip source parses: ") + kRecipeNames[r]);
    const std::string text = recipeToText(a);
    std::string err;
    c(parseRecipe(text, &b, &err), std::string("recipeToText output reparses: ") + kRecipeNames[r]);
    c(a == b, std::string("round-trip preserves the recipe exactly: ") + kRecipeNames[r]);
    c(generateTexture(a, 77, 48) == generateTexture(b, 77, 48),
      std::string("round-trip generates identical pixels: ") + kRecipeNames[r]);
  }
  // remap alias parses and serialises canonically as curve.
  TextureRecipe r;
  std::string err;
  c(parseRecipe("noise scale=2\nremap invert=1 posterize=4\n", &r, &err) &&
        recipeToText(r).find("curve") != std::string::npos,
    "remap alias parses and serialises as curve");
}

void testParseErrors(Checker& c) {
  TextureRecipe r;
  std::string err;
  c(!parseRecipe("frobnicate scale=2\n", &r, &err) && !err.empty(), "parse error: unknown node reported");
  c(!parseRecipe("noise scale=banana\n", &r, &err) && !err.empty(), "parse error: bad number reported");
  c(!parseRecipe("noise bogus=1\n", &r, &err) && !err.empty(), "parse error: unknown key reported");
  c(!parseRecipe("palette stops=0.5\n", &r, &err) && !err.empty(), "parse error: malformed palette stop reported");
  c(!parseRecipe("", &r, &err) && !err.empty(), "parse error: empty recipe reported");
  c(!parseRecipe("noise scale=2 scale=3\n", &r, &err) && !err.empty(), "parse error: duplicate key reported");
  // Hostile-input bands (P3-3, Appendix E M2): previously unvalidated
  // floats reached llround/pow/floor UB in the generator.
  c(!parseRecipe("noise lacunarity=100\n", &r, &err) && !err.empty(), "parse error: lacunarity > 4 rejected");
  c(!parseRecipe("noise lacunarity=0.5\n", &r, &err) && !err.empty(), "parse error: lacunarity < 1 rejected");
  c(!parseRecipe("noise gain=2\n", &r, &err) && !err.empty(), "parse error: gain > 1 rejected");
  c(!parseRecipe("noise\ncurve gamma=0\n", &r, &err) && !err.empty(), "parse error: gamma 0 rejected");
  c(!parseRecipe("noise\ncurve gamma=100\n", &r, &err) && !err.empty(), "parse error: gamma > 10 rejected");
  c(!parseRecipe("warp strength=1000\n", &r, &err) && !err.empty(), "parse error: warp strength > 8 rejected");
  c(!parseRecipe("noise\nblend mix=2\n", &r, &err) && !err.empty(), "parse error: blend mix > 1 rejected");
  c(parseRecipe("noise lacunarity=3.5 gain=0.25\ncurve gamma=2.5 contrast=-3\nwarp strength=2\n", &r, &err),
    "bands: in-band edge values still parse");
  // Node / stop / input caps (E M2).
  {
    std::string many;
    for (int i = 0; i < 300; ++i) many += "constant value=0.5\n";
    c(!parseRecipe(many, &r, &err) && !err.empty(), "parse error: > 256 nodes rejected");
    std::string stops = "palette stops=";
    for (int i = 0; i < 65; ++i) {
      if (i) stops += ",";
      stops += "0.5:#ff0000";
    }
    c(!parseRecipe("noise\n" + stops + "\n", &r, &err) && !err.empty(),
      "parse error: > 64 palette stops rejected");
    const std::string huge(size_t(1) << 20, '#');
    c(!parseRecipe(huge + "\nnoise\n", &r, &err) && !err.empty(), "parse error: > 1 MiB recipe text rejected");
  }
}

void testCache(Checker& c) {
  TextureRecipe recipe;
  c(parseOrFail(kRecipes[1], &recipe), "cache recipe parses");
  const Image direct = generateTexture(recipe, 31337, 64);
  const Image& cached1 = generateTextureCached(recipe, 31337, 64);
  c(cached1 == direct, "cache: cached pixels identical to direct generation");
  const Image snapshot = cached1;
  const Image& cached2 = generateTextureCached(recipe, 31337, 64);
  c(cached2 == snapshot, "cache: second lookup returns identical pixels");
  c(textureCacheSize() >= 1 && textureCacheSize() <= 64, "cache: LRU size stays within its 64-entry bound");
  // Clamped-size key (P3-3, Appendix E L1): size 8192 clamps to the same
  // 4096 generation as size 4096, so both must hit ONE cache entry.
  {
    TextureRecipe small;
    c(parseOrFail("constant value=0.25\npalette stops=0:#101010,1:#f0f0f0\n", &small),
      "cache: clamp-key recipe parses");
    const Image& a = generateTextureCached(small, 777, 4096);
    const size_t afterFirst = textureCacheSize();
    const Image& b = generateTextureCached(small, 777, 8192);
    c(a == b && a.width() == 4096, "cache: size 8192 returns the clamped 4096 image");
    c(textureCacheSize() == afterFirst, "cache: clamped sizes share one entry (no duplicate)");
  }
}

// Minimum per-iteration time over several batches: on a shared VM the
// minimum is the batch least disturbed by other tenants, so it tracks the
// actual work far better than a mean.
double benchMs(const TextureRecipe& r, bool reference) {
  constexpr int kBatches = 5, kIters = 20;
  double best = 1e30;
  for (int b = 0; b < kBatches; ++b) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i) {
      const Image img = reference ? generateTextureReference(r, 2024, 64) : generateTexture(r, 2024, 64);
      if (img.empty()) return -1.0;
    }
    const auto t1 = std::chrono::steady_clock::now();
    best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count() / kIters);
  }
  return best;
}

void testOptimizedMatchesReference(Checker& c) {
  for (size_t r = 0; r < 4; ++r) {
    TextureRecipe recipe;
    if (!parseOrFail(kRecipes[r], &recipe)) continue;
    const Image fast = generateTexture(recipe, 4242, 64);
    const Image ref = generateTextureReference(recipe, 4242, 64);
    c(fast == ref, std::string("optimised path is pixel-identical to reference (") + kRecipeNames[r] + ")");
    std::printf("  [bench] texture: %-14s 64x64 reference %8.3f ms  optimised %8.3f ms\n", kRecipeNames[r],
                benchMs(recipe, true), benchMs(recipe, false));
  }
}

}  // namespace

int runTextureSelfTest() {
  Checker c;
  testDeterminismAndVariance(c);
  testTiling(c);
  testRoundTrip(c);
  testParseErrors(c);
  testCache(c);
  testOptimizedMatchesReference(c);
  return c.fails();
}

}  // namespace litholattice
