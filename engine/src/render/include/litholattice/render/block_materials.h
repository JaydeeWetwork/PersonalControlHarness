// BlockMaterials: block models (assets) resolved against the block registry (world) into what
// the mesher and the chunk shader need: a texture-array layer + UV rect per block face and
// state, wire pieces, shader bindings (mask layers, palette rows) and animation tables.
// Immutable once built; shared read-only by mesh worker threads.
#pragma once
#include <map>
#include <string>
#include <string_view>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "litholattice/assets/block_model.h"
#include "litholattice/assets/texture_registry.h"
#include "litholattice/world/block.h"

namespace litholattice {

struct FaceMaterial {
  static constexpr uint8_t kNotAnimated = 0xFF;
  uint8_t layer = 0;  ///< texture-array layer (first frame when animated)
  UvRect uv;
  uint8_t animation = kNotAnimated;  ///< index into BlockMaterials::animations()
  bool animated() const { return animation != kNotAnimated; }
};

/// Chunk-shader uniforms that come from the block models (layers and palette rows).
/// Filled by the game's IBlockAppearance::bindMaterials while the materials are built.
struct MaterialBindings {
  std::array<int, 2> stripPalette{}, stripRing{};           ///< kShadeStripTint0 / 1: palette row, ring mask layer
  std::array<int, 3> tierPalette{}, tierMask{}, tierRim{};  ///< kShadePalette16 / 32 / 64: palette row, masks
  int pipMask = 0, cellMask = 0, debugDigits = 0; std::array<int, 2> stripDot{};  /* dot masks; DEBUG digit tile */                            ///< kShadePips / kShadeCells mask layers
};
class IBlockAppearance;

/// One animation as the shader plays it: frames sequence[start .. start + length).
struct ShaderAnimation {
  int sequenceStart = 0, sequenceLength = 0;
  float frameMs = 50.f;
};

class BlockMaterials {
 public:
  static constexpr size_t kMaxAnimations = 8, kMaxAnimationFrames = 64;  ///< chunk shader limits

  /// Every face of the first `blockCount` blocks shows the missing texture (no pack loaded).
  /// Ids past the table also read as missing, so a default-built table is always safe.
  explicit BlockMaterials(size_t blockCount = 0);
  /// Resolves `models` (by BlockDef::tex of every block in `blocks`) against `textures`. Blocks
  /// without a model and unusable entries are reported in `warnings` and render as the missing
  /// texture. `appearance` (optional) fills the shader bindings from the models.
  static BlockMaterials build(const BlockRegistry& blocks, const TextureRegistry& textures, const BlockModelSet& models,
                              std::vector<std::string>* warnings, const IBlockAppearance* appearance = nullptr);

  const FaceMaterial& face(BlockId b, FaceSlot f, bool active) const {
    return b < faces_.size() ? faces_[b][size_t(f)][active ? 1 : 0] : kMissing;
  }
  const FaceMaterial& wirePiece(BlockId b, WirePiece p) const { return b < wire_.size() ? wire_[b][size_t(p)] : kMissing; }
  const MaterialBindings& bindings() const { return bindings_; }
  /// Any loaded texture by name as a static face (overlays, connected variants, masks drawn as
  /// quads); nullptr if the packs have no such texture.
  const FaceMaterial* named(std::string_view name) const {
    const auto it = named_.find(name);
    return it == named_.end() ? nullptr : &it->second;
  }
  std::span<const ShaderAnimation> animations() const { return animations_; }
  std::span<const int> animationFrames() const { return frames_; }
  /// HUD swatch colour (mean colour of the active top face; wires: palette at full strength).
  const std::array<float, 3>& swatch(BlockId b) const { return b < swatch_.size() ? swatch_[b] : kMissingSwatch; }

  /// Swatch of a block that is not in the table (magenta).
  static constexpr std::array<float, 3> kMissingSwatch{1.f, 0.f, 1.f};

 private:
  static constexpr FaceMaterial kMissing{};
  using StateFaces = std::array<FaceMaterial, 2>;  // [base, active]
  std::vector<std::array<StateFaces, kFaceSlotCount>> faces_;
  std::vector<std::array<FaceMaterial, kWirePieceCount>> wire_;
  std::vector<std::array<float, 3>> swatch_;
  MaterialBindings bindings_;
  std::vector<ShaderAnimation> animations_;
  std::vector<int> frames_;
  std::map<std::string, FaceMaterial, std::less<>> named_;
};

}  // namespace litholattice
