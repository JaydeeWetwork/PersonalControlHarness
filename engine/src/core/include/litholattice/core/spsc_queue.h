// Bounded single-producer / single-consumer queue. Wait-free on both sides, no allocation after
// construction, so the consumer may run in a real-time audio callback.
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>

namespace litholattice {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // padding from alignas is the point: head and tail on separate cache lines
#endif

/// Fixed-capacity SPSC ring buffer. Capacity must be a power of two; one producer thread calls
/// tryPush, one consumer thread calls tryPop.
template <typename T, size_t Capacity>
class SpscQueue {
  static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>, "items are copied in the consumer's real-time context");

 public:
  /// Producer: enqueue a copy of `v`. Returns false (and drops nothing else) when full.
  bool tryPush(const T& v) {
    const size_t head = head_.load(std::memory_order_relaxed);
    if (head - tail_.load(std::memory_order_acquire) == Capacity) return false;
    items_[head & (Capacity - 1)] = v;
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  /// Consumer: dequeue the oldest item, if any.
  std::optional<T> tryPop() {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) return std::nullopt;
    T v = items_[tail & (Capacity - 1)];
    tail_.store(tail + 1, std::memory_order_release);
    return v;
  }

 private:
  std::array<T, Capacity> items_{};
  alignas(64) std::atomic<size_t> head_{0};  // written by the producer
  alignas(64) std::atomic<size_t> tail_{0};  // written by the consumer
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace litholattice
