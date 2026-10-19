// CPU texture array: ITextureArray backed by host memory, for the software
// renderer. Layers are stored at the software fragment path's native tile
// size: 16x16 texels, one uint32 per texel packed 0xAARRGGBB — the exact
// layout software_shaders.h samples (texData[layer*256 + ty*16 + tx]).
#pragma once
#include <cstdint>
#include <vector>
#include "litholattice/render/texture_array.h"

namespace litholattice {

class CpuTextureArray final : public ITextureArray {
 public:
  /// The software fragment shaders sample a fixed 16x16 tile per layer,
  /// whatever resolution the source registry carries.
  static constexpr int kTileSize = 16;

  /// Replaces the contents with every layer of `textures`, box-downsampled
  /// to kTileSize x kTileSize (the same bounds/averaging biome_capture
  /// applies when it builds the software texture buffer by hand, so both
  /// feed paths agree texel-for-texel). Palettes are not stored: the
  /// software fragment inputs carry no palette data — strip/palette shading
  /// on this path resolves from the vertex mode/value fields instead.
  bool upload(const TextureRegistry& textures) override;
  int layerCount() const override { return layers_; }
  /// The stored tile resolution (kTileSize), not the source registry's.
  int tileSize() const override { return kTileSize; }

  /// Raw upload behind SoftwareRenderer::setTextures: `layers` tiles of
  /// kTileSize x kTileSize uint32 texels (0xAARRGGBB), copied. Null data or
  /// zero layers clears the array.
  void setRaw(const uint32_t* data, uint32_t layers);

  /// Stored texels for the rasterizer; nullptr when the array is empty.
  const uint32_t* data() const { return texels_.empty() ? nullptr : texels_.data(); }
  uint32_t layers() const { return uint32_t(layers_); }

 private:
  std::vector<uint32_t> texels_;  ///< layers_ * 256 texels, 0xAARRGGBB
  int layers_ = 0;
};

}  // namespace litholattice
