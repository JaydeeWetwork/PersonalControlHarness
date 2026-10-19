// delimiter_pairing.cpp: block-comment delimiter pairing for lithoscript
// text fields. The scanner below is the ConsoleRegistry::tokenize /
// extractComments state machine, restated for positional queries: it must
// produce IDENTICAL delimiter decisions (see the cross-check tests in
// console_selftest.cpp).
#include "litholattice/console/delimiter_pairing.h"

namespace litholattice::console {
namespace {

// Fail-closed input cap. Editor text fields never legitimately approach
// this size; anything larger is degenerate input (e.g. a runaway paste)
// and is refused without allocating or scanning. All public functions
// below check this first and return their safe default.
constexpr size_t kMaxInputBytes = 16u * 1024u * 1024u;  // 16 MiB

enum class LexState { Normal, InString, InBlock };

struct ScanResult {
  LexState endState = LexState::Normal;
  bool endInLineComment = false;
  size_t realOpens = 0;   // `/*` transitions Normal -> InBlock
  size_t realCloses = 0;  // `*/` transitions InBlock -> Normal
  size_t loneCloses = 0;  // `*/` seen in Normal state (tokenizer-inert,
                          // but lone for paste pairing)
  size_t lastOpenPos = std::string_view::npos;  // index of the latest real `/*`
};

// One pass with the tokenizer-identical rules. `init`/`initLine` let pasted
// text continue the lexical context it lands in.
ScanResult scanLex(std::string_view text, LexState init, bool initLine) {
  ScanResult r;
  LexState st = init;
  bool inLine = initLine;
  const size_t n = text.size();
  size_t i = 0;
  while (i < n) {
    const char c = text[i];
    if (inLine) {
      // `//` comment: everything is inert until `\n`.
      if (c == '\n') inLine = false;
      ++i;
      continue;
    }
    if (st == LexState::InBlock) {
      // Inside a block comment everything is comment text (quotes and
      // `//` included) until the first `*/`. No nesting: `/*` here is inert.
      if (c == '*' && i + 1 < n && text[i + 1] == '/') {
        st = LexState::Normal;
        ++r.realCloses;
        i += 2;
      } else {
        ++i;
      }
      continue;
    }
    if (st == LexState::InString) {
      if (c == '\\' && i + 1 < n) {
        i += 2;  // escaped char: never a marker
      } else if (c == '"') {
        st = LexState::Normal;
        ++i;
      } else {
        ++i;
      }
      continue;
    }
    // Normal state.
    if (c == '"') {
      st = LexState::InString;
      ++i;
    } else if (c == '\\' && i + 1 < n) {
      i += 2;  // escaped char is literal: `\/` cannot open a comment
    } else if (c == '/' && i + 1 < n && text[i + 1] == '/') {
      inLine = true;
      i += 2;
    } else if (c == '/' && i + 1 < n && text[i + 1] == '*') {
      st = LexState::InBlock;
      ++r.realOpens;
      r.lastOpenPos = i;
      i += 2;
    } else if (c == '*' && i + 1 < n && text[i + 1] == '/') {
      // Literal text to the tokenizer, but a lone delimiter for pairing.
      ++r.loneCloses;
      i += 2;
    } else {
      ++i;
    }
  }
  r.endState = st;
  r.endInLineComment = inLine;
  return r;
}

// True when the trailing `/*` of `before` is a REAL opener under the
// lexical rules. Scans the whole text so escapes (`\/*`), strings
// (`"/*`), line comments (`// /*`) and already-open blocks (`/* /*`)
// are all classified exactly like the tokenizer classifies them.
bool trailingOpenerIsReal(std::string_view before) {
  const size_t n = before.size();
  if (n < 2 || before[n - 2] != '/' || before[n - 1] != '*') return false;
  const ScanResult r = scanLex(before, LexState::Normal, false);
  return r.lastOpenPos == n - 2;
}

}  // namespace

ScanEndState scanEndState(std::string_view text) {
  // Fail closed on degenerate input: report Normal, scan nothing.
  if (text.size() > kMaxInputBytes) return ScanEndState::Normal;
  const ScanResult r = scanLex(text, LexState::Normal, false);
  if (r.endInLineComment) return ScanEndState::InLineComment;
  switch (r.endState) {
    case LexState::InString: return ScanEndState::InString;
    case LexState::InBlock: return ScanEndState::InBlock;
    case LexState::Normal: break;
  }
  return ScanEndState::Normal;
}

AutoCloseResult autoCloseOnType(std::string_view before, std::string_view after) {
  AutoCloseResult r;
  // Fail closed on degenerate input: refuse BEFORE allocating or scanning.
  // applied=false, so the editor (per contract) applies nothing.
  if (before.size() > kMaxInputBytes || after.size() > kMaxInputBytes) {
    r.text.clear();
    r.cursor = 0;
    r.applied = false;
    return r;
  }
  r.text = std::string(before) + std::string(after);
  r.cursor = before.size();
  r.applied = false;
  if (!trailingOpenerIsReal(before)) return r;
  // Pair it: cursor stays between the delimiters, inside the new block.
  r.text = std::string(before) + "*/" + std::string(after);
  r.applied = true;
  return r;
}

SkipOverResult trySkipOverCloser(std::string_view before, std::string_view after,
                                 const std::unordered_set<size_t>& autoInsertedClosers) {
  SkipOverResult r;
  // Fail closed on degenerate input: refuse BEFORE allocating or scanning.
  // skipped=false, so the editor (per contract) applies nothing. (The
  // closer set needs no guard: this function performs a single lookup.)
  if (before.size() > kMaxInputBytes || after.size() > kMaxInputBytes) {
    r.skipped = false;
    r.text.clear();
    r.cursor = 0;
    return r;
  }
  r.skipped = false;
  r.text = std::string(before) + std::string(after);
  r.cursor = before.size();
  // Gate 0: the editor calls this right after the user typed `/`
  // completing a `*/`, so `before` must end with those two characters.
  const size_t n = before.size();
  if (n < 2 || before[n - 2] != '*' || before[n - 1] != '/') return r;
  const std::string_view beforeBase = before.substr(0, n - 2);
  // Gate 1: a closer immediately after the cursor, to skip over.
  if (after.size() < 2 || after[0] != '*' || after[1] != '/') return r;
  // Gate 2: that closer is one WE auto-inserted. Its absolute start
  // offset is the cursor position (before.size()). A `*/` the user
  // typed or pasted deliberately is never in the set, so typing `*/`
  // before it inserts literally — no hijack, ever.
  if (autoInsertedClosers.find(n) == autoInsertedClosers.end()) return r;
  // Gate 3: lexical context — the cursor must sit inside an open block
  // comment. In particular a string (where `*/` is literal text) never
  // skips, even with a stale set entry.
  if (scanEndState(beforeBase) != ScanEndState::InBlock) return r;
  // Skip: drop the just-typed `*/`, advance past the existing closer.
  r.skipped = true;
  r.text = std::string(beforeBase) + std::string(after);
  r.cursor = beforeBase.size() + 2;
  return r;
}

std::string balancePastedDelimiters(std::string_view pasted, std::string_view contextBefore) {
  // Fail closed on degenerate input: return the paste byte-identical
  // (no pairing, no corruption) rather than scanning absurd sizes.
  if (pasted.size() > kMaxInputBytes || contextBefore.size() > kMaxInputBytes)
    return std::string(pasted);
  // 1. Lexical context at the paste cursor.
  const ScanResult ctx = scanLex(contextBefore, LexState::Normal, false);
  LexState init = ctx.endState;
  bool initLine = ctx.endInLineComment;
  // 2. Walk the pasted text from that context. Delimiters met while
  //    inside a string, line comment, or an already-open block are inert.
  const ScanResult p = scanLex(pasted, init, initLine);

  // 3. Unmatched inner openers. The outer block (when the context ends
  //    inside one) contributes depth 1; the FIRST real close, if any,
  //    closes it — `/*` cannot nest, so any `/*` before that close was
  //    inert comment text.
  const bool outerOpen = (init == LexState::InBlock);
  const bool outerClosed = outerOpen && p.realCloses > 0;
  const size_t depthEnd = (outerOpen ? 1 : 0) + p.realOpens - p.realCloses;
  const size_t outerDepth = (outerOpen && !outerClosed) ? 1 : 0;
  const size_t unmatchedOpens = depthEnd > outerDepth ? depthEnd - outerDepth : 0;

  if (p.loneCloses == 0 && unmatchedOpens == 0) return std::string(pasted);

  // Lone closers get their openers prepended (the common case is pasting
  // the tail of a comment block: `/*` at the front reconstructs it);
  // lone openers get closers appended (always lands inside the block,
  // so each appended `*/` is a real closer).
  std::string out;
  out.reserve(pasted.size() + 2 * (p.loneCloses + unmatchedOpens));
  for (size_t k = 0; k < p.loneCloses; ++k) out += "/*";
  out += pasted;
  for (size_t k = 0; k < unmatchedOpens; ++k) out += "*/";
  return out;
}

}  // namespace litholattice::console
