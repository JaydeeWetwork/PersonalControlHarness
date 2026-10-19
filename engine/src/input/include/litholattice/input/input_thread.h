// The input thread: InputEvents flow from the OS to the engine without ever
// stalling behind sim, render, or I/O.
//
// ==== THE INPUT-THREAD CONTRACT (read this before wiring input) ====
// Who owns the input thread: InputPump. Construct it with the game's
// IInputBackend; in Mode::Threaded it spawns one NamedThread ("litho-input",
// pinned to the realtime affinity mask) that polls the backend in a loop.
// In Mode::Manual it spawns no thread and the owner calls pumpOnce() instead —
// REQUIRED for backends that must stay on the main thread (the GLFW backend's
// poll() calls glfwPollEvents(), which is main-thread-only on macOS).
//
// How events are queued and drained: the pump is the single producer into a
// lock-free SpscQueue<InputEvent, 1024> (same idiom as the audio command queue:
// wait-free push, no allocation after construction). The TICK thread is the
// single consumer: once per frame it calls drainInto(), which moves every queued
// event (oldest first) into a vector. Fan-out to engine, window, and UI happens
// on the tick thread from that one drained span — InputSystem::processFrame()
// feeds the engine, frameEvents() feeds the UI — so there is exactly one
// cross-thread queue, not one per consumer.
//
// Timestamping: the pump re-stamps every event with its own monotonic
// steady_clock on receipt (backends use heterogeneous clocks — glfwGetTime vs.
// scripted times — so the pump normalizes to one coherent clock). Times are
// therefore non-decreasing in drain order.
//
// Overflow: the queue holds 1024 events; when full, the newest event is dropped
// and dropped() counts it (debug-logged). A frame's input is dozens of events,
// so sustained overflow means the tick thread stopped draining — a bug.
//
// Ordering guarantees vs. the deterministic sim: LIVE INPUT IS NON-DETERMINISTIC,
// full stop — OS timing decides when a key press lands. The determinism boundary
// sits at the drain: the engine drains the pump exactly once per tick/frame,
// BEFORE the sim tick, and the sim sees a fixed event list for that tick.
// Deterministic GIVEN the list. Two runs with live input are NOT expected to
// hash identically; two runs fed the same ReplayInputBackend stream ARE —
// ReplayInputBackend bypasses the pump entirely (driven on the tick thread, no
// input thread involved) and stays the deterministic path for tests, replays,
// and golden traces.
//
// Liveness: input NEVER stalls behind sim/render/I/O — the input thread touches
// no game state, takes no game locks, and its push is wait-free; the tick thread
// only drains (never waits on) the queue. If the tick thread is busy, events
// accumulate in the queue (or drop with a count) — key presses are never lost
// to a lock, only to a genuinely full queue.
// ======================================================================
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "litholattice/core/spsc_queue.h"
#include "litholattice/core/thread_debug.h"
#include "litholattice/input/input_backend.h"
#include "litholattice/input/input_event.h"

namespace litholattice {

class InputPump {
 public:
  enum class Mode {
    Threaded,  ///< pump owns the input thread; backend must be pollable off the main thread
    Manual,    ///< no thread; the owner calls pumpOnce() once per frame (e.g. GLFW backend)
  };

  /// `pollInterval`: how often the input thread polls the backend in Threaded mode.
  explicit InputPump(std::unique_ptr<IInputBackend> backend, Mode mode = Mode::Threaded,
                     std::chrono::milliseconds pollInterval = std::chrono::milliseconds(2));
  ~InputPump();  // stops the thread (if any) and joins it: disciplined, never detached

  InputPump(const InputPump&) = delete;
  InputPump& operator=(const InputPump&) = delete;

  /// Tick thread, once per frame: move all queued events into `out` (oldest
  /// first). Never blocks, never waits on the input thread.
  void drainInto(std::vector<InputEvent>& out);

  /// Manual mode: poll the backend once, right now, on the calling thread.
  /// (A no-op safeguard in Threaded mode — the thread already polls.)
  void pumpOnce();

  /// Events dropped because the queue was full (see contract). Monotonic.
  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

  bool threaded() const { return mode_ == Mode::Threaded; }

  static constexpr size_t kQueueCapacity = 1024;  // power of two (SpscQueue requirement)

 private:
  void threadLoop();
  void pollOnce();  // backend->poll into staging_, stamp, wait-free push into queue_
  static double nowSeconds();

  static_assert(std::is_trivially_copyable_v<InputEvent>,
                "InputEvent must stay trivially copyable for the lock-free SPSC queue");

  std::unique_ptr<IInputBackend> backend_;
  const Mode mode_;
  const std::chrono::milliseconds pollInterval_;
  SpscQueue<InputEvent, kQueueCapacity> queue_;  // input thread -> tick thread, lock-free
  std::vector<InputEvent> staging_;             // backend's per-poll output (input thread only)
  std::atomic<bool> running_{false};
  std::thread inputThread_;
  std::atomic<uint64_t> dropped_{0};
};

}  // namespace litholattice
