#include "vk_loader.h"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define LITHOLATTICE_VK_DEFINE(name) PFN_##name name = nullptr;
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion = nullptr;
PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT = nullptr;
PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT = nullptr;
LITHOLATTICE_VK_GLOBAL_FUNCTIONS(LITHOLATTICE_VK_DEFINE)
LITHOLATTICE_VK_INSTANCE_FUNCTIONS(LITHOLATTICE_VK_DEFINE)
LITHOLATTICE_VK_SURFACE_FUNCTIONS(LITHOLATTICE_VK_DEFINE)
LITHOLATTICE_VK_DEVICE_FUNCTIONS(LITHOLATTICE_VK_DEFINE)
#undef LITHOLATTICE_VK_DEFINE

namespace litholattice::vk {
namespace {

PFN_vkVoidFunction librarySymbol(const char* name) {
#if defined(_WIN32)
  static HMODULE lib = LoadLibraryA("vulkan-1.dll");
  return lib ? reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(lib, name)) : nullptr;
#else
  static void* lib = [] {
    void* h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    return h ? h : dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  }();
  return lib ? reinterpret_cast<PFN_vkVoidFunction>(dlsym(lib, name)) : nullptr;
#endif
}

/// Stores `fn` into `slot` (cast to the slot's type); true if it was found.
template <typename Fn>
bool assign(Fn& slot, PFN_vkVoidFunction fn) {
  slot = reinterpret_cast<Fn>(fn);
  return fn != nullptr;
}

}  // namespace

bool loadLoader() {
  if (vkGetInstanceProcAddr) return true;
  if (!assign(vkGetInstanceProcAddr, librarySymbol("vkGetInstanceProcAddr"))) return false;
  bool ok = true;
#define LITHOLATTICE_VK_LOAD(name) ok &= assign(name, vkGetInstanceProcAddr(nullptr, #name));
  LITHOLATTICE_VK_GLOBAL_FUNCTIONS(LITHOLATTICE_VK_LOAD)
#undef LITHOLATTICE_VK_LOAD
  assign(vkEnumerateInstanceVersion, vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
  if (!ok) vkGetInstanceProcAddr = nullptr;
  return ok;
}

bool loadInstanceFunctions(VkInstance instance, bool withSurface) {
  bool ok = true, surface = true;
#define LITHOLATTICE_VK_LOAD(name) ok &= assign(name, vkGetInstanceProcAddr(instance, #name));
  LITHOLATTICE_VK_INSTANCE_FUNCTIONS(LITHOLATTICE_VK_LOAD)
#undef LITHOLATTICE_VK_LOAD
#define LITHOLATTICE_VK_LOAD(name) surface &= assign(name, vkGetInstanceProcAddr(instance, #name));
  LITHOLATTICE_VK_SURFACE_FUNCTIONS(LITHOLATTICE_VK_LOAD)
#undef LITHOLATTICE_VK_LOAD
  if (withSurface) ok &= surface;
  assign(vkCreateDebugUtilsMessengerEXT, vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
  assign(vkDestroyDebugUtilsMessengerEXT, vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
  return ok;
}

bool loadDeviceFunctions(VkDevice device) {
  bool ok = true;
#define LITHOLATTICE_VK_LOAD(name) ok &= assign(name, vkGetDeviceProcAddr(device, #name));
  LITHOLATTICE_VK_DEVICE_FUNCTIONS(LITHOLATTICE_VK_LOAD)
#undef LITHOLATTICE_VK_LOAD
  return ok;
}

}  // namespace litholattice::vk
