// RAII owners for Vulkan device-level handles, plus result checking. Private to render_vk.
#pragma once
#include <utility>
#include "vk_loader.h"

namespace litholattice::vk {

/// Owns one handle created from `VkDevice`; destroys it with `Destroy` (vkDestroyX / vkFreeMemory).
/// Move-only. Destroy is a reference to the loaded function pointer, read at destruction time.
template <typename Handle, auto& Destroy>
class DeviceHandle {
 public:
  DeviceHandle() = default;
  DeviceHandle(VkDevice device, Handle handle) : device_(device), handle_(handle) {}
  DeviceHandle(DeviceHandle&& o) noexcept : device_(o.device_), handle_(std::exchange(o.handle_, Handle{})) {}
  DeviceHandle& operator=(DeviceHandle&& o) noexcept {
    if (this != &o) {
      reset();
      device_ = o.device_;
      handle_ = std::exchange(o.handle_, Handle{});
    }
    return *this;
  }
  DeviceHandle(const DeviceHandle&) = delete;
  DeviceHandle& operator=(const DeviceHandle&) = delete;
  ~DeviceHandle() { reset(); }

  void reset() {
    if (handle_ != Handle{}) Destroy(device_, std::exchange(handle_, Handle{}), nullptr);
  }
  Handle get() const { return handle_; }
  explicit operator bool() const { return handle_ != Handle{}; }

 private:
  VkDevice device_ = VK_NULL_HANDLE;
  Handle handle_{};
};

using UniqueBuffer = DeviceHandle<VkBuffer, vkDestroyBuffer>;
using UniqueMemory = DeviceHandle<VkDeviceMemory, vkFreeMemory>;
using UniqueImage = DeviceHandle<VkImage, vkDestroyImage>;
using UniqueImageView = DeviceHandle<VkImageView, vkDestroyImageView>;
using UniqueSampler = DeviceHandle<VkSampler, vkDestroySampler>;
using UniqueRenderPass = DeviceHandle<VkRenderPass, vkDestroyRenderPass>;
using UniqueFramebuffer = DeviceHandle<VkFramebuffer, vkDestroyFramebuffer>;
using UniqueShaderModule = DeviceHandle<VkShaderModule, vkDestroyShaderModule>;
using UniquePipelineLayout = DeviceHandle<VkPipelineLayout, vkDestroyPipelineLayout>;
using UniquePipeline = DeviceHandle<VkPipeline, vkDestroyPipeline>;
using UniqueDescriptorSetLayout = DeviceHandle<VkDescriptorSetLayout, vkDestroyDescriptorSetLayout>;
using UniqueDescriptorPool = DeviceHandle<VkDescriptorPool, vkDestroyDescriptorPool>;
using UniqueCommandPool = DeviceHandle<VkCommandPool, vkDestroyCommandPool>;
using UniqueFence = DeviceHandle<VkFence, vkDestroyFence>;
using UniqueSemaphore = DeviceHandle<VkSemaphore, vkDestroySemaphore>;
using UniqueSwapchain = DeviceHandle<VkSwapchainKHR, vkDestroySwapchainKHR>;

/// A zero-initialized Vulkan info struct with its sType set.
template <typename T>
T make(VkStructureType type) {
  T info{};
  info.sType = type;
  return info;
}

/// Name of a VkResult for log lines ("VK_ERROR_DEVICE_LOST", ...).
const char* resultName(VkResult r);

/// True on VK_SUCCESS; otherwise logs "<what> failed: <result>" and returns false.
bool check(VkResult r, const char* what);

}  // namespace litholattice::vk
