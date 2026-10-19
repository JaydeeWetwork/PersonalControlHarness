// VertexArena: chunk vertex data sub-allocated from a few large buffers, so the number of Vulkan
// allocations stays tiny (drivers cap it, e.g. 4096) however many sub-chunks are loaded.
// First-fit free lists with coalescing. Private to render_vk.
// Kinds: Host (host-visible, written in place, the GPU reads over PCIe), Device (DEVICE_LOCAL,
// filled by vkCmdCopyBuffer from the renderer's staging ring), ReBar (DEVICE_LOCAL and
// host-visible, written in place; a block that cannot get such memory falls back to Device).
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "vk_resources.h"

namespace litholattice::vk {

class VertexArena {
 public:
  /// A range of one block; `size` is the rounded-up allocation size.
  struct Slice {
    uint32_t block = 0;
    VkDeviceSize offset = 0, size = 0;
  };

  enum class Kind { Host, Device, ReBar };
  static const char* kindName(Kind k);
  /// "host" | "device" | "rebar"; false if unknown.
  static bool parseKind(const std::string& s, Kind& out);

  VertexArena(const VulkanDevice& dev, Kind kind, VkDeviceSize blockSize = VkDeviceSize(32) << 20);
  Kind kind() const { return kind_; }

  /// Reserves `bytes` (a new block is added when none has room). nullopt if out of memory.
  std::optional<Slice> allocate(VkDeviceSize bytes);
  /// Returns a slice; the caller guarantees the GPU no longer reads it.
  void free(const Slice& s);
  /// Drops every block (the caller guarantees the GPU is idle).
  void clear();

  VkBuffer buffer(const Slice& s) const { return blocks_[s.block].buffer.handle(); }
  /// Where to write the slice in place, or nullptr for a device-only block (stage + copy).
  uint8_t* data(const Slice& s) const {
    uint8_t* m = blocks_[s.block].buffer.mapped();
    return m ? m + s.offset : nullptr;
  }
  size_t blockCount() const { return blocks_.size(); }

 private:
  struct Block {
    Buffer buffer;
    std::map<VkDeviceSize, VkDeviceSize> freeRanges;  ///< offset -> size, non-adjacent
  };
  bool addBlock(VkDeviceSize size);

  const VulkanDevice& dev_;
  Kind kind_;
  VkDeviceSize blockSize_;
  std::vector<Block> blocks_;
};

}  // namespace litholattice::vk
