// Minimal logging: printf-style, timestamped, one RECORD per call, thread-safe.
// A record is not necessarily one physical line: the formatted message is passed
// through verbatim, so a caller that formats a multi-line block (e.g. the
// fail-fast corruption report, fail_fast.cpp) produces a multi-line text-log
// entry and a single multi-line LogTap record. Line-wise parsers of the text
// log must tolerate continuation lines that carry no timestamp prefix.
// After logOpenFile(), every line is also written (and flushed) to the log file so it can
// be tailed live; the previous run's file is kept as <name>.prev.log.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace litholattice {
#if defined(__GNUC__)
#define LITHOLATTICE_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define LITHOLATTICE_PRINTF(a, b)
#endif
void logInfo(const char* fmt, ...) LITHOLATTICE_PRINTF(1, 2);
void logWarn(const char* fmt, ...) LITHOLATTICE_PRINTF(1, 2);
void logError(const char* fmt, ...) LITHOLATTICE_PRINTF(1, 2);

// ---- Engine-log record path (ring-buffer tap) ----
// Every logInfo/logWarn/logError line is ALSO pushed into a fixed-size ring buffer
// (the tap): the general engine-log record path, not console-private. In-game
// consumers (the debug console) read it via engineLogTap().snapshot(); engine
// subsystems that log on behalf of another system use the same printf-style calls.
//
// WORKER-3 CONTRACT (mod suspension / auto-recovery logging): log through the
// normal calls, e.g.
//   logWarn("modsys: suspended mod '%s': %s", modId, reason);
//   logInfo("modsys: auto-recovered mod '%s' after %u ticks", modId, ticks);
// and read the records back with engineLogTap().snapshot(). Do NOT invent a
// parallel log channel: this tap IS the record path.
enum class LogLevel : uint8_t { Info, Warn, Error };
struct LogRecord {
  uint64_t seq = 0;       ///< monotonic push sequence (lets readers track "new since X")
  LogLevel level = LogLevel::Info;
  std::string timestamp;  ///< "HH:MM:SS.mmm" wall clock, the same stamp as the text log
  std::string text;       ///< the formatted message, without any level prefix
};
class LogTap {
 public:
  static constexpr size_t kCapacity = 512;  ///< last-N records kept (power of two)
  void push(LogRecord rec);                 ///< thread-safe; drops the oldest past capacity
  std::vector<LogRecord> snapshot() const;  ///< oldest -> newest; thread-safe
  size_t size() const;
  void clear();
 private:
  mutable std::mutex mutex_;
  std::vector<LogRecord> buf_;  ///< ring storage; buf_.size() <= kCapacity
  size_t head_ = 0;             ///< index of the oldest record
  uint64_t nextSeq_ = 1;
};
/// The process-wide log tap. Thread-safe.
LogTap& engineLogTap();
// Rotate an existing file to *.prev.log, then tee all log lines into path. False on failure.
bool logOpenFile(const std::string& path);
void logCloseFile();
}  // namespace litholattice
