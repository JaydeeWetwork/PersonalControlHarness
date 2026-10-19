#include "litholattice/core/timer_resolution.h"
#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>  // timeBeginPeriod: 1 ms scheduler granularity
#endif

namespace litholattice {

#if defined(_WIN32)
HighResTimerScope::HighResTimerScope() : active_(timeBeginPeriod(1) == TIMERR_NOERROR) {}
HighResTimerScope::~HighResTimerScope() {
  if (active_) timeEndPeriod(1);
}
#else
HighResTimerScope::HighResTimerScope() = default;
HighResTimerScope::~HighResTimerScope() = default;
#endif

}  // namespace litholattice
