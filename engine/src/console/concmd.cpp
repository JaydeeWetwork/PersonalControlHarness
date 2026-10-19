// Console language implementation: tokenizer, typed cvars, aliases, exec sandbox.
#include "litholattice/console/concmd.h"

#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>
#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "litholattice/core/log.h"
#include "litholattice/defrag/defrag.h"
#include "litholattice/input/input_types.h"

namespace litholattice::console {
namespace {

// Joins argv[1..] with single spaces.
std::string joinRest(const std::vector<std::string>& argv) {
  std::string out;
  for (size_t i = 1; i < argv.size(); ++i) {
    if (i > 1) out += ' ';
    out += argv[i];
  }
  return out;
}

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
  return true;
}

const char* typeName(CVarType t) {
  switch (t) {
    case CVarType::String: return "string";
    case CVarType::Int: return "int";
    case CVarType::Float: return "float";
    case CVarType::Bool: return "bool";
  }
  return "?";
}

// Escapes a value for the `name "value"` archived format.
std::string escapeValue(const std::string& v) {
  std::string out;
  for (char c : v) {
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out;
}

// Tiny printf-style formatter for command output (print takes one string).
template <typename... Args>
std::string format(const char* fmt, Args... args) {
  char buf[512];
  std::snprintf(buf, sizeof(buf), fmt, args...);
  return std::string(buf);
}

// Strict whole-string integer parse: no silent 0 on garbage (fail-closed).
bool parseInt64(const std::string& s, int64_t& out) {
  if (s.empty()) return false;
  size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
  if (i == s.size()) return false;
  int64_t v = 0;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    const int d = s[i] - '0';
    if (v > (INT64_MAX - d) / 10) return false;  // overflow: reject
    v = v * 10 + d;
  }
  out = s[0] == '-' ? -v : v;
  return true;
}

// Strict whole-string double parse (fail-closed like parseInt64).
bool parseDouble(const std::string& s, double& out) {
  if (s.empty()) return false;
  try {
    size_t n = 0;
    const double v = std::stod(s, &n);
    if (n != s.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
  } catch (...) {
    return false;
  }
}

#if defined(__linux__)
// TOCTOU-hardened open for executeFile: mirrors the storage::SandboxedFile
// gates (storage/sandboxed_io.h):
//   1. open() with O_NOFOLLOW — a trailing symlink fails closed with ELOOP
//      instead of being followed out of the config dir;
//   2. fstat — must be a regular file (no FIFOs, devices, directories);
//   3. read back the true path via /proc/self/fd/N and re-check containment
//      under the config dir (catches intermediate-component swaps that
//      O_NOFOLLOW cannot see).
// Returns an open FILE* (fclose() closes the fd), or nullptr + `why`.
// Note: symlinks are refused outright (fail-closed) — even ones pointing
// inside the config dir. A player symlinking their autoexec.lth elsewhere
// will see "exec: ... (symlink refused)".
FILE* openExecFileHardened(const std::filesystem::path& full,
                           const std::filesystem::path& base, std::string& why) {
  const int fd = ::open(full.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    why = std::string("cannot open '") + full.string() + "': " + std::strerror(errno) +
          (errno == ELOOP ? " (symlink refused)" : "");
    return nullptr;
  }
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    why = "refused '" + full.string() + "': not a regular file";
    ::close(fd);
    return nullptr;
  }
  char proc[64];
  std::snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
  char target[4096];
  const ssize_t len = ::readlink(proc, target, sizeof(target) - 1);
  if (len <= 0) {
    why = "cannot re-validate '" + full.string() + "'";
    ::close(fd);
    return nullptr;
  }
  target[len] = '\0';
  const std::string ts(target), bs = base.string();
  if (ts.size() < bs.size() || ts.compare(0, bs.size(), bs) != 0 ||
      (ts.size() > bs.size() && ts[bs.size()] != '/')) {
    why = "refused '" + full.string() + "': path escapes the config dir";
    ::close(fd);
    return nullptr;
  }
  FILE* f = ::fdopen(fd, "r");
  if (!f) {
    why = std::string("cannot open '") + full.string() + "': " + std::strerror(errno);
    ::close(fd);
    return nullptr;
  }
  return f;  // fclose() closes the fd
}
#endif

}  // namespace

namespace {
// Strips `/* ... */` block comments from `text` (they may span lines).
// Double-quoted strings and backslash escapes protect comment markers,
// exactly like ConsoleRegistry::tokenize(). `//` line comments pass through
// untouched — tokenize() still drops them per line — and a `/*` inside one
// is just comment text, never a block opener. On an unterminated `/*`
// returns false and sets `err` (callers refuse fail-closed); `out` is left
// unspecified on failure.
bool stripBlockComments(std::string_view text, std::string& out, std::string& err) {
  out.clear();
  out.reserve(text.size());
  enum class St { Code, Str, LineComment };
  St st = St::Code;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    switch (st) {
      case St::Str:
        out += c;
        if (c == '\\' && i + 1 < text.size())
          out += text[++i];
        else if (c == '"')
          st = St::Code;
        break;
      case St::LineComment:
        out += c;
        if (c == '\n') st = St::Code;
        break;
      case St::Code:
        if (c == '"') {
          st = St::Str;
          out += c;
        } else if (c == '\\' && i + 1 < text.size()) {
          out += c;
          out += text[++i];
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
          st = St::LineComment;
          out += "//";
          ++i;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
          // Block comment: everything until the first `*/` is dropped
          // (quotes and `//` inside are plain comment text).
          size_t j = i + 2;
          while (j + 1 < text.size() && !(text[j] == '*' && text[j + 1] == '/')) ++j;
          if (j + 1 >= text.size()) {
            err = "unterminated block comment";
            return false;
          }
          i = j + 1;  // resume on the `/` of `*/`
        } else {
          out += c;
        }
        break;
    }
  }
  return true;
}
}  // namespace

bool ConsoleRegistry::isValidName(std::string_view name) {
  if (name.empty() || name.size() > 64) return false;
  for (char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':';
    if (!ok) return false;
  }
  return true;
}

std::vector<std::vector<std::string>> ConsoleRegistry::tokenize(std::string_view line,
                                                                std::string* err) {
  std::vector<std::vector<std::string>> cmds;
  std::vector<std::string> cur;
  std::string tok;
  bool inQuotes = false, inBlock = false, haveTok = false;

  auto flushTok = [&] {
    if (haveTok) {
      cur.push_back(tok);
      tok.clear();
      haveTok = false;
    }
  };
  auto flushCmd = [&] {
    flushTok();
    if (!cur.empty()) {
      cmds.push_back(cur);
      cur.clear();
    }
  };

  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (inBlock) {
      // Inside a block comment everything is comment text (quotes and `//`
      // included) until the first `*/`.
      if (c == '*' && i + 1 < line.size() && line[i + 1] == '/') {
        inBlock = false;
        ++i;
      }
      continue;
    }
    if (inQuotes) {
      if (c == '\\' && i + 1 < line.size()) {
        tok += line[i + 1];
        ++i;
      } else if (c == '"') {
        inQuotes = false;
      } else {
        tok += c;
      }
      haveTok = true;
      continue;
    }
    if (c == '"') {
      inQuotes = true;
      haveTok = true;  // `""` is an empty argument, not "no argument"
    } else if (c == '\\' && i + 1 < line.size()) {
      tok += line[i + 1];  // `\;` / `\ ` escape the separator/space
      ++i;
      haveTok = true;
    } else if (c == ';') {
      flushCmd();
    } else if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
      break;  // `//` comment: rest of the line is dropped
    } else if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
      inBlock = true;  // `/*` comment: everything until `*/` is dropped
      ++i;
    } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      flushTok();
    } else {
      tok += c;
      haveTok = true;
    }
  }
  if (inBlock) {
    // Unterminated `/*`: a clean error, never silently eating the rest of
    // the input. The partial command list is discarded (fail-closed).
    if (err) *err = "unterminated block comment";
    return {};
  }
  flushCmd();
  return cmds;
}

std::vector<std::vector<std::string>> ConsoleRegistry::tokenize(std::string_view line) {
  std::string err;
  return tokenize(line, &err);  // documented: the error form's empty list, error dropped
}

std::vector<std::string> ConsoleRegistry::extractComments(std::string_view text,
                                                          std::string* err) {
  std::vector<std::string> out;
  bool inQuotes = false;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (inQuotes) {
      if (c == '\\' && i + 1 < text.size())
        ++i;  // escaped char: never a comment marker
      else if (c == '"')
        inQuotes = false;
      continue;
    }
    if (c == '"') {
      inQuotes = true;
    } else if (c == '\\' && i + 1 < text.size()) {
      ++i;  // escaped char is literal: `\/` cannot open a comment
    } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      // Line comment: body is the rest of the line, minus a trailing `\r`
      // (CRLF tolerance). The `\n` itself is not part of the body.
      size_t eol = text.find('\n', i + 2);
      if (eol == std::string_view::npos) eol = text.size();
      size_t end = eol;
      if (end > i + 2 && text[end - 1] == '\r') --end;
      out.emplace_back(text.substr(i + 2, end - (i + 2)));
      i = eol;  // the for-loop's ++i steps past the `\n` (or off the end)
    } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      // Block comment: body is the verbatim inner text (may span lines).
      // The first `*/` closes it; quotes and `//` inside are plain text.
      size_t j = i + 2;
      while (j + 1 < text.size() && !(text[j] == '*' && text[j + 1] == '/')) ++j;
      if (j + 1 >= text.size()) {
        if (err) *err = "unterminated block comment";
        return out;  // comments extracted so far are kept; the error is reported
      }
      out.emplace_back(text.substr(i + 2, j - (i + 2)));
      i = j + 1;  // resume on the `/` of `*/`
    }
  }
  return out;
}

bool ConsoleRegistry::coerce(CVarType type, std::string_view raw, std::string& canonical) {
  switch (type) {
    case CVarType::String:
      canonical = std::string(raw);
      return true;
    case CVarType::Int: {
      if (raw.empty()) return false;
      char* end = nullptr;
      // Fix 2026-10-11 (regression, LOW): strtol clamps huge input to
      // LONG_MAX/MIN with errno=ERANGE; without the check the clamped value
      // flows on and the settings change-fn's stoi throws -> silent no-op
      // with cvar/settings desync. Reject on ERANGE.
      errno = 0;
      const long v = std::strtol(std::string(raw).c_str(), &end, 10);
      if (errno == ERANGE) return false;
      if (end == nullptr || *end != '\0') return false;
      canonical = std::to_string(v);
      return true;
    }
    case CVarType::Float: {
      if (raw.empty()) return false;
      char* end = nullptr;
      errno = 0;  // same ERANGE hazard as Int above (strtod -> HUGE_VAL)
      const double v = std::strtod(std::string(raw).c_str(), &end);
      if (errno == ERANGE) return false;
      if (end == nullptr || *end != '\0') return false;
      char buf[64];
      std::snprintf(buf, sizeof buf, "%.9g", v);
      canonical = buf;
      return true;
    }
    case CVarType::Bool: {
      if (ieq(raw, "1") || ieq(raw, "true") || ieq(raw, "yes") || ieq(raw, "on")) {
        canonical = "1";
        return true;
      }
      if (ieq(raw, "0") || ieq(raw, "false") || ieq(raw, "no") || ieq(raw, "off")) {
        canonical = "0";
        return true;
      }
      return false;
    }
  }
  return false;
}

bool ConsoleRegistry::registerCommand(std::string name, std::string help, ConsoleCommandFn fn,
                                      std::string* err) {
  if (!isValidName(name)) {
    if (err) *err = "invalid command name '" + name + "'";
    return false;
  }
  if (commands_.count(name) || cvars_.count(name) || aliases_.count(name)) {
    if (err) *err = "name '" + name + "' is already taken";
    return false;
  }
  commands_.emplace(std::move(name), Command{std::move(help), std::move(fn), CommandFlags{}});
  return true;
}

bool ConsoleRegistry::registerCommand(std::string name, std::string help, ConsoleCommandFn fn,
                                      CommandFlags flags, std::string* err) {
  if (!isValidName(name)) {
    if (err) *err = "invalid command name '" + name + "'";
    return false;
  }
  if (commands_.count(name) || cvars_.count(name) || aliases_.count(name)) {
    if (err) *err = "name '" + name + "' is already taken";
    return false;
  }
  commands_.emplace(std::move(name), Command{std::move(help), std::move(fn), flags});
  return true;
}

bool ConsoleRegistry::unregisterCommand(const std::string& name) {
  return commands_.erase(name) > 0;
}

bool ConsoleRegistry::hasCommand(const std::string& name) const { return commands_.count(name) > 0; }

std::string ConsoleRegistry::commandHelp(const std::string& name) const {
  const auto it = commands_.find(name);
  return it == commands_.end() ? std::string() : it->second.help;
}

std::string ConsoleRegistry::aliasBody(const std::string& name) const {
  const auto it = aliases_.find(name);
  return it == aliases_.end() ? std::string() : it->second.body;
}

bool ConsoleRegistry::registerCVar(std::string name, CVarType type, std::string defaultValue,
                                   bool archived, std::string help, std::string* err) {
  return registerCVar(std::move(name), type, std::move(defaultValue), archived, std::move(help),
                      CVarFlags{}, err);
}

bool ConsoleRegistry::registerCVar(std::string name, CVarType type, std::string defaultValue,
                                    bool archived, std::string help, CVarFlags flags,
                                    std::string* err) {
  if (!isValidName(name)) {
    if (err) *err = "invalid cvar name '" + name + "'";
    return false;
  }
  if (commands_.count(name) || cvars_.count(name) || aliases_.count(name)) {
    if (err) *err = "name '" + name + "' is already taken";
    return false;
  }
  std::string canonical;
  if (!coerce(type, defaultValue, canonical)) {
    if (err) *err = "default value does not coerce to " + std::string(typeName(type));
    return false;
  }
  CVar cv;
  cv.name = name;
  cv.type = type;
  cv.value = canonical;
  cv.defaultValue = canonical;
  cv.archived = archived;
  cv.help = std::move(help);
  cv.readOnly = flags.readOnly;
  cv.replicated = flags.replicated;
  cv.cheat = flags.cheat;
  cvars_.emplace(std::move(name), std::move(cv));
  return true;
}

bool ConsoleRegistry::unregisterCVar(const std::string& name) { return cvars_.erase(name) > 0; }

const CVar* ConsoleRegistry::findCVar(const std::string& name) const {
  const auto it = cvars_.find(name);
  return it == cvars_.end() ? nullptr : &it->second;
}

bool ConsoleRegistry::setCVar(const std::string& name, const std::string& rawValue, std::string* err) {
  const auto it = cvars_.find(name);
  if (it == cvars_.end()) {
    if (err) *err = "unknown cvar '" + name + "'";
    return false;
  }
  if (it->second.readOnly) {
    if (err) *err = "cvar '" + name + "' is read-only";
    return false;
  }
  if (it->second.cheat && !callerHasCheatAuthority()) {
    if (err) *err = "cvar '" + name + "' requires cheats (not allowed in this context)";
    return false;
  }
  std::string canonical;
  if (!coerce(it->second.type, rawValue, canonical)) {
    if (err) *err = "value '" + rawValue + "' is not a " + typeName(it->second.type);
    return false;
  }
  it->second.value = std::move(canonical);
  if (it->second.onChange) it->second.onChange(*this, it->second);
  return true;
}

bool ConsoleRegistry::setCVarChangeFn(const std::string& name, CVarChangeFn fn) {
  const auto it = cvars_.find(name);
  if (it == cvars_.end()) return false;
  it->second.onChange = std::move(fn);
  return true;
}

std::vector<std::string> ConsoleRegistry::commandNames() const {
  std::vector<std::string> out;
  for (const auto& [name, _] : commands_) out.push_back(name);
  return out;
}

std::vector<const CVar*> ConsoleRegistry::cvars() const {
  std::vector<const CVar*> out;
  for (const auto& [_, cv] : cvars_) out.push_back(&cv);
  return out;
}

bool ConsoleRegistry::setAlias(std::string name, std::string body, std::string* err) {
  if (!isValidName(name)) {
    if (err) *err = "invalid alias name '" + name + "'";
    return false;
  }
  if (commands_.count(name) || cvars_.count(name)) {
    if (err) *err = "name '" + name + "' is already taken";
    return false;
  }
  // Stamp the definer's authority (mirrors setKeyBind): an alias planted
  // under a restricted mask keeps that mask at expansion time, so the body
  // can never execute at a later invoker's wider authority (Appendix F H1).
  aliases_[std::move(name)] = Alias{std::move(body), callerPermissions_};
  return true;
}

bool ConsoleRegistry::removeAlias(const std::string& name) { return aliases_.erase(name) > 0; }
bool ConsoleRegistry::hasAlias(const std::string& name) const { return aliases_.count(name) > 0; }

std::vector<std::string> ConsoleRegistry::aliasNames() const {
  std::vector<std::string> out;
  for (const auto& [name, _] : aliases_) out.push_back(name);
  return out;
}

bool ConsoleRegistry::setKeyBind(std::string keyName, std::string command, std::string* err) {
  const auto key = keyFromName(keyName);
  if (!key) {
    if (err) *err = "unknown key '" + keyName + "'";
    return false;
  }
  const bool overwrite = keyBinds_.count(keyName) > 0;
  // Stamp the binder's authority: a bind planted under a restricted mask
  // keeps that mask at dispatch (fail-closed against laundering).
  KeyBind bind{std::move(command), *key, callerPermissions_};
  const std::string loggedName = keyName;
  const std::string loggedCmd = bind.command;
  keyBinds_[std::move(keyName)] = std::move(bind);
  print("bind: '" + loggedName + "' -> \"" + loggedCmd + "\"" +
        (overwrite ? " (overwrote previous bind)" : ""));
  return true;
}

bool ConsoleRegistry::removeKeyBind(const std::string& keyName) {
  return keyBinds_.erase(keyName) > 0;
}

std::map<std::string, std::string> ConsoleRegistry::keyBinds() const {
  std::map<std::string, std::string> out;
  for (const auto& [name, bind] : keyBinds_) out.emplace(name, bind.command);
  return out;
}

void ConsoleRegistry::executeLine(std::string_view line) {
  // A command fn may call executeLine re-entrantly: save/restore the
  // transient execution state around the nested invocation. A nested
  // suspension queues its own continuation independently. The caller
  // permission mask is saved/restored too: a command fn that installs a
  // mask for its own duration (script console bindings, the MCP IConsoleExec
  // adapter) must not leak mask changes made by nested commands, and a
  // nested invocation must not widen the authority it was entered with.
  // RAII transient-state guard: a throwing command fn must not corrupt
  // execDepth_/currentCtx_/callerPermissions_/ticking_ for the next command.
  struct TransientGuard {
    ConsoleRegistry* self;
    ExecCtx* savedCtx;
    WaitUnwind savedWait;
    uint32_t savedMask;
    ~TransientGuard() {
      self->currentCtx_ = savedCtx;
      self->callerPermissions_ = savedMask;
      self->waitUnwind_ = std::move(savedWait);
    }
  };
  ExecCtx* savedCtx = currentCtx_;
  WaitUnwind savedWait = std::move(waitUnwind_);
  waitUnwind_ = WaitUnwind{};
  const uint32_t savedMask = callerPermissions_;
  TransientGuard guard{this, savedCtx, std::move(savedWait), savedMask};
  ExecCtx ctx;
  currentCtx_ = &ctx;
  // F-L5: use the error-reporting tokenize overload — the one-arg form drops
  // the error, so an unterminated block comment (`echo hi /* oops`) used to
  // run nothing and print nothing on interactive lines. The file path
  // (executeFile) reports the same error; the interactive path does now too.
  std::string tokErr;
  auto cmds = tokenize(line, &tokErr);
  if (!tokErr.empty()) {
    print(tokErr);
    return;
  }
  const ExecResult r = executeCommandList(std::move(cmds), 0, ctx);
  guard.self->currentCtx_ = savedCtx;
  guard.self->callerPermissions_ = savedMask;
  if (r == ExecResult::Suspended && waitUnwind_.active) {
    SuspendedScript s;
    s.cmds = std::move(waitUnwind_.pending);
    s.ticksLeft = waitUnwind_.ticks;
    s.aliasDepth = waitUnwind_.fireDepth;
    s.stepsUsed = ctx.stepsUsed;
    s.totalWaited = ctx.totalWaited;
    queueSuspended(std::move(s));
  }
  // Aborted: the termination message was already printed; nothing to keep.
  // Suspended-without-active-unwind: a nested executeLine already queued it.
  // (TransientGuard restores currentCtx_/callerPermissions_/waitUnwind_.)
}

ConsoleRegistry::ExecResult ConsoleRegistry::executeCommandList(
    const std::vector<std::vector<std::string>>& cmds, int aliasDepth, ExecCtx& ctx) {
  const int savedDepth = currentAliasDepth_;
  currentAliasDepth_ = aliasDepth;
  ExecResult r = ExecResult::Ok;
  for (size_t i = 0; i < cmds.size(); ++i) {
    r = executeTokens(cmds[i], aliasDepth, ctx);
    if (r == ExecResult::Suspended && waitUnwind_.active) {
      // A wait is unwinding: capture this frame's remainder after the
      // already-captured inner remainder (innermost first, in order).
      waitUnwind_.pending.insert(waitUnwind_.pending.end(),
                                 std::make_move_iterator(cmds.begin() + i + 1),
                                 std::make_move_iterator(cmds.end()));
      break;
    }
    if (r != ExecResult::Ok) break;  // Aborted, or a nested suspension already queued
  }
  currentAliasDepth_ = savedDepth;
  return r;
}

ConsoleRegistry::ExecResult ConsoleRegistry::executeTokens(const std::vector<std::string>& argv,
                                                           int aliasDepth, ExecCtx& ctx) {
  if (argv.empty()) return ExecResult::Ok;
  // Per-invocation step budget: bounds per-tick CPU no matter how the
  // script is shaped. Fail-closed with a printed message.
  if (++ctx.stepsUsed > kMaxScriptSteps) {
    print("lithoscript: step budget exceeded (" + std::to_string(kMaxScriptSteps) +
          " commands); script terminated");
    return ExecResult::Aborted;
  }
  const std::string& name = argv[0];

  // ++/-- shorthand: `<cvar>++` / `<cvar>--` (±1; int/float cvars only).
  // '+'/'-' are not valid name characters, so this cannot collide with a
  // registered command, cvar, or alias.
  if (name.size() > 2 && name[name.size() - 1] == name[name.size() - 2] &&
      (name.back() == '+' || name.back() == '-')) {
    return execIncDec(name.substr(0, name.size() - 2), name.back() == '+', argv);
  }

  // Aliases expand textually (extra args are appended, Quake-style), with a
  // recursion cap for `alias loop "loop"` style accidents.
  const auto aliasIt = aliases_.find(name);
  if (aliasIt != aliases_.end()) {
    if (aliasDepth >= kMaxAliasDepth) {
      print("alias recursion too deep (expanding '" + name + "')");
      return ExecResult::Ok;
    }
    std::string expanded = aliasIt->second.body;
    const std::string rest = joinRest(argv);
    if (!rest.empty()) {
      expanded += ' ';
      expanded += rest;
    }
    // Run the body under the DEFINER's authority composed with the current
    // (invoker's) mask — most restrictive wins (see concmd.h setAlias).
    // Nested alias expansions compose again against this already-narrowed
    // mask, so authority only ever narrows as expansion descends. The mask
    // is restored on every exit; if a `wait` suspends the body, cmdWait has
    // already captured this composed mask into the unwind state, so the
    // continuation resumes under it too (queueSuspended prefers it).
    const uint32_t savedMask = callerPermissions_;
    callerPermissions_ = composeCallerMasks(aliasIt->second.callerMask, savedMask);
    const ExecResult r = executeCommandList(tokenize(expanded), aliasDepth + 1, ctx);
    callerPermissions_ = savedMask;
    return r;
  }

  // Cvars: `name` prints, `name value` sets (typed).
  const auto cvarIt = cvars_.find(name);
  if (cvarIt != cvars_.end()) {
    if (argv.size() == 1) {
      print("\"" + name + "\" = \"" + cvarIt->second.value + "\" (" + typeName(cvarIt->second.type) +
            (cvarIt->second.archived ? ", archived" : "") + ")");
    } else {
      std::string err;
      if (!setCVar(name, joinRest(argv), &err)) print(err);
    }
    return ExecResult::Ok;
  }

  const auto cmdIt = commands_.find(name);
  if (cmdIt != commands_.end()) {
    if (cmdIt->second.flags.cheat && !callerHasCheatAuthority()) {
      print(format("command '%s' requires cheats (not allowed in this context)", argv[0].c_str()));
      return ExecResult::Ok;
    }
    // Session tier (P1b, Appendix F M6): destructive/session commands that
    // are not cheat-flagged (quit/save/map/debug.defrag) still refuse any
    // restricted caller mask. Cheat-gate semantics above are untouched.
    if (cmdIt->second.flags.session && !callerHasSessionAuthority()) {
      print(format("command '%s' requires session authority (not allowed in this context)",
                   argv[0].c_str()));
      return ExecResult::Ok;
    }
    cmdIt->second.fn(*this, argv);
    // The `wait` builtin signals through waitUnwind_ (see cmdWait): a
    // suspension unwinds cooperatively, a budget trip aborts fail-closed.
    if (waitUnwind_.aborted) {
      waitUnwind_.aborted = false;
      return ExecResult::Aborted;
    }
    if (waitUnwind_.active) return ExecResult::Suspended;
    return ExecResult::Ok;
  }

  print("Unknown command \"" + name + "\" (try \"help\")");
  return ExecResult::Ok;
}

void ConsoleRegistry::cmdWait(const ConsoleArgs& argv) {
  ExecCtx* ctx = currentCtx_;
  if (!ctx) {  // defensive: wait only makes sense inside an execution
    print("wait: no execution context");
    return;
  }
  long n = 1;  // bare `wait` waits 1 tick (HL1 muscle memory)
  if (argv.size() > 1) {
    const char* s = argv[1].c_str();
    char* end = nullptr;
    n = std::strtol(s, &end, 10);
    if (end == s || *end != '\0' || n < 0) {
      print("usage: wait [N]  (N >= 0 world ticks; default 1)");
      return;
    }
  }
  if (n == 0) return;  // documented no-op: no suspension (still counts as a step)
  int ticks = n > kMaxWaitTicks ? kMaxWaitTicks : static_cast<int>(n);
  if (n > kMaxWaitTicks)
    print("wait: clamped to " + std::to_string(kMaxWaitTicks) +
          " ticks (1 second of game time at 512 TPS)");
  // Total-wait budget: accumulated across every wait AND every suspension
  // of this invocation, so wait-ping-pongs cannot run forever.
  if (ctx->totalWaited + ticks > kMaxTotalWaitTicks) {
    print("lithoscript: total wait budget exceeded (" + std::to_string(kMaxTotalWaitTicks) +
          " ticks); script terminated");
    waitUnwind_.aborted = true;
    return;
  }
  ctx->totalWaited += ticks;
  // Cooperative suspension: capture-and-queue happens as this unwinds
  // through the executeCommandList frames (see executeCommandList). Nothing
  // blocks here; the engine thread is never held.
  waitUnwind_.active = true;
  waitUnwind_.ticks = ticks;
  waitUnwind_.fireDepth = currentAliasDepth_;
  // Capture the authority in effect HERE — inside any alias expansions,
  // callerPermissions_ is already the composed definer/invoker mask — so
  // the queued continuation resumes under exactly this authority even
  // though the alias frames restore the invoker's mask while unwinding.
  waitUnwind_.callerMask = callerPermissions_;
  waitUnwind_.hasCallerMask = true;
}

void ConsoleRegistry::queueSuspended(SuspendedScript s) {
  if (suspended_.size() >= static_cast<size_t>(kMaxSuspendedScripts)) {    print("lithoscript: too many suspended scripts (" + std::to_string(kMaxSuspendedScripts) +
          "); script terminated");
    return;
  }
  // Authority survives the suspension (Phase 4 seam). Prefer the mask the
  // `wait` captured at fire time (composed definer/invoker mask inside
  // alias expansions); fall back to the ambient mask for suspensions that
  // did not pass through cmdWait.
  s.callerMask = waitUnwind_.hasCallerMask ? waitUnwind_.callerMask : callerPermissions_;
  suspended_.push_back(std::move(s));
}

bool ConsoleRegistry::bumpCVar(const std::string& name, double delta, bool clamp, double lo,
                               double hi) {
  const auto it = cvars_.find(name);
  if (it == cvars_.end()) return false;
  if (it->second.type != CVarType::Int && it->second.type != CVarType::Float) return false;
  // F-L6: fail closed on non-finite input. incrementvar parses its delta with
  // raw strtod, which accepts "nan"/"inf"; the old code mapped a NaN result
  // to 0 for int cvars (silently ZEROING the cvar on `incrementvar myint nan`)
  // and let non-finite values flow into float cvars' canonical form. Refuse:
  // the cvar is left untouched and the caller prints its usage/error line.
  if (!std::isfinite(delta)) return false;
  char* end = nullptr;
  const double cur = std::strtod(it->second.value.c_str(), &end);
  if (end == nullptr || *end != '\0') return false;  // unreachable: canonical values parse
  if (!std::isfinite(cur)) return false;  // a cvar already holding inf/nan is not bumpable
  double nv = cur + delta;
  if (!std::isfinite(nv)) return false;  // overflow to inf: fail closed, no silent clamp
  if (clamp) nv = std::min(hi, std::max(lo, nv));
  std::string raw;
  if (it->second.type == CVarType::Int) {
    // Saturate instead of bare std::llround: llround is UB for out-of-range
    // inputs. nv is finite here (checked above), but can still exceed
    // long long range with a huge finite delta.
    long long q;
    if (nv >= static_cast<double>(LLONG_MAX)) {
      q = LLONG_MAX;
    } else if (nv <= static_cast<double>(LLONG_MIN)) {
      q = LLONG_MIN;
    } else {
      q = std::llround(nv);  // finite and in range: exact
    }
    raw = std::to_string(q);
  } else {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.9g", nv);
    raw = buf;
  }
  std::string err;
  return setCVar(name, raw, &err);  // fires onChange
}

ConsoleRegistry::ExecResult ConsoleRegistry::execIncDec(const std::string& cvarName, bool inc,
                                                        const ConsoleArgs& argv) {
  if (argv.size() != 1) {
    print("usage: <cvar>++ / <cvar>--   (no arguments)");
    return ExecResult::Ok;
  }
  if (!bumpCVar(cvarName, inc ? 1.0 : -1.0, false, 0.0, 0.0)) {
    if (cvars_.find(cvarName) == cvars_.end())
      print("Unknown command \"" + cvarName + (inc ? "++" : "--") + "\" (try \"help\")");
    else
      print(std::string("cannot ") + (inc ? "increment" : "decrement") + " '" + cvarName +
            "': not an int/float cvar");
  }
  return ExecResult::Ok;
}

void ConsoleRegistry::dispatchKeyBinds(const DeviceState& devices, bool textCapture) {
  if (textCapture || keyBinds_.empty()) return;
  for (const auto& [_, bind] : keyBinds_) {
    if (!devices.keyPressed(bind.key)) continue;  // key parsed once at setKeyBind
    // Run under the BINDER's authority, never the dispatcher's: a bind
    // planted by a restricted context (agent/mod script) keeps its restricted
    // mask here, so cheat-gated commands stay refused (fail-closed against
    // laundering across time). executeLine() save/restores the mask around
    // the nested call, so this restore only covers the install itself.
    const uint32_t savedMask = callerPermissions_;
    callerPermissions_ = bind.callerMask;
    executeLine(bind.command);
    callerPermissions_ = savedMask;
  }
}

void ConsoleRegistry::tick() {
  if (ticking_) {
    // A resumed command must not pump the queue it came from.
    print("lithoscript: tick() re-entered; ignoring");
    return;
  }
  ticking_ = true;
  for (size_t i = 0; i < suspended_.size();) {
    if (--suspended_[i].ticksLeft > 0) {
      ++i;
      continue;
    }
    SuspendedScript resume = std::move(suspended_[i]);
    suspended_.erase(suspended_.begin() + static_cast<ptrdiff_t>(i));
    // No ++i: erase shifts the next entry into i, and a re-suspend below
    // appends at the back, so every entry is visited exactly once.
    ExecCtx* savedCtx = currentCtx_;
    WaitUnwind savedWait = std::move(waitUnwind_);
    waitUnwind_ = WaitUnwind{};
    ExecCtx ctx;
    ctx.stepsUsed = resume.stepsUsed;      // budgets survive the suspension
    ctx.totalWaited = resume.totalWaited;
    currentCtx_ = &ctx;
    const uint32_t savedMask = callerPermissions_;  // authority survives too (Phase 4 seam)
    callerPermissions_ = resume.callerMask;
    const ExecResult r = executeCommandList(resume.cmds, resume.aliasDepth, ctx);
    currentCtx_ = savedCtx;
    if (r == ExecResult::Suspended && waitUnwind_.active) {
      SuspendedScript ns;
      ns.cmds = std::move(waitUnwind_.pending);
      ns.ticksLeft = waitUnwind_.ticks;
      ns.aliasDepth = waitUnwind_.fireDepth;
      ns.stepsUsed = ctx.stepsUsed;
      ns.totalWaited = ctx.totalWaited;
      // queueSuspended() stamps callerPermissions_ into ns.callerMask, so the
      // ambient restore MUST happen after this: otherwise a re-suspended script
      // resumes with ambient authority (permission laundering via `wait`).
      queueSuspended(std::move(ns));
    }
    callerPermissions_ = savedMask;
    // Aborted: the termination message was already printed; drop it.
    waitUnwind_ = std::move(savedWait);
  }
  // Background maintenance last: a defrag session does bounded work per pump,
  // so a ten-year Fragment never stalls the sim.
  if (defragService_) defragService_->pump();
  ticking_ = false;
}

bool ConsoleRegistry::executeFile(std::string_view name) {
  const std::string n(name);
  // Exec-chain depth cap: nesting at/past the cap is refused fail-closed.
  // The bound is enforced at every entry; synchronous a.lth -> b.lth ->
  // a.lth cycles terminate here, and wait-laced cycles (which unwind past
  // this point via suspension) terminate at kMaxTotalWaitTicks instead.
  if (execDepth_ >= kMaxExecDepth) {
    print("exec: refused '" + n + "': script nesting too deep (limit " +
          std::to_string(kMaxExecDepth) + "; possible cycle)");
    return false;
  }
  if (n.empty() || n[0] == '/' || n[0] == '\\' || n.find("..") != std::string::npos ||
      (n.size() >= 2 && n[1] == ':')) {
    print("exec: refused '" + n + "': path escapes the config dir");
    return false;
  }
  const bool isLth = n.size() >= 4 && n.compare(n.size() - 4, 4, ".lth") == 0;
  const bool isCfg = n.size() >= 4 && n.compare(n.size() - 4, 4, ".cfg") == 0;
  if (!isLth && !isCfg) {
    print("exec: refused '" + n + "': only .lth script files may be executed (.cfg is a legacy alias)");
    return false;
  }
  if (configDir_.empty()) {
    print("exec: no config dir set");
    return false;
  }
  std::error_code ec;
  const std::filesystem::path base = std::filesystem::weakly_canonical(configDir_, ec);
  if (ec) {
    print("exec: cannot resolve the config dir");
    return false;
  }
  // NOTE: do NOT weakly_canonical() base / n here — canonicalization resolves a
  // trailing symlink, which would defeat the O_NOFOLLOW gate below (the open
  // would see the resolved target, not the link). `n` is already validated as
  // a bare filename (no separators, no ".."), so base / n is lexically one
  // level under base; the hardened open itself validates what was opened.
  const std::filesystem::path full = base / n;
  // Hardened open: a resolve-then-open containment check here would be TOCTOU
  // (a symlink swap between check and open escapes the sandbox), so the open
  // itself goes through O_NOFOLLOW + fstat + /proc/self/fd re-validation on
  // Linux — the same three gates as storage::SandboxedFile.
  std::string contents;
#if defined(__linux__)
  {
    std::string why;
    FILE* f = openExecFileHardened(full, base, why);
    if (!f) {
      print("exec: " + why);
      return false;
    }
    char buf[8192];
    size_t nr = 0;
    while ((nr = std::fread(buf, 1, sizeof buf, f)) > 0) contents.append(buf, nr);
    const bool readErr = std::ferror(f) != 0;
    std::fclose(f);
    if (readErr) {
      print("exec: error reading '" + n + "'");
      return false;
    }
  }
#else
  {
    // Non-Linux: `n` is a validated bare filename so base / n is lexically
    // under the config dir, but without O_NOFOLLOW a trailing symlink is
    // followed (see storage/sandboxed_io.h) — the TOCTOU window is
    // documented, not closed, off Linux.
    std::ifstream in(full, std::ios::binary);
    if (!in) {
      print("exec: cannot open '" + n + "'");
      return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    contents = ss.str();
  }
#endif
  // Quake semantics: the file becomes part of the current command stream.
  // Lines are accumulated and run as ONE command list sharing the ambient
  // invocation's budget, so a `wait` inside the file suspends the rest of
  // the file too (and later lines of the same file). The depth counter is
  // released on every exit including suspension: temporal exec ping-pongs
  // (a.lth -wait-> b.lth -wait-> a.lth) are bounded by kMaxTotalWaitTicks,
  // while the counter still bounds synchronous C++ stack nesting.
  //
  // Block comments are stripped across the WHOLE file before line
  // splitting: a `/* ... */` comment may span lines, and per-line
  // tokenizing could not see that. `//` line comments are left for
  // tokenize() per line. An unterminated block comment refuses the whole
  // file fail-closed (prints the reason) — never silently eating the rest
  // of the script.
  {
    std::string stripped, stripErr;
    if (!stripBlockComments(contents, stripped, stripErr)) {
      print("exec: refused '" + n + "': " + stripErr);
      return false;
    }
    contents = std::move(stripped);
  }
  ++execDepth_;
  // RAII: a throwing executeCommandList must not leak execDepth_ or currentCtx_.
  struct ExecDepthGuard {
    ConsoleRegistry* self;
    ExecCtx* savedCtx;
    ~ExecDepthGuard() { --self->execDepth_; self->currentCtx_ = savedCtx; }
  };
  std::vector<std::vector<std::string>> all;
  {
    // Split exactly like std::getline would: '\n'-terminated chunks plus a
    // final unterminated chunk; no phantom empty line for a trailing '\n'.
    size_t pos = 0;
    while (pos < contents.size()) {
      const size_t nl = contents.find('\n', pos);
      const size_t len = (nl == std::string::npos ? contents.size() : nl) - pos;
      auto cmds = tokenize(std::string_view(contents.data() + pos, len));
      all.insert(all.end(), std::make_move_iterator(cmds.begin()),
                 std::make_move_iterator(cmds.end()));
      if (nl == std::string::npos) break;
      pos = nl + 1;
    }
  }
  ExecCtx local;
  ExecCtx* const savedCtx = currentCtx_;
  ExecDepthGuard depthGuard{this, savedCtx};
  if (!currentCtx_) currentCtx_ = &local;
  const ExecResult r = executeCommandList(all, 0, *currentCtx_);
  // depthGuard restores execDepth_ + currentCtx_ at scope exit (also on throw).
  if (r == ExecResult::Suspended && !savedCtx) {
    // Top-level call (tests/tools): take ownership of the continuation.
    // Nested calls (via the `exec` builtin) leave waitUnwind_ active so the
    // suspension propagates to the enclosing executeLine, which queues it.
    SuspendedScript s;
    s.cmds = std::move(waitUnwind_.pending);
    s.ticksLeft = waitUnwind_.ticks;
    s.aliasDepth = waitUnwind_.fireDepth;
    s.stepsUsed = local.stepsUsed;
    s.totalWaited = local.totalWaited;
    // Queue BEFORE clearing the unwind state: queueSuspended stamps the
    // wait-captured (composed) caller mask from waitUnwind_ when present.
    queueSuspended(std::move(s));
    waitUnwind_ = WaitUnwind{};
  } else if (!savedCtx) {
    waitUnwind_ = WaitUnwind{};  // no stale unwind state leaks past the top
  }
  return r != ExecResult::Aborted;
}

bool ConsoleRegistry::saveArchived(const std::filesystem::path& path, std::string* err) const {
  const auto fail = [&](const std::string& m) {
    if (err) *err = m;
    return false;
  };
  // Sandboxed, like exec (mod_interface.h: "exec/saveArchived stay
  // sandboxed"): the target must resolve under the config dir. Refuse
  // fail-closed when no config dir is set — an unsandboxed archived write is
  // never acceptable, and loadArchived (the old unsandboxed read twin) is
  // gone: archived cvars boot back through the sandboxed executeFile path.
  if (configDir_.empty())
    return fail("saveArchived: no config dir set (refusing unsandboxed write)");
  std::error_code ec;
  const std::filesystem::path base = std::filesystem::weakly_canonical(configDir_, ec);
  if (ec) return fail("saveArchived: cannot resolve the config dir");
  const std::filesystem::path full =
      std::filesystem::weakly_canonical(path.is_absolute() ? path : base / path, ec);
  if (ec) return fail("saveArchived: cannot resolve '" + path.string() + "'");
  const std::string bs = base.string(), fs = full.string();
  if (fs.size() < bs.size() || fs.compare(0, bs.size(), bs) != 0 ||
      (fs.size() > bs.size() && fs[bs.size()] != '/' && fs[bs.size()] != '\\')) {
    return fail("saveArchived: refused '" + path.string() + "': path escapes the config dir");
  }
  const std::filesystem::path tmp = std::filesystem::path(full.string() + ".tmp");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return fail("cannot open '" + tmp.string() + "' for writing");
    out << "// lithoscript archived cvars (written by the console; edit freely)\n";
    for (const auto& [_, cv] : cvars_) {
      if (!cv.archived) continue;
      out << cv.name << " \"" << escapeValue(cv.value) << "\"\n";
    }
    out.flush();
    if (!out) return fail("failed writing '" + tmp.string() + "'");
  }
  std::filesystem::rename(tmp, full, ec);
  if (ec) return fail("failed renaming to '" + full.string() + "': " + ec.message());
  return true;
}

void ConsoleRegistry::registerBuiltins(bool debugBuild) {
  registerCommand("help", "help [command]: list commands, or describe one", [](ConsoleRegistry& r, const ConsoleArgs& argv) {
    if (argv.size() > 1) {
      const auto it = r.commands_.find(argv[1]);
      if (it != r.commands_.end()) {
        r.print(argv[1] + ": " + it->second.help);
        return;
      }
      if (const CVar* cv = r.findCVar(argv[1])) {
        r.print("\"" + cv->name + "\" = \"" + cv->value + "\" (" + typeName(cv->type) + "): " + cv->help);
        return;
      }
      r.print("Unknown command \"" + argv[1] + "\"");
      return;
    }
    std::string line = "commands:";
    for (const auto& [name, _] : r.commands_) line += " " + name;
    r.print(line);
    r.print("(cvars are set with `name value` and read with `name`; try \"cvarlist\")");
  });

  registerCommand("echo", "echo ...: prints its arguments", [](ConsoleRegistry& r, const ConsoleArgs& argv) {
    r.print(joinRest(argv));
  });

  registerCommand("alias", "alias <name> \"cmd1; cmd2\": define a named command sequence (no args: list)",
                  [](ConsoleRegistry& r, const ConsoleArgs& argv) {
                    if (argv.size() == 1) {
                      if (r.aliases_.empty()) r.print("(no aliases)");
                      for (const auto& [name, a] : r.aliases_) r.print("alias " + name + " \"" + a.body + "\"");
                      return;
                    }
                    if (argv.size() == 2) {
                      const auto it = r.aliases_.find(argv[1]);
                      r.print(it == r.aliases_.end() ? "no alias \"" + argv[1] + "\""
                                                    : "alias " + argv[1] + " \"" + it->second.body + "\"");
                      return;
                    }
                    std::string err;
                    // The body is argv[2..] — NOT joinRest(argv) (argv[1..]),
                    // which folded the alias NAME into its own body and made
                    // every builtin-defined alias self-referential (expanding
                    // it recursed to the alias cap without ever running the
                    // intended commands; found by the P1b laundering repro,
                    // 2026-10-13). Join from index 2 with single spaces.
                    std::string body;
                    for (size_t i = 2; i < argv.size(); ++i) {
                      if (i > 2) body += ' ';
                      body += argv[i];
                    }
                    if (!r.setAlias(argv[1], body, &err)) r.print(err);
                  });

  registerCommand("unalias", "unalias <name>: removes an alias", [](ConsoleRegistry& r, const ConsoleArgs& argv) {
    if (argv.size() < 2) {
      r.print("usage: unalias <name>");
      return;
    }
    if (!r.removeAlias(argv[1])) r.print("no alias \"" + argv[1] + "\"");
  });

  registerCommand("exec", "exec <name.lth>: runs a lithoscript script from the config dir (sandboxed; .cfg still works as a legacy alias)",
                  [](ConsoleRegistry& r, const ConsoleArgs& argv) {
                    if (argv.size() < 2) {
                      r.print("usage: exec <name.lth>");
                      return;
                    }
                    r.executeFile(argv[1]);
                  });

  registerCommand("wait", "wait [N]: suspends the rest of the script for N world ticks (default 1; max 512, clamped; wait 0 = no-op)",
                  [](ConsoleRegistry& r, const ConsoleArgs& argv) { r.cmdWait(argv); });

  registerCommand("toggle", "toggle <cvar> [value...]: flip a bool/int cvar 0/1, or cycle through the given values",
                  [](ConsoleRegistry& r, const ConsoleArgs& argv) {
                    if (argv.size() < 2) {
                      r.print("usage: toggle <cvar> [value...]");
                      return;
                    }
                    const CVar* cv = r.findCVar(argv[1]);
                    if (!cv) {
                      r.print("toggle: unknown cvar '" + argv[1] + "'");
                      return;
                    }
                    std::string err;
                    if (argv.size() == 2) {
                      if (cv->type != CVarType::Bool && cv->type != CVarType::Int) {
                        r.print("toggle: '" + cv->name +
                                "' is not a bool/int cvar (give values to cycle)");
                        return;
                      }
                      if (!r.setCVar(cv->name, cv->value == "0" ? "1" : "0", &err)) r.print(err);
                      return;
                    }
                    std::vector<std::string> vals;
                    for (size_t i = 2; i < argv.size(); ++i) {
                      std::string c;
                      if (!coerce(cv->type, argv[i], c)) {
                        r.print("toggle: '" + argv[i] + "' is not a " + std::string(typeName(cv->type)));
                        return;
                      }
                      vals.push_back(std::move(c));
                    }
                    size_t idx = vals.size() - 1;  // current not listed -> start at vals[0]
                    for (size_t i = 0; i < vals.size(); ++i)
                      if (vals[i] == cv->value) {
                        idx = i;
                        break;
                      }
                    if (!r.setCVar(cv->name, vals[(idx + 1) % vals.size()], &err)) r.print(err);
                  });

  registerCommand("incrementvar",
                  "incrementvar <cvar> <min> <max> <delta>: add delta to an int/float cvar, clamped to [min,max]",
                  [](ConsoleRegistry& r, const ConsoleArgs& argv) {
                    if (argv.size() != 5) {
                      r.print("usage: incrementvar <cvar> <min> <max> <delta>");
                      return;
                    }
                    const CVar* cv = r.findCVar(argv[1]);
                    if (!cv) {
                      r.print("incrementvar: unknown cvar '" + argv[1] + "'");
                      return;
                    }
                    if (cv->type != CVarType::Int && cv->type != CVarType::Float) {
                      r.print("incrementvar: '" + cv->name + "' is not an int/float cvar");
                      return;
                    }
                    char* end = nullptr;
                    const double lo = std::strtod(argv[2].c_str(), &end);
                    if (*end) {
                      r.print("incrementvar: min is not a number");
                      return;
                    }
                    const double hi = std::strtod(argv[3].c_str(), &end);
                    if (*end) {
                      r.print("incrementvar: max is not a number");
                      return;
                    }
                    const double d = std::strtod(argv[4].c_str(), &end);
                    if (*end) {
                      r.print("incrementvar: delta is not a number");
                      return;
                    }
                    if (lo > hi) {
                      r.print("incrementvar: min > max");
                      return;
                    }
                    if (!r.bumpCVar(cv->name, d, true, lo, hi))
                      r.print("incrementvar: failed (cvar vanished mid-command?)");
                  });

  registerCommand("find", "find <text>: lists commands, cvars and aliases containing <text> (case-insensitive)",
                  [](ConsoleRegistry& r, const ConsoleArgs& argv) {
                    if (argv.size() < 2) {
                      r.print("usage: find <text>");
                      return;
                    }
                    const std::string& needle = argv[1];
                    auto hit = [&](std::string_view hay) {
                      if (hay.size() < needle.size()) return false;
                      for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
                        bool eq = true;
                        for (size_t j = 0; j < needle.size(); ++j)
                          if (std::tolower((unsigned char)hay[i + j]) !=
                              std::tolower((unsigned char)needle[j])) {
                            eq = false;
                            break;
                          }
                        if (eq) return true;
                      }
                      return false;
                    };
                    bool any = false;
                    for (const auto& name : r.commandNames())
                      if (hit(name)) {
                        r.print("command: " + name);
                        any = true;
                      }
                    for (const CVar* cv : r.cvars())
                      if (hit(cv->name)) {
                        r.print("cvar: \"" + cv->name + "\" = \"" + cv->value + "\"");
                        any = true;
                      }
                    for (const auto& name : r.aliasNames())
                      if (hit(name)) {
                        r.print("alias: " + name);
                        any = true;
                      }
                    if (!any) r.print("(no matches for \"" + needle + "\")");
                  });

  registerCommand("unbindall", "unbindall: removes all key binds", [](ConsoleRegistry& r, const ConsoleArgs&) {
    r.clearKeyBinds();
  });

  registerCommand("cvarlist", "cvarlist: lists all cvars with types, values, flags and archive state",
                  [](ConsoleRegistry& r, const ConsoleArgs&) {
                    for (const CVar* cv : r.cvars()) {
                      std::string flags;
                      if (cv->archived) flags += ", archived";
                      if (cv->readOnly) flags += ", readonly";
                      if (cv->replicated) flags += ", replicated";
                      if (cv->cheat) flags += ", cheat";
                      r.print("\"" + cv->name + "\" = \"" + cv->value + "\" (" + typeName(cv->type) +
                              flags + "): " + cv->help);
                    }
                  });

  registerCommand("cmdlist", "cmdlist: lists all commands with their help text", [](ConsoleRegistry& r, const ConsoleArgs&) {
    for (const auto& [name, cmd] : r.commands_) r.print(name + ": " + cmd.help);
  });

  registerCommand("bind", "bind <key> \"command\": binds a key to a command (dispatched by the input pump; not while typing)",
                  [](ConsoleRegistry& r, const ConsoleArgs& argv) {
                    if (argv.size() == 1) {
                      const auto binds = r.keyBinds();
                      if (binds.empty()) r.print("(no key binds)");
                      for (const auto& [key, cmd] : binds) r.print(key + " -> \"" + cmd + "\"");
                      return;
                    }
                    if (argv.size() == 2) {
                      const auto binds = r.keyBinds();
                      const auto it = binds.find(argv[1]);
                      r.print(it == binds.end() ? "no bind on \"" + argv[1] + "\""
                                               : argv[1] + " -> \"" + it->second + "\"");
                      return;
                    }
                    std::string err;
                    if (!r.setKeyBind(argv[1], joinRest(argv), &err)) r.print(err);
                  });

  registerCommand("unbind", "unbind <key>: removes a key bind", [](ConsoleRegistry& r, const ConsoleArgs& argv) {
    if (argv.size() < 2) {
      r.print("usage: unbind <key>");
      return;
    }
    if (!r.removeKeyBind(argv[1])) r.print("no bind on \"" + argv[1] + "\"");
  });

  registerCommand("version", "version: prints the lithoscript/engine version", [](ConsoleRegistry& r, const ConsoleArgs&) {
    r.print("lithoscript console (SDK 1)");
  });

  // debug.defrag — the engine defrag/restructure service (JD 2026-10-11).
  // General service, Fragment first client: compact/reorder/reclaim, chunked
  // across ticks, crash-safe commit, reindex on completion. The game registers
  // its stores at boot (registerStore); without one the command says so.
  // Session tier (P1b, Appendix F M6): starting/cancelling a defrag mutates
  // engine storage state, so restricted callers (agents/mods) cannot run
  // this at all — not even `status` (one flag for the whole command keeps
  // the gate simple and fail-closed).
  registerCommand("debug.defrag",
                  "debug.defrag [<store>|status|cancel]: defrag a registered store "
                  "(no args: status)",
                  [](ConsoleRegistry& r, const ConsoleArgs& argv) {
                    defrag::DefragService* d = r.defragService();
                    if (!d) {
                      r.print("debug.defrag: no defrag service registered");
                      return;
                    }
                    if (argv.size() < 2 || argv[1] == "status") {
                      r.print(d->statusLine());
                      return;
                    }
                    if (argv[1] == "cancel") {
                      d->cancel();
                      r.print("debug.defrag: cancelled");
                      return;
                    }
                    if (d->beginDefrag(argv[1])) {
                      r.print("debug.defrag: started on '" + argv[1] + "' (" +
                              d->statusLine() + ")");
                    } else {
                      r.print("debug.defrag: cannot start: " + d->lastError());
                    }
                  },
                  CommandFlags{false, true});

  if (debugBuild) {
    // Debug builds get the extra verbose introspection commands.
    registerCommand("dbg_logtap", "dbg_logtap: engine-log tap stats (debug builds only)",
                    [](ConsoleRegistry& r, const ConsoleArgs&) {
                      const LogTap& tap = engineLogTap();
                      r.print("log tap: " + std::to_string(tap.size()) + "/" +
                              std::to_string(LogTap::kCapacity) + " records");
                    });
    registerCommand("dbg_registry", "dbg_registry: console registry counts (debug builds only)",
                    [](ConsoleRegistry& r, const ConsoleArgs&) {
                      r.print("registry: " + std::to_string(r.commands_.size()) + " commands, " +
                              std::to_string(r.cvars_.size()) + " cvars, " +
                              std::to_string(r.aliases_.size()) + " aliases, " +
                              std::to_string(r.keyBinds_.size()) + " key binds");
                    });
  }

  // query — "SQL for voxels", the primitive agents/proxies use to SEE the
  // world. Region bounds are MANDATORY (fail-closed: missing coords = usage
  // error, never an unbounded scan); the backend caps cells and results.
  // Provisional syntax (flaggable for JD):
  //   query blocks <blockId> <x1> <y1> <z1> <x2> <y2> <z2>
  //   query count  <blockId> <x1> <y1> <z1> <x2> <y2> <z2>
  //   query entities <type|*> <cx> <cy> <cz> <radius>
  registerCommand("query",
                  "query blocks|count <blockId> <x1> <y1> <z1> <x2> <y2> <z2> | "
                  "query entities <type|*> <cx> <cy> <cz> <radius> — read-only world queries "
                  "(bounds mandatory)",
                  [](ConsoleRegistry& r, const ConsoleArgs& argv) {
                    IWorldQuery* q = r.worldQuery();
                    if (!q) {
                      r.print("query: no world backend registered");
                      return;
                    }
                    if (argv.size() < 2) {
                      r.print("usage: query blocks|count <blockId> <x1> <y1> <z1> <x2> <y2> <z2>");
                      r.print("       query entities <type|*> <cx> <cy> <cz> <radius>");
                      return;
                    }
                    const std::string& sub = argv[1];
                    if (sub == "blocks" || sub == "count") {
                      if (argv.size() != 9) {
                        r.print(format("usage: query %s <blockId> <x1> <y1> <z1> <x2> <y2> <z2>  "
                                "(all six bounds required)", sub.c_str()));
                        return;
                      }
                      // Strict integer parsing: any non-numeric bound is a usage
                      // error, never a silent 0 (fail-closed).
                      int64_t v[7];
                      for (int i = 0; i < 7; ++i) {
                        if (!parseInt64(argv[2 + i], v[i])) {
                          r.print(format("query %s: '%s' is not an integer", sub.c_str(), argv[2 + i].c_str()));
                          return;
                        }
                      }
                      if (v[0] < 0 || v[0] > UINT32_MAX) {
                        r.print(format("query %s: blockId out of range", sub.c_str()));
                        return;
                      }
                      // Mirror the MCP world_query_blocks range check: parseInt64
                      // accepts up to INT64_MAX, so reject out-of-int32 bounds
                      // BEFORE narrowing (a silent wrap would feed the backend
                      // garbage and poison the kMaxQueryCells guard).
                      for (int i = 1; i < 7; ++i) {
                        if (v[i] < INT32_MIN || v[i] > INT32_MAX) {
                          r.print(format("query %s: coordinate out of range", sub.c_str()));
                          return;
                        }
                      }
                      const int x1 = static_cast<int>(std::min(v[1], v[4]));
                      const int x2 = static_cast<int>(std::max(v[1], v[4]));
                      const int y1 = static_cast<int>(std::min(v[2], v[5]));
                      const int y2 = static_cast<int>(std::max(v[2], v[5]));
                      const int z1 = static_cast<int>(std::min(v[3], v[6]));
                      const int z2 = static_cast<int>(std::max(v[3], v[6]));
                      // int64 span arithmetic: x2 - x1 can overflow int even
                      // with int32 bounds (INT32_MAX - INT32_MIN), which was
                      // signed-overflow UB in the guard itself.
                      const uint64_t cells =
                          static_cast<uint64_t>(static_cast<int64_t>(x2) - x1 + 1) *
                          static_cast<uint64_t>(static_cast<int64_t>(y2) - y1 + 1) *
                          static_cast<uint64_t>(static_cast<int64_t>(z2) - z1 + 1);
                      if (cells > kMaxQueryCells) {
                        r.print(format("query %s: region too large (%llu cells > %llu max)", sub.c_str(),
                                static_cast<unsigned long long>(cells),
                                static_cast<unsigned long long>(kMaxQueryCells)));
                        return;
                      }
                      std::string err;
                      WorldQueryResult res = sub == "blocks"
                                                 ? q->queryBlocks(static_cast<uint32_t>(v[0]), x1, y1, z1,
                                                                  x2, y2, z2, &err)
                                                 : q->queryCount(static_cast<uint32_t>(v[0]), x1, y1, z1,
                                                                 x2, y2, z2, &err);
                      if (!err.empty()) {
                        r.print(format("query %s: %s", sub.c_str(), err.c_str()));
                        return;
                      }
                      r.print(format("query %s: %llu match%s in %llu cells%s", sub.c_str(),
                              static_cast<unsigned long long>(res.totalMatches),
                              res.totalMatches == 1 ? "" : "es",
                              static_cast<unsigned long long>(res.cellsScanned),
                              res.truncated ? " (truncated)" : ""));
                      if (sub == "blocks") {
                        for (const auto& h : res.blocks)
                          r.print(format("  (%d, %d, %d)", h.x, h.y, h.z));
                      }
                      return;
                    }
                    if (sub == "entities") {
                      if (argv.size() != 7) {
                        r.print("usage: query entities <type|*> <cx> <cy> <cz> <radius>");
                        return;
                      }
                      double c[4];
                      for (int i = 0; i < 4; ++i) {
                        if (!parseDouble(argv[3 + i], c[i])) {
                          r.print(format("query entities: '%s' is not a number", argv[3 + i].c_str()));
                          return;
                        }
                      }
                      if (!(c[3] > 0) || c[3] > 1000000.0) {
                        r.print("query entities: radius must be positive and sane");
                        return;
                      }
                      std::string err;
                      WorldQueryResult res =
                          q->queryEntities(argv[2], c[0], c[1], c[2], c[3], &err);
                      if (!err.empty()) {
                        r.print(format("query entities: %s", err.c_str()));
                        return;
                      }
                      r.print(format("query entities: %llu match%s%s", static_cast<unsigned long long>(res.totalMatches),
                              res.totalMatches == 1 ? "" : "es",
                              res.truncated ? " (truncated)" : ""));
                      for (const auto& h : res.entities)
                        r.print(format("  #%llu %s (%.1f, %.1f, %.1f)",
                                static_cast<unsigned long long>(h.id),
                                h.typeName.empty() ? "?" : h.typeName.c_str(), h.x, h.y, h.z));
                      return;
                    }
                    r.print("usage: query blocks|count ... | query entities ...");
                  });
}

}  // namespace litholattice::console
