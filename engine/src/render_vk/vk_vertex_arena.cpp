#include "vk_vertex_arena.h"
#include <algorithm>
#include <iterator>
#include "litholattice/core/log.h"

namespace litholattice::vk {
namespace {
constexpr VkDeviceSize kAlignment = 256;  // keeps every slice 12-byte-vertex and cache friendly
VkDeviceSize alignUp(VkDeviceSize v) { return (v + kAlignment - 1) / kAlignment * kAlignment; }
}  // namespace

const char* VertexArena::kindName(Kind k) {
  switch (k) {
    case Kind::Host: return "host";
    case Kind::Device: return "device";
    case Kind::ReBar: return "rebar";
  }
  return "host";
}

bool VertexArena::parseKind(const std::string& s, Kind& out) {
  for (Kind k : {Kind::Host, Kind::Device, Kind::ReBar})
    if (s == kindName(k)) { out = k; return true; }
  return false;
}

VertexArena::VertexArena(const VulkanDevice& dev, Kind kind, VkDeviceSize blockSize) : dev_(dev), kind_(kind), blockSize_(blockSize) {}

bool VertexArena::addBlock(VkDeviceSize size) {
  constexpr auto kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  const VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  std::optional<Buffer> buf;
  if (kind_ == Kind::ReBar) {
    buf = Buffer::create(dev_, size, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | kHost);
    if (!buf) {
      logInfo("vulkan: no (more) host-visible device-local memory for the vertex arena; new blocks use device + staging");
      kind_ = Kind::Device;
    }
  }
  if (!buf) buf = kind_ == Kind::Host ? Buffer::create(dev_, size, usage, kHost)
                                      : Buffer::create(dev_, size, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!buf) return false;
  Block b;
  b.buffer = std::move(*buf);
  b.freeRanges.emplace(0, size);
  blocks_.push_back(std::move(b));
  return true;
}

std::optional<VertexArena::Slice> VertexArena::allocate(VkDeviceSize bytes) {
  const VkDeviceSize size = alignUp(std::max<VkDeviceSize>(bytes, 1));
  for (int attempt = 0; attempt < 2; ++attempt) {
    for (uint32_t i = 0; i < blocks_.size(); ++i) {
      auto& ranges = blocks_[i].freeRanges;
      for (auto it = ranges.begin(); it != ranges.end(); ++it) {
        if (it->second < size) continue;
        const Slice s{i, it->first, size};
        const VkDeviceSize rest = it->second - size, restOffset = it->first + size;
        ranges.erase(it);
        if (rest) ranges.emplace(restOffset, rest);
        return s;
      }
    }
    if (attempt == 0 && !addBlock(std::max(blockSize_, size))) return std::nullopt;
  }
  return std::nullopt;
}

void VertexArena::free(const Slice& s) {
  auto& ranges = blocks_[s.block].freeRanges;
  auto it = ranges.emplace(s.offset, s.size).first;
  if (const auto next = std::next(it); next != ranges.end() && it->first + it->second == next->first) {
    it->second += next->second;
    ranges.erase(next);
  }
  if (it != ranges.begin()) {
    const auto prev = std::prev(it);
    if (prev->first + prev->second == it->first) {
      prev->second += it->second;
      ranges.erase(it);
    }
  }
}

void VertexArena::clear() { blocks_.clear(); }

}  // namespace litholattice::vk
