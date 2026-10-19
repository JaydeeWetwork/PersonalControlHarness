#include "vk_resources.h"
#include "litholattice/core/log.h"

namespace litholattice::vk {
namespace {

std::optional<UniqueMemory> allocate(const VulkanDevice& dev, const VkMemoryRequirements& req,
                                     VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) {
  const uint32_t type = dev.findMemoryType(req.memoryTypeBits, required, required | preferred);
  if (type == UINT32_MAX) {
    logError("vulkan: no memory type with flags 0x%x", unsigned(required));
    return std::nullopt;
  }
  auto ai = make<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = type;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  if (!check(vkAllocateMemory(dev.device(), &ai, nullptr, &mem), "vkAllocateMemory")) return std::nullopt;
  return UniqueMemory(dev.device(), mem);
}

}  // namespace

std::optional<Buffer> Buffer::create(const VulkanDevice& dev, VkDeviceSize size, VkBufferUsageFlags usage,
                                     VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) {
  Buffer b;
  auto ci = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
  ci.size = size;
  ci.usage = usage;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buf = VK_NULL_HANDLE;
  if (!check(vkCreateBuffer(dev.device(), &ci, nullptr, &buf), "vkCreateBuffer")) return std::nullopt;
  b.buffer_ = UniqueBuffer(dev.device(), buf);
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(dev.device(), buf, &req);
  auto mem = allocate(dev, req, required, preferred);
  if (!mem) return std::nullopt;
  b.memory_ = std::move(*mem);
  if (!check(vkBindBufferMemory(dev.device(), buf, b.memory_.get(), 0), "vkBindBufferMemory")) return std::nullopt;
  if (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
    void* p = nullptr;
    if (!check(vkMapMemory(dev.device(), b.memory_.get(), 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory")) return std::nullopt;
    b.mapped_ = static_cast<uint8_t*>(p);
  }
  b.size_ = size;
  return b;
}

std::optional<Image> Image::create(const VulkanDevice& dev, const VkImageCreateInfo& info, VkImageViewType viewType,
                                   VkImageAspectFlags aspect) {
  Image img;
  VkImage handle = VK_NULL_HANDLE;
  if (!check(vkCreateImage(dev.device(), &info, nullptr, &handle), "vkCreateImage")) return std::nullopt;
  img.image_ = UniqueImage(dev.device(), handle);
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(dev.device(), handle, &req);
  auto mem = allocate(dev, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
  if (!mem) return std::nullopt;
  img.memory_ = std::move(*mem);
  if (!check(vkBindImageMemory(dev.device(), handle, img.memory_.get(), 0), "vkBindImageMemory")) return std::nullopt;
  auto vi = make<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
  vi.image = handle;
  vi.viewType = viewType;
  vi.format = info.format;
  vi.subresourceRange = {aspect, 0, info.mipLevels, 0, info.arrayLayers};
  VkImageView view = VK_NULL_HANDLE;
  if (!check(vkCreateImageView(dev.device(), &vi, nullptr, &view), "vkCreateImageView")) return std::nullopt;
  img.view_ = UniqueImageView(dev.device(), view);
  return img;
}

bool submitOnce(const VulkanDevice& dev, VkCommandPool pool, const std::function<void(VkCommandBuffer)>& record) {
  auto ai = make<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
  ai.commandPool = pool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (!check(vkAllocateCommandBuffers(dev.device(), &ai, &cmd), "vkAllocateCommandBuffers")) return false;
  auto bi = make<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  bool ok = check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
  if (ok) {
    record(cmd);
    ok = check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
  }
  if (ok) {
    auto si = make<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    ok = check(vkQueueSubmit(dev.queue(), 1, &si, VK_NULL_HANDLE), "vkQueueSubmit (upload)") &&
         check(vkQueueWaitIdle(dev.queue()), "vkQueueWaitIdle");
  }
  vkFreeCommandBuffers(dev.device(), pool, 1, &cmd);
  return ok;
}

void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                     VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                     VkPipelineStageFlags dstStage, uint32_t baseMip, uint32_t mipCount) {
  auto b = make<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
  b.srcAccessMask = srcAccess;
  b.dstAccessMask = dstAccess;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, VK_REMAINING_ARRAY_LAYERS};
  vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace litholattice::vk
