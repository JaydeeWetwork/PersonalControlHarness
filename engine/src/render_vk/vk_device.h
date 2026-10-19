// VulkanDevice: instance (+ validation messenger), window surface, the chosen physical device
// (discrete GPU preferred), logical device and its graphics+present queue. Private to render_vk.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "vk_loader.h"
#include "litholattice/render_vk/vulkan_renderer.h"

namespace litholattice::vk {

class VulkanDevice {
 public:
  /// Construction token: only VulkanDevice's own factory can make one, so it can use make_unique / make_shared.
  class Passkey {
    friend class VulkanDevice;
    Passkey() = default;
  };
  explicit VulkanDevice(Passkey) {}
  /// Creates everything up to the logical device. Returns nullptr and sets `error` on failure.
  static std::unique_ptr<VulkanDevice> create(const VulkanRendererDesc& desc, std::string* error);
  ~VulkanDevice();
  VulkanDevice(const VulkanDevice&) = delete;
  VulkanDevice& operator=(const VulkanDevice&) = delete;

  VkInstance instance() const { return instance_; }
  VkPhysicalDevice physical() const { return physical_; }
  VkDevice device() const { return device_; }
  VkSurfaceKHR surface() const { return surface_; }
  VkQueue queue() const { return queue_; }
  uint32_t queueFamily() const { return queueFamily_; }
  const VkPhysicalDeviceProperties& properties() const { return properties_; }

  /// Index of a memory type allowed by `typeBits` with all of `required` (and, if possible, all of
  /// `preferred` too). UINT32_MAX if none.
  uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required,
                          VkMemoryPropertyFlags preferred = 0) const;
  bool supportsFormat(VkFormat format, VkFormatFeatureFlags features, bool optimalTiling) const;
  bool supportsVertexFormat(VkFormat format) const;

  /// "GPU name | discrete | Vulkan 1.3.280 | driver 566.36 | instance 1.4.309 | validation on".
  std::string description() const;

 private:
  bool createInstance(const VulkanRendererDesc& desc, std::string* error);
  bool pickPhysicalDevice(std::string* error);
  bool createDevice(std::string* error);

  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  uint32_t instanceVersion_ = VK_API_VERSION_1_0;
  bool validation_ = false;
  VkPhysicalDeviceProperties properties_{};
  VkPhysicalDeviceMemoryProperties memory_{};
};

/// Instance-level version string "1.4.309" from a packed VK_MAKE_API_VERSION value.
std::string versionString(uint32_t packed);

}  // namespace litholattice::vk
