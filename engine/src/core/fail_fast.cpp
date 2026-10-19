// Production fail-fast death path. See fail_fast.h for the contract.
#include "litholattice/core/fail_fast.h"

#include "litholattice/core/log.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>

namespace litholattice {

std::string formatSimCorruptionReport(const SimCorruptionReport& r) {
  char buf[4096];
  std::snprintf(buf, sizeof buf,
                "FATAL sim corruption: %s\n"
                "  subsystem: %s\n"
                "  at: %s:%d\n"
                "  tick: %s\n"
                "  expected hash: 0x%016" PRIx64 "\n"
                "  actual hash:   0x%016" PRIx64 "\n"
                "  detail: %s\n",
                r.what.c_str(), r.subsystem.c_str(),
                r.file.empty() ? "<unknown>" : r.file.c_str(), r.line,
                r.tick == kUnknownTick ? "<unknown>" : std::to_string(r.tick).c_str(), r.expectedHash,
                r.actualHash, r.detail.empty() ? "-" : r.detail.c_str());
  return std::string(buf);
}

bool simStateCorrupted(uint64_t expectedHash, uint64_t actualHash) {
  return expectedHash != actualHash;
}

SimCorruptionReport simCorruptionFromException(const std::exception& e, const char* subsystem,
                                               uint64_t tick) {
  SimCorruptionReport r;
  r.what = "exception-in-tick";
  r.subsystem = subsystem != nullptr ? subsystem : "<unknown>";
  r.tick = tick;
  r.detail = e.what();
  return r;
}

[[noreturn]] void simFailFast(const SimCorruptionReport& r) {
  const std::string text = formatSimCorruptionReport(r);
  // Raw stderr first: the log path allocates and takes a lock, so the report is
  // emitted directly in case the log machinery is the thing that's broken.
  std::fputs(text.c_str(), stderr);
  std::fflush(stderr);
  logError("%s", text.c_str());  // engine log path (stderr + log file, flushed)
  std::abort();                  // halt: never return, never unwind, never continue
}

[[noreturn]] void simFailFastFromException(const std::exception& e, const char* subsystem,
                                           uint64_t tick) {
  simFailFast(simCorruptionFromException(e, subsystem, tick));
}

}  // namespace litholattice
