// GPU texture array behind an interface: backends implement it (gl33, Vulkan); the rest of the
// engine only hands it a TextureRegistry. Layer i = TextureId{i}; palettes become rows of a
// separate palette texture (row = PaletteId).
#pragma once
#include "litholattice/assets/texture_registry.h"

namespace litholattice {

class ITextureArray {
 public:
  virtual ~ITextureArray() = default;
  /// Replaces the contents with every layer and palette of `textures` (main / render thread).
  virtual bool upload(const TextureRegistry& textures) = 0;
  virtual int layerCount() const = 0;
  virtual int tileSize() const = 0;
};

}  // namespace litholattice
