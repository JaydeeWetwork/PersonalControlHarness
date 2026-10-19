// texture_recipe.h: data-driven procedural texture recipes.
//
// A recipe is an ordered list of nodes, authored as line-based text (one node
// per line, `name key=value key=value ...`) so player/mod tools can write
// recipes without touching C++. Generation (texture_generator.h) evaluates the
// nodes in order into a float field stack:
//
//   - generator nodes (noise / voronoi / grid / constant) PUSH a field whose
//     values are normalised to [0,1]; they sample at the current (possibly
//     domain-warped) coordinates.
//   - warp accumulates a per-pixel domain offset used by later generators.
//   - modifier nodes (ridged / billow / curve) transform the top field.
//   - blend pops the top two fields, combines them, and pushes the result.
//   - palette maps the top field through an RGBA gradient into an image.
//   - glow / dither are image post steps (they run after palette; if no
//     palette ran yet, a greyscale palette is applied implicitly first).
//
// Text round-trip is exact: recipeToText(parseRecipe(t)) generates identical
// pixels to parseRecipe(t) (floats are printed with %.9g, enough digits to
// round-trip a float exactly).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace litholattice {

struct TextureColor {
  uint8_t r = 0, g = 0, b = 0, a = 255;
  bool operator==(const TextureColor&) const = default;
};

struct GradientStop {
  float pos = 0.0f;  // 0..1 along the field value
  TextureColor color{};
  bool operator==(const GradientStop&) const = default;
};

enum class TextureNodeType {
  Noise,     // tileable FBM noise (value or gradient/Perlin-style)
  Voronoi,   // tileable cellular noise (F1 or F2-F1 edges)
  Warp,      // domain-warp the coordinates for later generators
  Ridged,    // top field: v -> 1 - |2v-1|
  Billow,    // top field: v -> (2v-1)^2
  Curve,     // top field: invert/gamma/contrast/posterize/clamp (aka remap)
  Blend,     // combine top two fields
  Grid,      // axis-aligned grid-lines field
  Constant,  // push a uniform field
  Palette,   // field -> RGBA image through a gradient
  Glow,      // post: brighten bright pixels toward a glow colour
  Dither,    // post: ordered Bayer 4x4 dither
};

enum class NoiseKind { Value, Gradient };
enum class VoronoiMode { F1, Edge };  // Edge = F2-F1 (cracks)
enum class BlendMode { Multiply, Add, Min, Max, Screen };

struct TextureNode {
  TextureNodeType type = TextureNodeType::Noise;

  // Shared generator parameters.
  float scale = 4.0f;       // cells across the tile (noise / voronoi / warp)
  int octaves = 4;          // noise / warp FBM octaves
  float lacunarity = 2.0f;  // noise / warp
  float gain = 0.5f;        // noise / warp
  int seedOffset = 0;       // per-node seed salt (decorrelates stacked nodes)

  NoiseKind noiseKind = NoiseKind::Gradient;   // noise
  VoronoiMode voronoiMode = VoronoiMode::F1;   // voronoi
  BlendMode blendMode = BlendMode::Multiply;   // blend
  float mix = 1.0f;                            // blend: 0 = lower field, 1 = full combine

  float strength = 0.15f;  // warp: offset amplitude, in tile units

  // curve (remap)
  float contrast = 1.0f;
  float gamma = 1.0f;
  int posterize = 0;  // steps; 0 = off
  bool invert = false;
  float clampMin = 0.0f;
  float clampMax = 1.0f;

  // grid
  int cells = 8;
  float lineWidth = 0.08f;  // fraction of a cell
  float sharpness = 1.0f;   // falloff exponent

  float value = 0.0f;  // constant

  // palette
  std::vector<GradientStop> stops;

  // glow
  float threshold = 0.8f;
  float intensity = 0.8f;
  TextureColor glowColor{255, 204, 102, 255};

  // dither
  float amount = 16.0f;  // offset amplitude in 0..255 units

  bool operator==(const TextureNode&) const = default;
};

/// Parser caps (P3-3, Appendix E M2): a recipe is a future player/mod
/// authoring surface, so hostile text must not be able to force unbounded
/// work or memory. Every generator node stacks a size x size float field
/// (up to 64 MiB at the 4096 generation cap), so node count is capped;
/// palette stops are capped per node; the input text itself is capped.
inline constexpr size_t kMaxRecipeNodes = 256;
inline constexpr size_t kMaxPaletteStops = 64;
inline constexpr size_t kMaxRecipeTextBytes = size_t(1) << 20;  // 1 MiB of recipe text

struct TextureRecipe {
  std::vector<TextureNode> nodes;
  bool operator==(const TextureRecipe&) const = default;
};

/// Parses the line-based recipe text. On failure returns false and, when
/// `err` is non-null, fills it with "line N: ..." describing the problem.
/// Blank lines and lines starting with '#' are ignored.
bool parseRecipe(std::string_view text, TextureRecipe* out, std::string* err);

/// Canonical text form of a recipe (one node per line). Reparsing it yields
/// a recipe that generates byte-identical pixels.
std::string recipeToText(const TextureRecipe& recipe);

}  // namespace litholattice
