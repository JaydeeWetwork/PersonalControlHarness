// Private to the gl33 backend: ITextureArray as a GL_TEXTURE_2D_ARRAY (block layers, mipmapped)
// plus a GL_TEXTURE_2D palette texture (one row per palette).
#pragma once
#include "gl_loader.h"
#include "litholattice/render/texture_array.h"

namespace litholattice::gl33 {

class GlTextureArray final : public ITextureArray {
 public:
  GlTextureArray() = default;
  ~GlTextureArray() override;
  GlTextureArray(const GlTextureArray&) = delete;
  GlTextureArray& operator=(const GlTextureArray&) = delete;

  bool upload(const TextureRegistry& textures) override;
  int layerCount() const override { return layers_; }
  int tileSize() const override { return tile_; }

  /// Binds the array to texture unit 0 and the palettes to unit 1.
  void bind() const;

 private:
  GLuint array_ = 0, palette_ = 0;
  int layers_ = 0, tile_ = 0;
};

}  // namespace litholattice::gl33
