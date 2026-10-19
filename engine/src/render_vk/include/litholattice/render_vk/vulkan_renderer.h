// Vulkan backend (render_vk): implements IRenderer / ITextureArray (litholattice/render). It loads the
// system Vulkan loader at run time, so creating it simply fails (nullptr + reason) on a machine
// without Vulkan and the app falls back to OpenGL. Shaders are SPIR-V compiled at build time from
// assets/shaders/vk (plus the shared assets/shaders/chunk_shading.glsl); packs cannot override
// them. Windowing stays outside render: the app passes a VulkanSurfaceSource.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "litholattice/render/renderer.h"

namespace litholattice {

/// What the backend needs from the window system (platform module, wired up by the app).
struct VulkanSurfaceSource {
  /// Instance extensions the window system needs (VK_KHR_surface + the platform surface one).
  std::vector<std::string> instanceExtensions;
  /// Creates a VkSurfaceKHR for the window on the given VkInstance; returns 0 on failure.
  std::function<uint64_t(void* instance)> createSurface;
};

struct VulkanRendererDesc {
  VulkanSurfaceSource surface;
  bool vsync = false;
  /// Forces a present mode: "fifo", "mailbox" or "immediate" (falls back to FIFO when the
  /// surface lacks it). Empty: vsync -> FIFO, else IMMEDIATE, then MAILBOX, then FIFO.
  std::string presentMode;
  /// Chunk vertex memory: "device" (DEVICE_LOCAL + per-frame staging ring + vkCmdCopyBuffer,
  /// the default), "rebar" (DEVICE_LOCAL | HOST_VISIBLE written in place, falls back to device),
  /// or "host" (host-visible, the GPU reads over PCIe: the old path, kept for A/B benches).
  std::string vertexArena = "device";
  /// Enables VK_LAYER_KHRONOS_validation (+ debug messages in the log) when it is installed.
  bool validation = false;
};

/// Creates the Vulkan renderer (instance, device on the preferred GPU, swapchain, pipelines).
/// Returns nullptr and sets `error` if Vulkan is unavailable or any step fails. The renderer
/// presents in renderFrame(); the window must outlive it.
std::unique_ptr<IRenderer> createVulkanRenderer(const VulkanRendererDesc& desc, std::string* error);

/// Result of a headless Vulkan check (no window): used by --selftest.
struct VulkanProbe {
  bool loaderFound = false;  ///< false: no Vulkan loader library on this machine
  bool ok = false;           ///< instance created and at least one physical device listed
  /// True when a loader exists but this machine has no usable Vulkan
  /// device (instance creation reports VK_ERROR_INCOMPATIBLE_DRIVER, or
  /// enumeration yields 0 devices): an ENVIRONMENTAL absence, not a code
  /// failure -- the selftest reports SKIP, not FAIL (P2b-2, Appendix D
  /// claims note / G-L6). Genuine init failures with devices present
  /// keep ok=false with noDevices=false and still FAIL.
  bool noDevices = false;
  std::string summary;       ///< loader version and device names, or the error
};

/// Opens the loader, creates and destroys a bare instance and lists the physical devices.
VulkanProbe probeVulkan();

}  // namespace litholattice
