// InputPump: the input thread (see input_thread.h for the contract).
#include "litholattice/input/input_thread.h"

#include <utility>

#include "litholattice/core/job_system.h"  // ThreadUtil: naming + realtime affinity
#include "litholattice/core/log.h"

namespace litholattice {

InputPump::InputPump(std::unique_ptr<IInputBackend> backend, Mode mode,
                     std::chrono::milliseconds pollInterval)
    : backend_(std::move(backend)), mode_(mode), pollInterval_(pollInterval) {
  if (mode_ == Mode::Threaded) {
    running_.store(true, std::memory_order_release);
    inputThread_ = std::thread([this] { threadLoop(); });
    LITHO_THREAD_LOG("input", "input thread started (backend='%s')", backend_->name().data());
  } else {
    LITHO_THREAD_LOG("input", "manual mode (backend='%s'): owner drives pumpOnce()",
                     backend_->name().data());
  }
}

InputPump::~InputPump() {
  if (inputThread_.joinable()) {
    running_.store(false, std::memory_order_release);
    inputThread_.join();  // disciplined: the input thread is never detached
    LITHO_THREAD_LOG("input", "input thread joined");
  }
}

void InputPump::threadLoop() {
  ThreadUtil::setName("litho-input");
  if (const uint64_t mask = JobSystem::realtimeAffinityMask(); mask != 0)
    ThreadUtil::pinToCores(mask);  // off the sim workers' cores (see job_system.h policy)
  while (running_.load(std::memory_order_acquire)) {
    pollOnce();
    std::this_thread::sleep_for(pollInterval_);
  }
  pollOnce();  // final drain: don't lose the events that landed during shutdown
}

void InputPump::pumpOnce() {
  if (mode_ == Mode::Threaded) return;  // the owned thread already polls
  pollOnce();
}

void InputPump::pollOnce() {
  staging_.clear();
  backend_->poll(staging_);
  const double now = nowSeconds();
  for (InputEvent& ev : staging_) {
    ev.time = now;  // normalize to the pump's monotonic clock (see contract)
    if (!queue_.tryPush(ev)) {
      const uint64_t d = dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
      LITHO_THREAD_LOG("input", "queue full: dropped event type=%d (total dropped=%llu)",
                       (int)ev.type, (unsigned long long)d);
      // Rate-limited: a full queue means the tick thread stopped draining (a bug),
      // worth reporting without flooding the log at 500 Hz.
      if (d == 1 || d % 512 == 0)
        logError("input pump: event queue full, dropped an event (total %llu); "
                 "the tick thread is not draining",
                 (unsigned long long)d);
    }
  }
  LITHO_THREAD_LOG("input", "polled %zu events", staging_.size());
}

void InputPump::drainInto(std::vector<InputEvent>& out) {
  // Tick thread only. tryPop is wait-free; the input thread never waits on us.
  while (auto ev = queue_.tryPop()) out.push_back(*ev);
}

double InputPump::nowSeconds() {
  using namespace std::chrono;
  return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace litholattice
