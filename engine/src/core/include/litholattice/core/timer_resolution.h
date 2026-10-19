// Scoped request for a fine OS scheduler tick, so short sleeps are accurate (Windows defaults to
// ~15.6 ms; this asks for 1 ms while the scope lives). No-op on platforms that do not need it.
#pragma once

namespace litholattice {

class HighResTimerScope {
 public:
  HighResTimerScope();
  ~HighResTimerScope();
  HighResTimerScope(const HighResTimerScope&) = delete;
  HighResTimerScope& operator=(const HighResTimerScope&) = delete;

 private:
  bool active_ = false;
};

}  // namespace litholattice
