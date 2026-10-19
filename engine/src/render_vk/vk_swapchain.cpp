#include "vk_swapchain.h"
#include <algorithm>
#include <string>
#include "litholattice/core/log.h"

namespace litholattice::vk {

const char* presentModeName(VkPresentModeKHR m) {
  switch (m) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo_relaxed";
    default: return "other";
  }
}

std::optional<SurfaceConfig> chooseSurfaceConfig(const VulkanDevice& dev, bool vsync, const std::string& forced) {
  uint32_t n = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(dev.physical(), dev.surface(), &n, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(n);
  if (n) vkGetPhysicalDeviceSurfaceFormatsKHR(dev.physical(), dev.surface(), &n, formats.data());
  if (formats.empty()) return std::nullopt;
  SurfaceConfig cfg;
  cfg.color = formats[0];
  for (const VkSurfaceFormatKHR& f : formats)
    if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      cfg.color = f;
      break;
    }
  for (const VkFormat d : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT})
    if (dev.supportsFormat(d, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, true)) {
      cfg.depth = d;
      break;
    }
  if (cfg.depth == VK_FORMAT_UNDEFINED) return std::nullopt;

  vkGetPhysicalDeviceSurfacePresentModesKHR(dev.physical(), dev.surface(), &n, nullptr);
  std::vector<VkPresentModeKHR> modes(n);
  if (n) vkGetPhysicalDeviceSurfacePresentModesKHR(dev.physical(), dev.surface(), &n, modes.data());
  const auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
  std::string offered;
  for (VkPresentModeKHR m : modes) offered += std::string(offered.empty() ? "" : ", ") + presentModeName(m);
  cfg.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always supported
  if (!forced.empty()) {
    const VkPresentModeKHR want = forced == "mailbox" ? VK_PRESENT_MODE_MAILBOX_KHR
                                  : forced == "immediate" ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    if (has(want)) cfg.presentMode = want;
    else logError("vulkan: present mode '%s' not offered by the surface; using fifo", forced.c_str());
  } else if (!vsync) {  // uncapped: IMMEDIATE (MAILBOX is held to the refresh rate by some compositors)
    if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) cfg.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    else if (has(VK_PRESENT_MODE_MAILBOX_KHR)) cfg.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
  }
  logInfo("vulkan: present mode %s (surface offers: %s; vsync %s%s%s)", presentModeName(cfg.presentMode), offered.c_str(),
          vsync ? "on" : "off", forced.empty() ? "" : ", forced ", forced.c_str());
  return cfg;
}

std::unique_ptr<Swapchain> Swapchain::create(const VulkanDevice& dev, const SurfaceConfig& cfg, VkRenderPass pass,
                                             uint32_t width, uint32_t height, const Swapchain* old) {
  VkSurfaceCapabilitiesKHR caps;
  if (!check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev.physical(), dev.surface(), &caps), "surface capabilities"))
    return nullptr;
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == UINT32_MAX) {  // the window system lets the swapchain decide
    extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
  }
  if (extent.width == 0 || extent.height == 0) return nullptr;  // minimized
  uint32_t images = caps.minImageCount + 1;
  if (caps.maxImageCount) images = std::min(images, caps.maxImageCount);

  auto sc = std::make_unique<Swapchain>(Passkey{});
  sc->extent_ = extent;
  auto ci = make<VkSwapchainCreateInfoKHR>(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
  ci.surface = dev.surface();
  ci.minImageCount = images;
  ci.imageFormat = cfg.color.format;
  ci.imageColorSpace = cfg.color.colorSpace;
  ci.imageExtent = extent;
  ci.imageArrayLayers = 1;
  // Screenshots: the lithoscript `screenshot` command copies the presented image to host.
  // TRANSFER_SRC is optional in the spec, so only request it when the surface allows it.
  const bool wantTransferSrc =
      (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
  ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                  (wantTransferSrc ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                        ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
  ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  for (const VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR})
    if (caps.supportedCompositeAlpha & a) { ci.compositeAlpha = a; break; }
  ci.presentMode = cfg.presentMode;
  ci.clipped = VK_TRUE;
  ci.oldSwapchain = old ? old->handle() : VK_NULL_HANDLE;
  VkSwapchainKHR handle = VK_NULL_HANDLE;
  if (!check(vkCreateSwapchainKHR(dev.device(), &ci, nullptr, &handle), "vkCreateSwapchainKHR")) return nullptr;
  sc->swapchain_ = UniqueSwapchain(dev.device(), handle);

  uint32_t count = 0;
  vkGetSwapchainImagesKHR(dev.device(), handle, &count, nullptr);
  std::vector<VkImage> swapImages(count);
  vkGetSwapchainImagesKHR(dev.device(), handle, &count, swapImages.data());
  sc->images_ = swapImages;
  sc->transferSrc_ = wantTransferSrc;

  auto di = make<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
  di.imageType = VK_IMAGE_TYPE_2D;
  di.format = cfg.depth;
  di.extent = {extent.width, extent.height, 1};
  di.mipLevels = 1;
  di.arrayLayers = 1;
  di.samples = VK_SAMPLE_COUNT_1_BIT;
  di.tiling = VK_IMAGE_TILING_OPTIMAL;
  di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  di.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  sc->depth_ = Image::create(dev, di, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT);
  if (!sc->depth_) return nullptr;

  for (const VkImage img : swapImages) {
    auto vi = make<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = cfg.color.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (!check(vkCreateImageView(dev.device(), &vi, nullptr, &view), "swapchain image view")) return nullptr;
    sc->views_.emplace_back(dev.device(), view);
    const VkImageView attachments[] = {view, sc->depth_->view()};
    auto fi = make<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
    fi.renderPass = pass;
    fi.attachmentCount = 2;
    fi.pAttachments = attachments;
    fi.width = extent.width;
    fi.height = extent.height;
    fi.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (!check(vkCreateFramebuffer(dev.device(), &fi, nullptr, &fb), "vkCreateFramebuffer")) return nullptr;
    sc->framebuffers_.emplace_back(dev.device(), fb);
  }
  return sc;
}

}  // namespace litholattice::vk
