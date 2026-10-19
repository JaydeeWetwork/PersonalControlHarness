// Headless Vulkan check for --selftest: loader present, bare instance, physical devices listed.
#include <vector>
#include "vk_device.h"
#include "vk_handles.h"
#include "litholattice/render_vk/vulkan_renderer.h"

namespace litholattice {

VulkanProbe probeVulkan() {
  VulkanProbe probe;
  if (!vk::loadLoader()) {
    probe.summary = "no Vulkan loader on this machine";
    return probe;
  }
  probe.loaderFound = true;
  uint32_t version = VK_API_VERSION_1_0;
  if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&version);
  auto app = vk::make<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
  app.pApplicationName = "LithoLattice selftest";
  app.apiVersion = VK_API_VERSION_1_0;
  auto ci = vk::make<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
  ci.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  const VkResult r = vkCreateInstance(&ci, nullptr, &instance);
  if (r != VK_SUCCESS) {
    probe.summary = std::string("vkCreateInstance failed: ") + vk::resultName(r);
    // VK_ERROR_INCOMPATIBLE_DRIVER = the loader found no driver that can
    // serve a device on this machine (GPU-less box): environmental.
    if (r == VK_ERROR_INCOMPATIBLE_DRIVER) probe.noDevices = true;
    return probe;
  }
  probe.summary = "loader " + vk::versionString(version);
  if (vk::loadInstanceFunctions(instance, false)) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    if (n) vkEnumeratePhysicalDevices(instance, &n, devices.data());
    for (VkPhysicalDevice d : devices) {
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(d, &p);
      probe.summary += std::string(", ") + p.deviceName + " (Vulkan " + vk::versionString(p.apiVersion) + ")";
    }
    probe.ok = n > 0;
    if (!n) { probe.summary += ", no physical devices"; probe.noDevices = true; }
  } else {
    probe.summary += ", instance entry points missing";
  }
  vkDestroyInstance(instance, nullptr);
  return probe;
}

}  // namespace litholattice
