// Private to render_vk: ITextureArray as a mipmapped 2D-array image (block layers) plus a 2D
// palette image (one row per palette), each with its sampler, and a helper for small 2D images.
#pragma once
#include <functional>
#include <optional>
#include "vk_resources.h"
#include "litholattice/render/texture_array.h"

namespace litholattice::vk {

/// Uploads a single-mip 2D image (`bytesPerPixel` 1 or 4) through a staging buffer and leaves it
/// in SHADER_READ_ONLY_OPTIMAL. nullopt on failure.
std::optional<Image> uploadImage2D(const VulkanDevice& dev, VkCommandPool pool, VkFormat format, uint32_t width,
                                   uint32_t height, const uint8_t* pixels, uint32_t bytesPerPixel);

/// Nearest-filter, clamp-to-edge sampler (palettes, font).
UniqueSampler createNearestSampler(const VulkanDevice& dev);

class VkTextureArray final : public ITextureArray {
 public:
  /// `onReplaced` runs after every successful upload (device idle): rebind the new image views.
  VkTextureArray(const VulkanDevice& dev, VkCommandPool pool, std::function<void()> onReplaced);

  /// Waits for the device to go idle, then replaces both images. Keeps the old ones on failure.
  bool upload(const TextureRegistry& textures) override;
  int layerCount() const override { return layers_; }
  int tileSize() const override { return tile_; }

  VkDescriptorImageInfo arrayDescriptor() const;
  VkDescriptorImageInfo paletteDescriptor() const;

 private:
  std::optional<Image> uploadArray(const TextureRegistry& textures, uint32_t mipLevels) const;

  const VulkanDevice& dev_;
  VkCommandPool pool_;
  std::function<void()> onReplaced_;
  UniqueSampler arraySampler_, paletteSampler_;
  std::optional<Image> array_, palette_;
  int layers_ = 0, tile_ = 0;
};

}  // namespace litholattice::vk
