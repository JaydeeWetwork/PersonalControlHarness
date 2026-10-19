// completer.cpp: registry-agnostic tab-completion + autocomplete filtering.
#include "litholattice/console/completer.h"

#include <algorithm>

#include "litholattice/console/concmd.h"

namespace litholattice::console {
namespace {

bool startsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool isSpace(char c) { return c == ' ' || c == '\t'; }

int kindOrder(CompletionKind k) { return static_cast<int>(k); }

}  // namespace

const char* completionKindTag(CompletionKind kind) {
  switch (kind) {
    case CompletionKind::Command: return "cmd";
    case CompletionKind::Cvar: return "cvar";
    case CompletionKind::Alias: return "alias";
    case CompletionKind::File: return "file";
    case CompletionKind::Dimension: return "dim";
  }
  return "?";
}

CompletionSplit Completer::splitCompletionLine(std::string_view line) {
  CompletionSplit s;
  size_t i = 0;
  while (i < line.size() && isSpace(line[i])) ++i;
  const size_t headStart = i;
  while (i < line.size() && !isSpace(line[i])) ++i;
  s.head = line.substr(headStart, i - headStart);
  const size_t spaceStart = i;
  while (i < line.size() && isSpace(line[i])) ++i;
  s.hasSpace = i > spaceStart;
  s.rest = line.substr(i);
  return s;
}

bool Completer::isContextCommand(std::string_view head) {
  return head == "exec" || head == "map";
}

std::string Completer::longestCommonPrefix(const std::vector<CompletionCandidate>& matches) {
  if (matches.empty()) return {};
  std::string common = matches[0].name;
  for (size_t i = 1; i < matches.size(); ++i) {
    const std::string& name = matches[i].name;
    size_t n = 0;
    while (n < common.size() && n < name.size() && common[n] == name[n]) ++n;
    common.resize(n);
    if (common.empty()) break;
  }
  return common;
}

namespace {

// First-token matches over the source's candidates, sorted by name (kind
// breaks ties) for a stable, predictable list.
std::vector<CompletionCandidate> tokenMatches(ICompletionSource& source, std::string_view prefix) {
  std::vector<CompletionCandidate> out;
  for (auto& c : source.candidates())
    if (startsWith(c.name, prefix)) out.push_back(std::move(c));
  std::sort(out.begin(), out.end(), [](const CompletionCandidate& a, const CompletionCandidate& b) {
    if (a.name != b.name) return a.name < b.name;
    return kindOrder(a.kind) < kindOrder(b.kind);
  });
  return out;
}

// Argument matches after `exec` / `map`.
std::vector<CompletionCandidate> contextMatches(ICompletionSource& source, std::string_view head,
                                               std::string_view prefix) {
  std::vector<CompletionCandidate> out;
  if (head == "exec") {
    for (const std::string& f : source.scriptFiles(prefix)) {
      CompletionCandidate c;
      c.name = f;
      c.kind = CompletionKind::File;
      c.description = "script file";
      out.push_back(std::move(c));
    }
  } else if (head == "map") {
    for (const std::string& d : source.dimensionNames(prefix)) {
      CompletionCandidate c;
      c.name = d;
      c.kind = CompletionKind::Dimension;
      c.description = "dimension";
      out.push_back(std::move(c));
    }
  }
  return out;  // sources return these sorted already
}

}  // namespace

CompletionResult Completer::complete(std::string_view line) {
  CompletionResult r;
  r.completedLine = std::string(line);
  const CompletionSplit s = splitCompletionLine(line);
  if (s.head.empty()) return r;  // empty prefix: no-op

  std::vector<CompletionCandidate> matches;
  std::string token;  // the text being completed
  size_t tokenPos = 0;
  size_t tokenLen = 0;
  if (s.hasSpace && isContextCommand(s.head)) {
    // Completing the argument: "exec <prefix>".
    token = std::string(s.rest);
    tokenPos = size_t(s.rest.data() - line.data());
    tokenLen = s.rest.size();
    matches = contextMatches(source_, s.head, s.rest);
  } else {
    // Completing the first token; anything after it is preserved.
    token = std::string(s.head);
    tokenPos = size_t(s.head.data() - line.data());
    tokenLen = s.head.size();
    matches = tokenMatches(source_, s.head);
  }
  if (matches.empty()) return r;  // no matches: no-op

  r.matches = matches;
  const std::string common = longestCommonPrefix(matches);
  r.completedLine = std::string(line.substr(0, tokenPos)) + common +
                    std::string(line.substr(tokenPos + tokenLen));
  r.singleExact = matches.size() == 1 && matches[0].name == token;
  return r;
}

std::vector<CompletionCandidate> Completer::filter(std::string_view line) {
  const CompletionSplit s = splitCompletionLine(line);
  if (s.head.empty()) return {};  // empty prefix: show nothing
  std::string token;
  std::vector<CompletionCandidate> rows;
  if (s.hasSpace && isContextCommand(s.head)) {
    token = std::string(s.rest);
    rows = contextMatches(source_, s.head, s.rest);
  } else {
    token = std::string(s.head);
    rows = tokenMatches(source_, s.head);
  }
  if (rows.empty()) return {};
  if (rows.size() == 1 && rows[0].name == token) return {};  // nothing left to show
  return rows;
}

// ---- RegistryCompletionSource ----

RegistryCompletionSource::RegistryCompletionSource(const ConsoleRegistry& registry)
    : registry_(registry) {}

std::filesystem::path RegistryCompletionSource::scriptRoot() const {
  return scriptRootSet_ ? scriptRoot_ : registry_.configDir();
}

std::vector<CompletionCandidate> RegistryCompletionSource::candidates() {
  std::vector<CompletionCandidate> out;
  for (const std::string& name : registry_.commandNames()) {
    CompletionCandidate c;
    c.name = name;
    c.kind = CompletionKind::Command;
    c.description = registry_.commandHelp(name);
    out.push_back(std::move(c));
  }
  for (const CVar* v : registry_.cvars()) {
    CompletionCandidate c;
    c.name = v->name;
    c.kind = CompletionKind::Cvar;
    c.description = v->help;
    c.detail = v->value;  // read live: always the CURRENT value
    out.push_back(std::move(c));
  }
  for (const std::string& name : registry_.aliasNames()) {
    CompletionCandidate c;
    c.name = name;
    c.kind = CompletionKind::Alias;
    std::string body = registry_.aliasBody(name);
    if (body.size() > 64) body = body.substr(0, 61) + "...";
    c.description = std::move(body);
    out.push_back(std::move(c));
  }
  return out;
}

std::vector<std::string> RegistryCompletionSource::scriptFiles(std::string_view prefix) {
  std::vector<std::string> out;
  const std::filesystem::path root = scriptRoot();  // lazy: tracks setConfigDir()
  if (root.empty()) return out;
  // SANDBOX: non-recursive listing of the script root only; basenames are
  // returned, so no ".." or absolute path can ever escape through here.
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec)) continue;
    const std::string name = entry.path().filename().string();
    if (!startsWith(name, prefix)) continue;
    // exec's own accepted set: .lth scripts plus the legacy .cfg alias.
    const std::string ext = entry.path().extension().string();
    if (ext != ".lth" && ext != ".cfg") continue;
    out.push_back(name);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> RegistryCompletionSource::dimensionNames(std::string_view prefix) {
  std::vector<std::string> names = dimensionProvider_
                                       ? dimensionProvider_()
                                       : std::vector<std::string>{"organic", "cosmic", "underwafer",
                                                                  "void", "wafer"};
  std::vector<std::string> out;
  for (const std::string& n : names)
    if (startsWith(n, prefix)) out.push_back(n);
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace litholattice::console
