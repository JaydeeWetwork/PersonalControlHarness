#include "gl_texture_array.h"
#include <algorithm>
#include <vector>
#include "litholattice/core/log.h"

namespace litholattice::gl33 {

GlTextureArray::~GlTextureArray() {
  if (array_) glDeleteTextures(1, &array_);
  if (palette_) glDeleteTextures(1, &palette_);
}

bool GlTextureArray::upload(const TextureRegistry& textures) {
  if (!array_) glGenTextures(1, &array_);
  tile_ = textures.tileSize();
  layers_ = int(textures.textures().size());
  glBindTexture(GL_TEXTURE_2D_ARRAY, array_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, tile_, tile_, layers_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  for (int i = 0; i < layers_; ++i) {
    const Image& img = textures.textures()[size_t(i)].image;
    // Tile-geometry check (P3-3, Appendix D L1): glTexSubImage3D below
    // uploads tile_ x tile_ texels from the image's buffer unconditionally.
    // A smaller image would be an out-of-bounds host read fed to GL; a
    // larger one a silent crop. TextureRegistry is expected to guarantee
    // tile-sized images, but this upload is the last line of defence:
    // skip + log a mismatched layer (it stays zero-filled = black, and
    // the registry's own missing-texture path normally prevents this).
    if (img.width() != tile_ || img.height() != tile_) {
      logError("gl33: texture layer %d is %dx%d, expected %dx%d - layer left empty",
               i, img.width(), img.height(), tile_, tile_);
      continue;
    }
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, tile_, tile_, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                    img.pixels().data());
  }
  glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);  // crisp pixel art up close
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  // Palettes: row = PaletteId, padded with transparent black to the longest palette.
  const int w = textures.paletteWidth(), h = std::max<int>(1, int(textures.palettes().size()));
  std::vector<uint8_t> px(size_t(w) * size_t(h) * 4, 0);
  for (size_t r = 0; r < textures.palettes().size(); ++r) {
    const auto& colors = textures.palettes()[r].colors;
    for (size_t x = 0; x < colors.size(); ++x)
      for (size_t c = 0; c < 4; ++c) px[(r * size_t(w) + x) * 4 + c] = colors[x][c];
  }
  if (!palette_) glGenTextures(1, &palette_);
  glBindTexture(GL_TEXTURE_2D, palette_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);
  return glGetError() == 0;
}

void GlTextureArray::bind() const {
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D_ARRAY, array_);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, palette_);
  glActiveTexture(GL_TEXTURE0);
}

}  // namespace litholattice::gl33
