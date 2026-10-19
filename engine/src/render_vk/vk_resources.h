// GPU memory-backed resources for the Vulkan backend: buffers (optionally persistently mapped),
// images with a view, and one-shot command submission for uploads. Private to render_vk.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include "vk_device.h"
#include "vk_handles.h"

namespace litholattice::vk {

/// A buffer with its own memory. Host-visible buffers stay mapped for their whole lifetime.
class Buffer {
 public:
  Buffer() = default;
  /// Returns nullopt (with a log line) if no memory type has `required`.
  static std::optional<Buffer> create(const VulkanDevice& dev, VkDeviceSize size, VkBufferUsageFlags usage,
                                      VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0);
  VkBuffer handle() const { return buffer_.get(); }
  VkDeviceSize size() const { return size_; }
  /// Mapped pointer (host-visible buffers only, else nullptr).
  uint8_t* mapped() const { return mapped_; }

 private:
  UniqueMemory memory_;  // declared first: destroyed after the buffer
  UniqueBuffer buffer_;
  uint8_t* mapped_ = nullptr;
  VkDeviceSize size_ = 0;
};

/// A device-local image with one view over all its mip levels and layers.
class Image {
 public:
  Image() = default;
  static std::optional<Image> create(const VulkanDevice& dev, const VkImageCreateInfo& info, VkImageViewType viewType,
                                     VkImageAspectFlags aspect);
  VkImage handle() const { return image_.get(); }
  VkImageView view() const { return view_.get(); }

 private:
  UniqueMemory memory_;
  UniqueImage image_;
  UniqueImageView view_;
};

/// Records commands with `record` into a temporary command buffer, submits it and waits for the
/// queue (uploads at load time only, never per frame).
bool submitOnce(const VulkanDevice& dev, VkCommandPool pool, const std::function<void(VkCommandBuffer)>& record);

/// Image layout transition barrier for all mips and layers of `image` (colour aspect).
void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                     VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                     VkPipelineStageFlags dstStage, uint32_t baseMip = 0,
                     uint32_t mipCount = VK_REMAINING_MIP_LEVELS);

}  // namespace litholattice::vk
