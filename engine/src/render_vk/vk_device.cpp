#include "vk_device.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "vk_handles.h"
#include "litholattice/core/log.h"

namespace litholattice::vk {
namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

VKAPI_ATTR VkBool32 VKAPI_CALL onDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  const char* level = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning";
  logError("vulkan validation %s: %s", level, data && data->pMessage ? data->pMessage : "?");
  return VK_FALSE;
}

template <typename T, typename F>
std::vector<T> enumerate(F&& fill) {
  uint32_t n = 0;
  fill(&n, static_cast<T*>(nullptr));
  std::vector<T> out(n);
  if (n) fill(&n, out.data());
  out.resize(n);
  return out;
}

bool hasLayer(const char* name) {
  const auto layers = enumerate<VkLayerProperties>([](uint32_t* n, VkLayerProperties* p) {
    return vkEnumerateInstanceLayerProperties(n, p);
  });
  return std::any_of(layers.begin(), layers.end(), [&](const VkLayerProperties& l) { return !std::strcmp(l.layerName, name); });
}

bool hasInstanceExtension(const char* name) {
  const auto exts = enumerate<VkExtensionProperties>([](uint32_t* n, VkExtensionProperties* p) {
    return vkEnumerateInstanceExtensionProperties(nullptr, n, p);
  });
  return std::any_of(exts.begin(), exts.end(), [&](const VkExtensionProperties& e) { return !std::strcmp(e.extensionName, name); });
}

const char* typeName(VkPhysicalDeviceType t) {
  switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
  }
}

int typeScore(VkPhysicalDeviceType t) {
  switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 1000;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 500;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 200;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 100;
    default: return 50;
  }
}

/// Vendor-specific driver version encodings (NVIDIA 10.8.8.6 bits, Intel on Windows 18.14).
std::string driverString(const VkPhysicalDeviceProperties& p) {
  const uint32_t v = p.driverVersion;
  char buf[48];
  if (p.vendorID == 0x10DE)
    std::snprintf(buf, sizeof buf, "%u.%02u", (v >> 22) & 0x3FFu, (v >> 14) & 0xFFu);
#if defined(_WIN32)
  else if (p.vendorID == 0x8086)
    std::snprintf(buf, sizeof buf, "%u.%u", v >> 14, v & 0x3FFFu);
#endif
  else
    return versionString(v);
  return buf;
}

}  // namespace

std::string versionString(uint32_t packed) {
  return std::to_string(VK_API_VERSION_MAJOR(packed)) + "." + std::to_string(VK_API_VERSION_MINOR(packed)) + "." +
         std::to_string(VK_API_VERSION_PATCH(packed));
}

std::unique_ptr<VulkanDevice> VulkanDevice::create(const VulkanRendererDesc& desc, std::string* error) {
  if (!loadLoader()) {
    *error = "no Vulkan loader (vulkan-1.dll / libvulkan.so.1) found";
    return nullptr;
  }
  auto d = std::make_unique<VulkanDevice>(Passkey{});
  if (!d->createInstance(desc, error)) return nullptr;
  const uint64_t surface = desc.surface.createSurface ? desc.surface.createSurface(d->instance_) : 0;
  if (!surface) {
    *error = "window surface creation failed";
    return nullptr;
  }
  d->surface_ = reinterpret_cast<VkSurfaceKHR>(surface);
  if (!d->pickPhysicalDevice(error) || !d->createDevice(error)) return nullptr;
  return d;
}

VulkanDevice::~VulkanDevice() {
  if (device_) {
    vkDeviceWaitIdle(device_);
    vkDestroyDevice(device_, nullptr);
  }
  if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
  if (messenger_ && vkDestroyDebugUtilsMessengerEXT) vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
  if (instance_) vkDestroyInstance(instance_, nullptr);
}

bool VulkanDevice::createInstance(const VulkanRendererDesc& desc, std::string* error) {
  if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&instanceVersion_);
  std::vector<const char*> extensions;
  for (const std::string& e : desc.surface.instanceExtensions) extensions.push_back(e.c_str());
  std::vector<const char*> layers;
  validation_ = desc.validation && hasLayer(kValidationLayer);
  if (desc.validation && !validation_) logInfo("vulkan: validation requested but %s is not installed", kValidationLayer);
  const bool debugUtils = validation_ && hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  if (validation_) layers.push_back(kValidationLayer);
  if (debugUtils) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

  auto app = make<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
  app.pApplicationName = "LithoLattice application";
  app.pEngineName = "LithoLattice";
  app.apiVersion = VK_API_VERSION_1_0;  // the backend uses core 1.0 + VK_KHR_swapchain only
  auto ci = make<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
  ci.pApplicationInfo = &app;
  ci.enabledExtensionCount = uint32_t(extensions.size());
  ci.ppEnabledExtensionNames = extensions.data();
  ci.enabledLayerCount = uint32_t(layers.size());
  ci.ppEnabledLayerNames = layers.data();
  const VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
  if (r != VK_SUCCESS) {
    *error = std::string("vkCreateInstance failed: ") + resultName(r);
    return false;
  }
  if (!loadInstanceFunctions(instance_, true)) {
    *error = "instance entry points missing";
    return false;
  }
  if (debugUtils && vkCreateDebugUtilsMessengerEXT) {
    auto mi = make<VkDebugUtilsMessengerCreateInfoEXT>(VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
    mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mi.pfnUserCallback = onDebugMessage;
    check(vkCreateDebugUtilsMessengerEXT(instance_, &mi, nullptr, &messenger_), "debug messenger");
  }
  return true;
}

bool VulkanDevice::pickPhysicalDevice(std::string* error) {
  const auto devices = enumerate<VkPhysicalDevice>([&](uint32_t* n, VkPhysicalDevice* p) {
    return vkEnumeratePhysicalDevices(instance_, n, p);
  });
  int64_t best = -1;
  for (VkPhysicalDevice pd : devices) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    const auto exts = enumerate<VkExtensionProperties>([&](uint32_t* n, VkExtensionProperties* p) {
      return vkEnumerateDeviceExtensionProperties(pd, nullptr, n, p);
    });
    const bool swapchain = std::any_of(exts.begin(), exts.end(), [](const VkExtensionProperties& e) {
      return !std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    });
    const auto families = enumerate<VkQueueFamilyProperties>([&](uint32_t* n, VkQueueFamilyProperties* p) {
      vkGetPhysicalDeviceQueueFamilyProperties(pd, n, p);
      return VK_SUCCESS;
    });
    int family = -1;
    for (uint32_t i = 0; i < families.size() && family < 0; ++i) {
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface_, &present);
      if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) family = int(i);
    }
    const bool usable = swapchain && family >= 0;
    logInfo("vulkan: device '%s' (%s, Vulkan %s)%s", props.deviceName, typeName(props.deviceType),
            versionString(props.apiVersion).c_str(), usable ? "" : " - unusable (no swapchain / present queue)");
    // Device type first, then the newest Vulkan version (a native driver beats a layered one,
    // e.g. Windows' D3D12-based "Microsoft Direct3D12 (...)" device on the same GPU).
    const int64_t score = int64_t(typeScore(props.deviceType)) << 32 | props.apiVersion;
    if (usable && score > best) {
      best = score;
      physical_ = pd;
      queueFamily_ = uint32_t(family);
      properties_ = props;
    }
  }
  if (!physical_) {
    *error = devices.empty() ? "no Vulkan devices" : "no Vulkan device can present to the window";
    return false;
  }
  vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);
  return true;
}

bool VulkanDevice::createDevice(std::string* error) {
  const float priority = 1.f;
  auto qi = make<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
  qi.queueFamilyIndex = queueFamily_;
  qi.queueCount = 1;
  qi.pQueuePriorities = &priority;
  const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  auto ci = make<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
  ci.queueCreateInfoCount = 1;
  ci.pQueueCreateInfos = &qi;
  ci.enabledExtensionCount = 1;
  ci.ppEnabledExtensionNames = extensions;
  const VkResult r = vkCreateDevice(physical_, &ci, nullptr, &device_);
  if (r != VK_SUCCESS) {
    *error = std::string("vkCreateDevice failed: ") + resultName(r);
    return false;
  }
  if (!loadDeviceFunctions(device_)) {
    *error = "device entry points missing";
    return false;
  }
  vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
  return true;
}

uint32_t VulkanDevice::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required,
                                      VkMemoryPropertyFlags preferred) const {
  uint32_t fallback = UINT32_MAX;
  for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags f = memory_.memoryTypes[i].propertyFlags;
    if (!(typeBits & (1u << i)) || (f & required) != required) continue;
    if ((f & preferred) == preferred) return i;
    if (fallback == UINT32_MAX) fallback = i;
  }
  return fallback;
}

bool VulkanDevice::supportsFormat(VkFormat format, VkFormatFeatureFlags features, bool optimalTiling) const {
  VkFormatProperties p;
  vkGetPhysicalDeviceFormatProperties(physical_, format, &p);
  const VkFormatFeatureFlags have = optimalTiling ? p.optimalTilingFeatures : p.linearTilingFeatures;
  return (have & features) == features;
}

bool VulkanDevice::supportsVertexFormat(VkFormat format) const {
  VkFormatProperties p;
  vkGetPhysicalDeviceFormatProperties(physical_, format, &p);
  return (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
}

std::string VulkanDevice::description() const {
  return std::string(properties_.deviceName) + " | " + typeName(properties_.deviceType) + " | Vulkan " +
         versionString(properties_.apiVersion) + " | driver " + driverString(properties_) + " | instance " +
         versionString(instanceVersion_) + (validation_ ? " | validation on" : "");
}

}  // namespace litholattice::vk
