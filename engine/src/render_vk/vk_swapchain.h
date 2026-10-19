// Swapchain with its image views, a shared depth buffer and one framebuffer per image. Rebuilt
// as a whole on resize (the render pass and formats stay). Private to render_vk.
#pragma once
#include <memory>
#include <optional>
#include <vector>
#include "vk_resources.h"

namespace litholattice::vk {

/// Formats and present mode chosen once per device/surface; the render pass is built from them.
struct SurfaceConfig {
  VkSurfaceFormatKHR color{};
  VkFormat depth = VK_FORMAT_UNDEFINED;
  VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
};

/// Picks B8G8R8A8/R8G8B8A8 UNORM (matching the OpenGL backend's non-sRGB output), a depth format
/// and FIFO (vsync) or MAILBOX / IMMEDIATE. nullopt if the surface offers nothing usable.
std::optional<SurfaceConfig> chooseSurfaceConfig(const VulkanDevice& dev, bool vsync, const std::string& forced = {});
const char* presentModeName(VkPresentModeKHR m);

class Swapchain {
 public:
  /// Construction token: only Swapchain's own factory can make one, so it can use make_unique / make_shared.
  class Passkey {
    friend class Swapchain;
    Passkey() = default;
  };
  explicit Swapchain(Passkey) {}
  /// Creates a swapchain of about `width` x `height` (clamped to the surface limits). `old` is
  /// handed to the driver for reuse and may be null. nullptr on failure.
  static std::unique_ptr<Swapchain> create(const VulkanDevice& dev, const SurfaceConfig& cfg, VkRenderPass pass,
                                           uint32_t width, uint32_t height, const Swapchain* old);

  VkSwapchainKHR handle() const { return swapchain_.get(); }
  VkExtent2D extent() const { return extent_; }
  uint32_t imageCount() const { return uint32_t(framebuffers_.size()); }
  VkFramebuffer framebuffer(uint32_t image) const { return framebuffers_[image].get(); }
  /// Raw swapchain image (for the screenshot image-to-host copy).
  VkImage image(uint32_t i) const { return images_[i]; }
  /// True when the swapchain images were created with TRANSFER_SRC usage
  /// (the screenshot copy path); false on drivers that do not allow it.
  bool supportsTransferSrc() const { return transferSrc_; }

 private:
  UniqueSwapchain swapchain_;
  std::vector<UniqueImageView> views_;
  std::optional<Image> depth_;
  std::vector<UniqueFramebuffer> framebuffers_;  // declared last: destroyed first
  std::vector<VkImage> images_;  ///< raw swapchain images (owned by the swapchain)
  bool transferSrc_ = false;     ///< images carry VK_IMAGE_USAGE_TRANSFER_SRC_BIT
  VkExtent2D extent_{};
};

}  // namespace litholattice::vk
