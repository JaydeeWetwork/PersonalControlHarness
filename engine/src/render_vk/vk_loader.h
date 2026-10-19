// Run-time Vulkan loader: opens the system loader library (vulkan-1.dll / libvulkan.so.1, which
// ships with the GPU driver) and fetches the entry points the backend uses into global function
// pointers with the standard names. Nothing links against Vulkan, so the game still starts on a
// machine without it. Private to render_vk; include this instead of <vulkan/vulkan.h>.
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

// Entry points by level. Add a function here to make it available everywhere in the backend.
#define LITHOLATTICE_VK_GLOBAL_FUNCTIONS(X) \
  X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties)

#define LITHOLATTICE_VK_INSTANCE_FUNCTIONS(X)                                                                         \
  X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                        \
  X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties)                         \
  X(vkGetPhysicalDeviceFormatProperties) X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice)          \
  X(vkGetDeviceProcAddr)

// VK_KHR_surface (instance extension): only resolvable when the extension is enabled.
#define LITHOLATTICE_VK_SURFACE_FUNCTIONS(X)                                                                          \
  X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
  X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define LITHOLATTICE_VK_DEVICE_FUNCTIONS(X)                                                                           \
  X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle)           \
  X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR)      \
  X(vkQueuePresentKHR) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory)                                   \
  X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory)              \
  X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)                  \
  X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler)                        \
  X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer)              \
  X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout)    \
  X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateDescriptorSetLayout)                         \
  X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool)                     \
  X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkDestroyCommandPool)     \
  X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer)        \
  X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore)              \
  X(vkDestroySemaphore) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline)                 \
  X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed)         \
  X(vkCmdDraw) X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdPipelineBarrier)        \
  X(vkCmdCopyBufferToImage) X(vkCmdBlitImage) X(vkCmdCopyBuffer) X(vkCmdCopyImageToBuffer)

#define LITHOLATTICE_VK_DECLARE(name) extern PFN_##name name;
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
extern PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion;  // null on a Vulkan 1.0 loader
extern PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT;    // null without debug utils
extern PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT;  // null without debug utils
LITHOLATTICE_VK_GLOBAL_FUNCTIONS(LITHOLATTICE_VK_DECLARE)
LITHOLATTICE_VK_INSTANCE_FUNCTIONS(LITHOLATTICE_VK_DECLARE)
LITHOLATTICE_VK_SURFACE_FUNCTIONS(LITHOLATTICE_VK_DECLARE)
LITHOLATTICE_VK_DEVICE_FUNCTIONS(LITHOLATTICE_VK_DECLARE)
#undef LITHOLATTICE_VK_DECLARE

namespace litholattice::vk {

/// Opens the system Vulkan loader (once; it stays loaded for the life of the process) and fetches
/// the global entry points. False if there is no loader on this machine.
bool loadLoader();
/// Fetches the instance-level entry points (debug utils stay null unless the extension is on).
/// `withSurface`: the instance has VK_KHR_surface enabled and its functions are required too.
/// False if a required one is missing.
bool loadInstanceFunctions(VkInstance instance, bool withSurface);
/// Fetches the device-level entry points. False if a required one is missing.
bool loadDeviceFunctions(VkDevice device);

}  // namespace litholattice::vk
