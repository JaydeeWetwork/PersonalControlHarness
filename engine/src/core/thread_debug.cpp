// Debug sink for LITHOLATTICE_THREAD_DEBUG (see thread_debug.h). Compiles to almost
// nothing when the flag is off.
#include "litholattice/core/thread_debug.h"

#ifdef LITHOLATTICE_THREAD_DEBUG

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace litholattice {
namespace detail {

namespace {
// The debug-log lock: L3 in the lock order, the absolute leaf. Taken to emit one
// line and released immediately; no other lock is ever acquired while holding it.
std::mutex g_threadLogMutex;
}  // namespace

uint64_t threadDebugNowUs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

uint64_t threadDebugNextJobId() {
  static std::atomic<uint64_t> nextId{1};
  return nextId.fetch_add(1, std::memory_order_relaxed);
}

void threadLog(const char* tag, const char* fmt, ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  char who[32];
#if defined(__linux__)
  if (pthread_getname_np(pthread_self(), who, sizeof(who)) != 0)
    std::snprintf(who, sizeof(who), "tid-%lu", (unsigned long)pthread_self());
#else
  std::snprintf(who, sizeof(who), "tid-%zu",
                std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif

  std::lock_guard<std::mutex> l(g_threadLogMutex);  // L3 leaf
  std::fprintf(stderr, "[litho-thread][%s][%s] %s\n", tag, who, msg);
  std::fflush(stderr);
}

}  // namespace detail
}  // namespace litholattice

#endif  // LITHOLATTICE_THREAD_DEBUG
