#include "litholattice/core/log.h"
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <share.h>
#endif

namespace litholattice {
namespace {
std::mutex gLogMutex;
FILE* gFile = nullptr;

void stamp(char* buf, size_t n) {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::time_t t = system_clock::to_time_t(now);
  const int ms = int(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::snprintf(buf, n, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
}

void vlog(FILE* console, LogLevel tapLevel, const char* level, const char* fmt, va_list ap) {
  char msg[2048];
  std::vsnprintf(msg, sizeof msg, fmt, ap);
  char ts[32];
  stamp(ts, sizeof ts);
  std::lock_guard l(gLogMutex);
  std::fprintf(console, "[%s] %s%s\n", ts, level, msg);
  std::fflush(console);
  if (gFile) { std::fprintf(gFile, "[%s] %s%s\n", ts, level, msg); std::fflush(gFile); }
  LogRecord rec;
  rec.level = tapLevel;
  rec.timestamp = ts;
  rec.text = msg;
  engineLogTap().push(std::move(rec));  // tap lock nests inside gLogMutex, always in this order
}
}  // namespace

void logInfo(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlog(stdout, LogLevel::Info, "", fmt, ap); va_end(ap); }
void logWarn(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlog(stderr, LogLevel::Warn, "WARN ", fmt, ap); va_end(ap); }
void logError(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlog(stderr, LogLevel::Error, "ERROR ", fmt, ap); va_end(ap); }

void LogTap::push(LogRecord rec) {
  std::lock_guard l(mutex_);
  rec.seq = nextSeq_++;
  if (buf_.size() < kCapacity) {
    buf_.push_back(std::move(rec));
  } else {
    buf_[head_] = std::move(rec);
    head_ = (head_ + 1) % kCapacity;
  }
}

std::vector<LogRecord> LogTap::snapshot() const {
  std::lock_guard l(mutex_);
  std::vector<LogRecord> out;
  out.reserve(buf_.size());
  for (size_t i = 0; i < buf_.size(); ++i) out.push_back(buf_[(head_ + i) % kCapacity]);
  return out;
}

size_t LogTap::size() const {
  std::lock_guard l(mutex_);
  return buf_.size();
}

void LogTap::clear() {
  std::lock_guard l(mutex_);
  buf_.clear();
  head_ = 0;
}

LogTap& engineLogTap() {
  static LogTap tap;
  return tap;
}

bool logOpenFile(const std::string& path) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path p(path);
  std::lock_guard l(gLogMutex);
  if (gFile) { std::fclose(gFile); gFile = nullptr; }  // Windows cannot rename an open file
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  if (fs::exists(p, ec)) {
    fs::path prev = p; prev.replace_extension(".prev.log");
    fs::remove(prev, ec);
    fs::rename(p, prev, ec);
  }
#if defined(_WIN32)
  gFile = _wfsopen(p.c_str(), L"w", _SH_DENYWR);  // shared read: the log can be tailed while the game runs
#else
  gFile = std::fopen(path.c_str(), "w");
#endif
  return gFile != nullptr;
}

void logCloseFile() {
  std::lock_guard l(gLogMutex);
  if (gFile) { std::fclose(gFile); gFile = nullptr; }
}
}  // namespace litholattice
