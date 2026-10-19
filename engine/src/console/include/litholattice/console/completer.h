// completer.h: registry-agnostic tab-completion + autocomplete filtering for
// the lithoscript console language. Pure logic, STL only, no UI dependency —
// it is deliberately NOT tangled into the console widget so a future Fragment
// script editor can reuse it by implementing ICompletionSource.
//
// FRAGMENT SEAM: the completer takes ALL of its candidates from the injected
// ICompletionSource. The debug console feeds it a RegistryCompletionSource
// (below); the Fragment script editor later implements its own source (e.g.
// lithoscript keywords plus Fragment-specific commands) and gets identical
// completion behavior with zero console code.
//
// COMPLETION RULES (HL1-style, documented + tested):
//   * Prefix match over the source's candidates, case-sensitive (console
//     names are lowercase by registration convention).
//   * complete(): the longest common prefix of the matches replaces the
//     typed prefix; with exactly one match a trailing space is appended.
//     No matches -> no-op (the input line is returned unchanged).
//     An empty prefix -> no matches (nothing to complete).
//   * filter(): the live dropdown rows for the current line. Returns empty
//     when there is nothing useful to show: empty prefix, no matches, or a
//     single match whose name already equals the typed prefix.
//   * Context completion: when the first token is `exec` or `map` and a
//     space follows it, completion runs over the ARGUMENT instead —
//     `exec` completes script filenames (via the injected, sandboxed
//     file-lister), `map` completes dimension names (via the injected
//     dimension provider).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace litholattice::console {

class ConsoleRegistry;  // concmd.h

/// Kind of a completion candidate. Command/Cvar/Alias come from the console
/// registry; File/Dimension only appear after `exec` / `map`.
enum class CompletionKind : uint8_t { Command, Cvar, Alias, File, Dimension };

/// Short display tag for a kind ("cmd", "cvar", "alias", "file", "dim").
const char* completionKindTag(CompletionKind kind);

/// One completion candidate: `name` is the text inserted on accept.
struct CompletionCandidate {
  std::string name;
  CompletionKind kind = CompletionKind::Command;
  std::string description;  ///< help text (commands/cvars), truncated alias body, "" otherwise
  std::string detail;       ///< cvar CURRENT VALUE; "" for everything else
};

/// Injected candidate source — this interface IS the Fragment seam.
/// Implementations must be cheap to call (candidates() runs per keystroke).
class ICompletionSource {
 public:
  virtual ~ICompletionSource() = default;
  /// All first-token candidates: commands, cvars, aliases (unsorted ok —
  /// the Completer sorts deterministically).
  virtual std::vector<CompletionCandidate> candidates() = 0;
  /// Script filenames offered after `exec <prefix>`. SANDBOXED by contract:
  /// the implementer lists inside its script root only and returns bare
  /// filenames (no directories, no "..", no absolute paths).
  virtual std::vector<std::string> scriptFiles(std::string_view prefix) = 0;
  /// Dimension names offered after `map <prefix>`.
  virtual std::vector<std::string> dimensionNames(std::string_view prefix) = 0;
};

/// Result of one HL1-style Tab completion.
struct CompletionResult {
  std::string completedLine;  ///< full input line with the completed token spliced in
  std::vector<CompletionCandidate> matches;
  /// True when there is exactly one match and the typed prefix already
  /// equals its name: the caller should append a trailing space.
  bool singleExact = false;
};

/// How a line splits for completion purposes.
struct CompletionSplit {
  std::string_view head;  ///< first token ("" when the line is empty/blank)
  std::string_view rest;  ///< text after the first run of spaces ("" when none)
  bool hasSpace = false;  ///< a space followed the head -> context completion may apply
};

class Completer {
 public:
  explicit Completer(ICompletionSource& source) : source_(source) {}

  /// Splits a line the way completion does (exposed so UI layers and the
  /// Fragment editor share one parser).
  static CompletionSplit splitCompletionLine(std::string_view line);

  /// HL1-style Tab completion of `line`. Never throws; no matches (or an
  /// empty prefix) -> completedLine == line and matches empty.
  CompletionResult complete(std::string_view line);

  /// Live dropdown rows for `line` (VS Code-style filtering). Empty when
  /// there is nothing useful to show: empty prefix, no matches, or one
  /// match that already equals the typed prefix. Rows are sorted by name
  /// (kind breaks ties) for a stable, predictable list.
  std::vector<CompletionCandidate> filter(std::string_view line);

  /// Longest common prefix of candidate names ("" when empty).
  static std::string longestCommonPrefix(const std::vector<CompletionCandidate>& matches);

  /// True for the first-party commands that take a completable argument
  /// (`exec` -> files, `map` -> dimensions).
  static bool isContextCommand(std::string_view head);

 private:
  ICompletionSource& source_;
};

/// ICompletionSource backed by a ConsoleRegistry: commands (help as the
/// description), cvars (help + CURRENT VALUE as the detail), aliases
/// (body truncated to 64 chars as the description). exec's file lister is
/// sandboxed to the script root (defaults to the registry's config dir —
/// the same directory `exec` itself reads); dimension names default to the
/// five LithoKraft dimensions and can be overridden by the game.
class RegistryCompletionSource : public ICompletionSource {
 public:
  explicit RegistryCompletionSource(const ConsoleRegistry& registry);

  /// Overrides the script root (default: the registry's config dir, read
  /// lazily so setConfigDir() after construction still takes effect). The
  /// listing never leaves this directory: non-recursive, basenames only.
  void setScriptRoot(std::filesystem::path root) {
    scriptRoot_ = std::move(root);
    scriptRootSet_ = true;
  }
  std::filesystem::path scriptRoot() const;

  /// Overrides the built-in dimension list (wafer, underwafer, organic,
  /// space, void). The game installs its own provider here.
  void setDimensionProvider(std::function<std::vector<std::string>()> provider) {
    dimensionProvider_ = std::move(provider);
  }

  std::vector<CompletionCandidate> candidates() override;
  std::vector<std::string> scriptFiles(std::string_view prefix) override;
  std::vector<std::string> dimensionNames(std::string_view prefix) override;

 private:
  const ConsoleRegistry& registry_;
  std::filesystem::path scriptRoot_;
  bool scriptRootSet_ = false;
  std::function<std::vector<std::string>()> dimensionProvider_;
};

}  // namespace litholattice::console
