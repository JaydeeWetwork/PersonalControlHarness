// Wafer code surface rules: the per-biome surface work that JSON material names cannot
// express. Everything here is deterministic (world coordinates and the pipeline's seeded
// biome weights only, no RNG streams) so --seed reproduces the identical world.
#include "lithokraft/game/wafer_biomes.h"
#include "lithokraft/content/blocks.h"

namespace lithokraft {
namespace {

/// SplitMix64: deterministic 64-bit hash for the dither.
uint64_t splitmix64(uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  uint64_t z = x;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

/// Deterministic uniform [0, 1) from a column position and salt.
float hash01(int x, int z, uint64_t salt) {
  const uint64_t h = splitmix64((uint64_t(uint32_t(x)) | (uint64_t(uint32_t(z)) << 32)) ^ salt);
  return float(h >> 40) * (1.0f / 16777216.0f);
}

constexpr uint64_t kScribeSalt = 0x5C81BEull;   ///< scribe-line fade dither
constexpr uint64_t kCrystalSalt = 0xC87541ull;  ///< crystal-accent dither
constexpr uint64_t kPaletteSalt = 0x9A1E7ull;   ///< ground-palette dither

/// Positive modulo for world coordinates (C++ % keeps the sign).
int posMod(int v, int m) {
  const int r = v % m;
  return r < 0 ? r + m : r;
}

/// Per-biome ground palettes, placed by BlockId (the new biome surface blocks 46/49-51).
/// Runs after the JSON LayeredSurface and dithers the top block by the column's blend
/// weights (own salt, independent of the LayeredSurface dither): the boundary is a noisy
/// gradient, not a line. Palette:
///  - TraceForest     -> kWaferSilicon (1, default silicon)
///  - DicingPlains    -> kScribeSilicon (49, die-grid texture)
///  - CapacitorGroves -> kDenseSilicon (50, dark metallic)
///  - ResonatorSpires -> kFleckedSilicon (51, quartz-flecked)
///  - DeadZones       -> kDeadSilicon (46, grey, unpowered); also remaps the contiguous
///    stone run below the top (the JSON "stone" filler/crust) so dead ground reads solid.
/// Deterministic: seeded hash of the column position against the weights, no RNG.
class WaferPaletteSurface final : public litholattice::procgen::ISurfaceRule {
 public:
  void apply(litholattice::procgen::StageContext& ctx) const override {
    const int n = ctx.chunk.size;
    const size_t nb = ctx.maps.biomeCount;
    for (int z = 0; z < n; ++z)
      for (int x = 0; x < n; ++x) {
        const size_t i = size_t(z * n + x);
        const int h = ctx.maps.height[i];
        if (h < 0) continue;
        // Weight-proportioned biome pick (cumulative walk over the weight vector).
        uint8_t picked = ctx.maps.biome[i];
        if (nb > 1 && ctx.maps.biomeWeight.size() >= (i + 1) * nb) {
          const float t = hash01(ctx.chunk.worldX(x), ctx.chunk.worldZ(z), kPaletteSalt);
          const float* w = ctx.maps.biomeWeight.data() + i * nb;
          float acc = 0.f;
          for (size_t b = 0; b < nb; ++b) {
            acc += w[b];
            if (t < acc) {
              picked = uint8_t(b);
              break;
            }
          }
        }
        using litholattice::procgen::MaterialId;
        if (picked == uint8_t(WaferBiome::DeadZones)) {
          // Solid dead cap: the top plus the 5 filler blocks become dead silicon, then
          // any stone crust below follows. Dead ground reads grey all the way down
          // through the biome skin; deeper geology stays the dominant biome's material.
          for (int y = h; y >= h - 5 && y >= 0; --y)
            ctx.blocks.set(x, y, z, MaterialId(kDeadSilicon));
          for (int y = h - 6; y >= 0; --y) {
            if (ctx.blocks.get(x, y, z) != MaterialId(kStone)) break;
            ctx.blocks.set(x, y, z, MaterialId(kDeadSilicon));
          }
          continue;
        }
        BlockId top = kWaferSilicon;
        if (picked == uint8_t(WaferBiome::DicingPlains)) top = kScribeSilicon;
        if (picked == uint8_t(WaferBiome::CapacitorGroves)) top = kDenseSilicon;
        if (picked == uint8_t(WaferBiome::ResonatorSpires)) top = kFleckedSilicon;
        ctx.blocks.set(x, h, z, MaterialId(top));
      }
  }
};

/// Dicing Plains: the die-grid scribe lines. 1-wide lines every kScribeSpacing blocks in
/// world space (seamless across chunk borders), scored 1 block deep by lifting the top
/// block. Weight-aware: the grid fades with the plains weight at blend borders.
/// Subtle for now; the art pass comes later.
class ScribeLineSurface final : public litholattice::procgen::ISurfaceRule {
 public:
  void apply(litholattice::procgen::StageContext& ctx) const override {
    const int n = ctx.chunk.size;
    for (int z = 0; z < n; ++z)
      for (int x = 0; x < n; ++x) {
        const size_t i = size_t(z * n + x);
        const float plainsW = biomeWeightAt(ctx.maps, i, WaferBiome::DicingPlains);
        if (plainsW <= 0.f) continue;
        const int wx = ctx.chunk.worldX(x), wz = ctx.chunk.worldZ(z);
        if (posMod(wx, kScribeSpacing) != 0 && posMod(wz, kScribeSpacing) != 0) continue;
        if (hash01(wx, wz, kScribeSalt) >= plainsW) continue;
        const int h = ctx.maps.height[i];
        if (h <= 0) continue;
        ctx.blocks.set(x, h, z, litholattice::procgen::kAirMaterial);
        ctx.maps.height[i] = h - 1;  // keep the height map honest for features
      }
  }
};

/// Resonator Spires: sparse crystal accents on the silicon surface (~4% of columns at full
/// spire weight, scaled by the weight at blend borders). Deterministic hash of world
/// coordinates, no RNG stream to keep feature seeds stable.
class CrystalAccentSurface final : public litholattice::procgen::ISurfaceRule {
 public:
  void apply(litholattice::procgen::StageContext& ctx) const override {
    const int n = ctx.chunk.size;
    for (int z = 0; z < n; ++z)
      for (int x = 0; x < n; ++x) {
        const size_t i = size_t(z * n + x);
        const float spireW = biomeWeightAt(ctx.maps, i, WaferBiome::ResonatorSpires);
        if (spireW <= 0.f) continue;
        const int h = ctx.maps.height[i];
        if (h < 0) continue;
        if (hash01(ctx.chunk.worldX(x), ctx.chunk.worldZ(z), kCrystalSalt) < 0.04f * spireW)
          ctx.blocks.set(x, h, z, litholattice::procgen::MaterialId(kResonatorCrystal));
      }
  }
};

}  // namespace

std::vector<std::unique_ptr<litholattice::procgen::ISurfaceRule>> waferSurfaceRules() {
  std::vector<std::unique_ptr<litholattice::procgen::ISurfaceRule>> rules;
  rules.push_back(std::make_unique<WaferPaletteSurface>());
  rules.push_back(std::make_unique<ScribeLineSurface>());
  rules.push_back(std::make_unique<CrystalAccentSurface>());
  return rules;
}

}  // namespace lithokraft
