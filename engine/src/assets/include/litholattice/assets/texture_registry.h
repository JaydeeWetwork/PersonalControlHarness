// TextureRegistry: every texture, animation frame, palette and mask a pack set provides, under
// stable handles. A TextureId is the texture-array layer the GPU side uploads it to; id 0 is
// always the magenta "missing" checker, so an unknown name never breaks rendering.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "litholattice/assets/image.h"

namespace litholattice {

/// Texture-array layer. 0 = missing texture.
struct TextureId {
  uint16_t value = 0;
  bool missing() const { return value == 0; }
  auto operator<=>(const TextureId&) const = default;
};
/// Palette row; kNone = no palette.
struct PaletteId {
  static constexpr uint16_t kNone = 0xFFFF;
  uint16_t value = kNone;
  bool valid() const { return value != kNone; }
  auto operator<=>(const PaletteId&) const = default;
};
/// Animation index; kNone = not animated.
struct AnimationId {
  static constexpr uint16_t kNone = 0xFFFF;
  uint16_t value = kNone;
  bool valid() const { return value != kNone; }
  auto operator<=>(const AnimationId&) const = default;
};

struct Texture {
  std::string name;    ///< unique; animation frames are "<animation>#<frame>"
  std::string origin;  ///< pack the pixels came from ("" for the built-in missing texture)
  Image image;         ///< tile x tile
};

struct Palette {
  std::string name, origin;
  std::vector<std::array<uint8_t, 4>> colors;  ///< RGBA, index = shader lookup (e.g. strength)
};

/// Frame strip + timing (from the strip's .png.json sidecar or inline in resources.json).
struct AnimationTiming {
  int frameMs = 50;
  bool loop = true;
  std::vector<int> sequence;  ///< frame order; empty = 0..frames-1
};

struct AnimatedTexture {
  std::string name, origin;
  TextureId firstFrame;  ///< frames occupy consecutive layers
  int frameCount = 0;
  AnimationTiming timing;
};

class TextureRegistry {
 public:
  static constexpr int kMaxLayers = 256;  ///< chunk vertices carry an 8-bit layer

  /// Starts with the missing texture at id 0, sized `tileSize`.
  explicit TextureRegistry(int tileSize = 16);

  int tileSize() const { return tile_; }

  /// Adds a tile-sized texture. Returns nullopt (+ `err`) on a size mismatch, a duplicate name or
  /// a full array; the name is then remembered as missing so lookups resolve to id 0.
  std::optional<TextureId> add(std::string name, Image image, std::string origin, std::string* err);
  /// Splits a vertical strip (tile wide, n * tile tall) into n consecutive layers.
  std::optional<AnimationId> addAnimation(std::string name, const Image& strip, AnimationTiming timing,
                                          std::string origin, std::string* err);
  /// Row 0 of `image` becomes the palette (one colour per pixel).
  PaletteId addPalette(std::string name, const Image& image, std::string origin);

  /// Id by name; nullopt if never defined. A defined-but-failed name returns the missing id.
  std::optional<TextureId> find(std::string_view name) const;
  /// Id by name, missing (0) if unknown.
  TextureId id(std::string_view name) const { return find(name).value_or(TextureId{}); }
  std::optional<PaletteId> findPalette(std::string_view name) const;
  std::optional<AnimationId> findAnimation(std::string_view name) const;

  const Texture& texture(TextureId id) const { return textures_[id.value]; }
  const Palette& palette(PaletteId id) const { return palettes_[id.value]; }
  const AnimatedTexture& animation(AnimationId id) const { return animations_[id.value]; }
  std::span<const Texture> textures() const { return textures_; }
  std::span<const Palette> palettes() const { return palettes_; }
  std::span<const AnimatedTexture> animations() const { return animations_; }
  /// Longest palette (GPU palette texture width).
  int paletteWidth() const;
  /// Names that were defined but failed to load (they render as the missing texture).
  std::span<const std::string> missingNames() const { return missing_; }

 private:
  int tile_;
  std::vector<Texture> textures_;
  std::vector<Palette> palettes_;
  std::vector<AnimatedTexture> animations_;
  std::unordered_map<std::string, uint16_t> byName_, paletteByName_, animByName_;
  std::vector<std::string> missing_;
};

/// What changed between two registries (hot reload log).
struct RegistryDiff {
  std::vector<std::string> added, removed, changed;
  bool empty() const { return added.empty() && removed.empty() && changed.empty(); }
  /// "3 changed (a, b, c), 1 added (d)" or "no texture changes".
  std::string summary(size_t maxNames = 8) const;
};
RegistryDiff diffRegistries(const TextureRegistry& before, const TextureRegistry& after);

}  // namespace litholattice
