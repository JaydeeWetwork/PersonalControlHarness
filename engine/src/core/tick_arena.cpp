#include "litholattice/core/tick_arena.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>  // aligned operator new/delete

namespace litholattice {
namespace {

constexpr size_t kMinCapacity = 256;

bool isPow2(size_t v) { return v != 0 && (v & (v - 1)) == 0; }

// Next power of two >= v (v > 0). Pure function of the input, so the rounding is
// deterministic across builds and machines.
size_t nextPow2(size_t v) {
  --v;
  v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
  if constexpr (sizeof(size_t) > 4) v |= v >> 32;
  return v + 1;
}

// Bump `used` forward inside [data, data + size); returns nullptr when it does not fit.
void* bump(void* data, size_t size, size_t& used, size_t reqSize, size_t align) {
  const uintptr_t raw = reinterpret_cast<uintptr_t>(data) + used;
  const uintptr_t aligned = (raw + (align - 1)) & ~(align - 1);
  const size_t pad = static_cast<size_t>(aligned - raw);
  if (pad > size - used || reqSize > size - used - pad) return nullptr;  // no size_t wraparound
  used += pad + reqSize;
  return reinterpret_cast<void*>(aligned);
}

}  // namespace

void* TickArena::allocBlock(size_t size) {
  assert(size <= 0xFFFFFFFFu && "TickArena: blocks are limited to 4 GiB (32-bit handle offsets)");
  return ::operator new(size, std::align_val_t(kDefaultAlign));
}

void TickArena::freeBlock(void* p, size_t size) noexcept {
  ::operator delete(p, size, std::align_val_t(kDefaultAlign));
}

TickArena::TickArena(size_t capacityBytes) {
  if (capacityBytes < kMinCapacity) capacityBytes = kMinCapacity;
  blocks_.push_back(Block{allocBlock(capacityBytes), capacityBytes, 0});
}

TickArena::~TickArena() {
  for (const Block& b : blocks_) freeBlock(b.data, b.size);
}

TickArena::TickArena(TickArena&& other) noexcept
    : blocks_(std::move(other.blocks_)), cur_(other.cur_) {
  other.cur_ = 0;
}

TickArena& TickArena::operator=(TickArena&& other) noexcept {
  if (this != &other) {
    for (const Block& b : blocks_) freeBlock(b.data, b.size);
    blocks_ = std::move(other.blocks_);
    cur_ = other.cur_;
    other.cur_ = 0;
  }
  return *this;
}

void* TickArena::allocate(size_t size, size_t align) {
  // Moved-from arena (review A-L4): the move left blocks_ empty, and the
  // bump below would dereference blocks_[cur_] — UB. A moved-from arena is
  // a shell for destruction/assignment only; allocating from it fails fast.
  if (blocks_.empty()) {
    std::fprintf(stderr, "tick_arena: allocate() on a moved-from arena (fail fast)\n");
    std::abort();
  }
  // The old code asserted pow2 (debug-only): release builds with non-pow2 `align`
  // silently misaligned. Round up instead - over-alignment is always valid, and the
  // rounding is a pure function of the input, so release now matches debug.
  if (align == 0) align = 1;
  if (!isPow2(align)) align = nextPow2(align);
  if (void* p = bump(blocks_[cur_].data, blocks_[cur_].size, blocks_[cur_].used, size, align))
    return p;
  return allocateSlow(size, align);
}

void* TickArena::allocateSlow(size_t size, size_t align) {
  // Reuse an existing overflow block that fits (keeps block indices stable for handles).
  for (size_t i = cur_ + 1; i < blocks_.size(); ++i) {
    if (void* p = bump(blocks_[i].data, blocks_[i].size, blocks_[i].used, size, align)) {
      cur_ = i;
      return p;
    }
  }
  // Grow: new block is at least as large as the last one, so growth is a pure function
  // of the allocation sequence (deterministic); huge single requests allocate exactly.
  // Overflow guard: size + align wrapping would violate the "never returns nullptr"
  // contract with a tiny block. Fail fast instead — this is a programming error.
  if (size > SIZE_MAX - align) {
    std::fprintf(stderr, "tick_arena: allocation size %zu overflows (fail fast)\n", size);
    std::abort();
  }
  const size_t need = size + align;  // upper bound: alignment can waste at most align-1
  size_t bs = blocks_.back().size;
  if (bs < need) bs = need;
  blocks_.push_back(Block{allocBlock(bs), bs, 0});
  cur_ = blocks_.size() - 1;
  void* p = bump(blocks_[cur_].data, blocks_[cur_].size, blocks_[cur_].used, size, align);
  assert(p && "fresh overflow block must fit the request");
  return p;
}

void TickArena::reset() {
  for (Block& b : blocks_) b.used = 0;
  cur_ = 0;
}

void TickArena::rewind(Marker m) {
  assert(m.block <= cur_ && "TickArena::rewind: marker is newer than the current position");
  assert(m.used <= blocks_[m.block].used && "TickArena::rewind: marker is inside live memory");
  for (size_t i = m.block + 1; i <= cur_; ++i) blocks_[i].used = 0;
  cur_ = m.block;
  blocks_[cur_].used = m.used;
}

uint32_t TickArena::offsetOf(const void* p) const {
  const uintptr_t addr = reinterpret_cast<uintptr_t>(p);
  for (const Block& b : blocks_) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(b.data);
    if (addr >= base && addr < base + b.size)
      return static_cast<uint32_t>(addr - base);
  }
  assert(false && "TickArena::offsetOf: pointer is not inside this arena");
  return 0;
}

TickArena::Handle TickArena::toHandle(const void* p) const {
  const uintptr_t addr = reinterpret_cast<uintptr_t>(p);
  for (size_t i = 0; i < blocks_.size(); ++i) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(blocks_[i].data);
    if (addr >= base && addr < base + blocks_[i].size)
      return (static_cast<Handle>(i) << 32) | static_cast<uint32_t>(addr - base);
  }
  assert(false && "TickArena::toHandle: pointer is not inside this arena");
  return 0;
}

void* TickArena::fromHandle(Handle h) const {
  const size_t i = static_cast<size_t>(h >> 32);
  const size_t off = static_cast<size_t>(h & 0xFFFFFFFFu);
  // Validated in ALL builds (review A-L4): the check is two comparisons, and
  // the release-build alternative — a wild pointer from a bad handle — turns
  // a programming error into memory corruption. Fail fast instead.
  if (i >= blocks_.size() || off > blocks_[i].size) {
    std::fprintf(stderr, "tick_arena: fromHandle: bad handle (block %zu, offset %zu) (fail fast)\n", i, off);
    std::abort();
  }
  return static_cast<char*>(blocks_[i].data) + off;
}

size_t TickArena::used() const {
  size_t total = 0;
  for (const Block& b : blocks_) total += b.used;
  return total;
}

size_t TickArena::totalReserved() const {
  size_t total = 0;
  for (const Block& b : blocks_) total += b.size;
  return total;
}

}  // namespace litholattice
