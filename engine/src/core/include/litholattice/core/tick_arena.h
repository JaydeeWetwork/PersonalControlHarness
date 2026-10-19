// Per-tick bump (arena) allocator: allocate transient sim data during a tick, reset the
// whole arena at tick end. No per-allocation free, zero fragmentation, O(1) allocation.
//
// Research basis: engine optimization survey section 5.8 (floooh, "Handles are the better
// pointers"): one contiguous reservation, bump allocation is nearly free, reset per tick
// eliminates the malloc/free cycle for transient data. At 512 TPS that is 512 potential
// malloc/free pairs per second per allocator, gone.
//
// DETERMINISM: the memory layout is a pure function of the allocation call sequence
// (sizes, alignments, and call order). Two ticks issuing the same allocation sequence in
// the same order produce byte-identical layouts. Never hash or serialize raw pointers;
// serialize offsets/handles instead (see toHandle/fromHandle).
//
// OVERFLOW POLICY (locked): grow via chained blocks. When the primary block is exhausted,
// a new block is appended and allocation continues there; blocks are RETAINED across
// reset() so a steady-state tick never touches malloc/free after warmup. Overflow blocks
// never shrink and are at least as large as the previous block, so growth is a pure
// function of the allocation sequence (deterministic). totalReserved() reports the
// high-water mark for telemetry.
//
// THREAD SAFETY: not thread-safe by design (v1). One TickArena is owned by the single
// tick thread; sharing one across threads is a data race.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <type_traits>
#include <vector>

namespace litholattice {

class TickArena {
 public:
  static constexpr size_t kDefaultAlign = alignof(std::max_align_t);

  /// Checkpoint for nested scratch: mark(), allocate, rewind() discards everything after
  /// the mark. Cheap: two size_t copies, no allocation.
  struct Marker {
    size_t block = 0;
    size_t used = 0;
  };

  /// Stable reference to an allocation, valid until the next reset()/rewind() past it.
  /// Handles pack (block index, byte offset) so they stay valid across overflows and never
  /// bake in a raw address. Block indices are stable because blocks are never destroyed by
  /// reset(); they are only zeroed.
  using Handle = uint64_t;

  explicit TickArena(size_t capacityBytes);
  ~TickArena();

  TickArena(const TickArena&) = delete;
  TickArena& operator=(const TickArena&) = delete;
  TickArena(TickArena&&) noexcept;
  TickArena& operator=(TickArena&&) noexcept;

  /// Bump-allocate `size` bytes with `align` (defaults to max_align_t). A
  /// non-power-of-two `align` is rounded up to the next power of two (never
  /// silently misaligned, debug and release agree). O(1). Never returns nullptr:
  /// on exhaustion the arena grows (see OVERFLOW POLICY above). The memory is
  /// uninitialized.
  void* allocate(size_t size, size_t align = kDefaultAlign);

  /// Typed bump allocation of raw storage for `n` Ts; the caller owns construction.
  /// Alignment is alignof(T). The byte count is overflow-checked (review
  /// A-L4): an `n` whose `n * sizeof(T)` would wrap size_t fails fast
  /// (abort, like allocateSlow's oversize guard) instead of allocating a
  /// small block and handing back memory the caller believes is larger.
  template <typename T>
  T* alloc(size_t n = 1) {
    static_assert(std::is_trivially_destructible_v<T>,
                  "arena memory is never freed per-allocation, so T must be trivially destructible");
    if (sizeof(T) > 0 && n > std::numeric_limits<size_t>::max() / sizeof(T)) {
      std::fprintf(stderr, "tick_arena: alloc<T> count %zu overflows the byte count (fail fast)\n", n);
      std::abort();
    }
    return static_cast<T*>(allocate(n * sizeof(T), alignof(T)));
  }

  /// End-of-tick: all allocations are logically freed at once. Physical blocks are kept
  /// for the next tick (steady-state malloc-free). O(number of blocks), typically 1.
  void reset();

  /// Take a checkpoint of the current bump position. A moved-from arena
  /// (no blocks) marks the origin; allocating from it fails fast (see
  /// allocate() in tick_arena.cpp, review A-L4).
  Marker mark() const { return blocks_.empty() ? Marker{0, 0} : Marker{cur_, blocks_[cur_].used}; }

  /// Discard everything allocated after `m`. `m` must come from this arena and must not
  /// be newer than the current position.
  void rewind(Marker m);

  /// Byte offset of `p` inside its block (for building your own handles/serialization).
  /// `p` must point into this arena.
  uint32_t offsetOf(const void* p) const;

  /// Pack a stable handle for `p` (block index + offset). Valid until reset()/rewind().
  Handle toHandle(const void* p) const;

  /// Resolve a handle from toHandle(). Must be called before reset()/rewind() past it.
  /// The handle is validated in ALL builds (review A-L4): an out-of-range
  /// block index or offset fails fast (abort) instead of returning a wild
  /// pointer in release builds, where the old assert-only check vanished.
  void* fromHandle(Handle h) const;

  /// Bytes handed out since the last reset(), including alignment padding.
  size_t used() const;

  /// Primary block capacity in bytes.
  size_t capacity() const { return blocks_.empty() ? 0 : blocks_[0].size; }

  /// Total physical bytes reserved across all blocks (primary + overflow). Monotonic
  /// high-water mark; retained across reset() by design.
  size_t totalReserved() const;

  /// Number of chained blocks (1 when the primary block has never overflowed).
  size_t blockCount() const { return blocks_.size(); }

 private:
  struct Block {
    void* data = nullptr;  // aligned to kDefaultAlign
    size_t size = 0;
    size_t used = 0;
  };

  void* allocateSlow(size_t size, size_t align);
  static void* allocBlock(size_t size);
  static void freeBlock(void* p, size_t size) noexcept;

  std::vector<Block> blocks_;
  size_t cur_ = 0;  // block currently being bumped into
};

}  // namespace litholattice
