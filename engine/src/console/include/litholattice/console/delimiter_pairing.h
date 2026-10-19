// delimiter_pairing.h: block-comment delimiter pairing for lithoscript
// text fields (the future Fragment script editor first among them).
// Pure logic, STL only, no UI dependency — the editor calls these on
// typing/paste events and applies the returned edits itself.
//
// FRAGMENT SEAM: like the Completer, this unit knows nothing about
// widgets. The Fragment editor calls autoCloseOnType() when the user
// types the `*` completing a `/*`, trySkipOverCloser() when the user
// types the `/` completing a `*/`, and balancePastedDelimiters() on
// paste; all return plain data the editor splices in.
//
// LEXICAL RULES (identical to ConsoleRegistry::tokenize /
// extractComments — same state machine, same outcomes):
//   * `/*` opens a block comment; the FIRST `*/` closes it (no nesting).
//   * `//` starts a line comment running to `\n`; delimiters inside it
//     are inert.
//   * Markers inside double-quoted strings or right after a backslash
//     escape are literal text, never delimiters.
//   * `*/` outside a block comment is literal text to the TOKENIZER —
//     but for PASTE balancing it counts as a lone delimiter (see below),
//     because a pasted `*/` means the user copied half a comment block.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>

namespace litholattice::console {

/// Lexical end-state of a text scan, exposed so tests (and editor logic)
/// can assert the paste context explicitly.
enum class ScanEndState : unsigned char {
  Normal,        ///< ordinary code text
  InString,      ///< inside an unterminated double-quoted string
  InBlock,       ///< inside an unterminated `/* ... */` block comment
  InLineComment  ///< inside a `//` comment with no `\n` after it
};

/// Scans `text` with the exact tokenizer lexical rules and reports the
/// state at the end. Never throws. Inputs larger than 16 MiB report
/// Normal without scanning (fail-closed degenerate-input guard).
ScanEndState scanEndState(std::string_view text);

/// Result of autoCloseOnType().
struct AutoCloseResult {
  std::string text;  ///< full new editor text (before + inserted `*/` + after)
  size_t cursor;     ///< new cursor byte offset into text (inside the block)
  bool applied;      ///< false: the `/*` was not a real opener; text unchanged
};

/// Called by the editor right after the user typed `*` completing a `/*`
/// (so `before` ends with the two characters `/*` and the cursor sits
/// between `before` and `after`). When that trailing `/*` is a REAL
/// block-comment opener under the lexical rules, the closing `*/` is
/// inserted and the cursor is placed between the delimiters, inside the
/// new block: text = before + "*/" + after, cursor = before.size().
/// When the `/*` is not real (inside a string, escaped, inside a line
/// comment, or inside an already-open block comment) the text is
/// returned unchanged with applied=false. Inputs larger than 16 MiB are
/// refused with applied=false and empty text (fail-closed; the editor
/// must not apply the edit when applied=false). Never throws.
AutoCloseResult autoCloseOnType(std::string_view before, std::string_view after);

/// Result of trySkipOverCloser().
struct SkipOverResult {
  bool skipped;      ///< true: advanced past the existing `*/`; nothing inserted
  std::string text;  ///< full editor text after the keystroke
  size_t cursor;     ///< new cursor byte offset into text
};

/// VS Code-style skip-over for block comments: when the user types `/`
/// completing a `*/` and the cursor sits immediately before a closer
/// that THIS unit auto-inserted, the cursor advances past that closer
/// instead of inserting a duplicate `*/`.
///
/// Call convention (mirrors autoCloseOnType): the editor calls this right
/// after inserting the typed `/`, so `before` ends with the two
/// characters `*/` just typed and the cursor sits between `before` and
/// `after`. `autoInsertedClosers` holds the absolute start offsets of
/// every `*/` auto-inserted by autoCloseOnType and not yet consumed.
///
/// Skip-over fires only when ALL hold:
///   1. `after` begins with `*/` — there is a closer to skip over;
///   2. `before.size()` is in `autoInsertedClosers` — that closer is one
///      we auto-inserted (the editor records AutoCloseResult::cursor and
///      shifts the offsets on later edits);
///   3. scanEndState(before minus the typed `*/`) is InBlock — the cursor
///      is inside an open block comment, never inside a string where
///      `*/` is literal text.
///
/// DELIBERATE DIVERGENCE FROM VS CODE: VS Code skips over any matching
/// closer. We skip ONLY auto-inserted closers: a `*/` the user typed or
/// pasted deliberately is never hijacked — typing `*/` immediately before
/// it inserts the characters literally, exactly as requested. Skipping
/// never deletes text either way.
///
/// Caller duties: on autoCloseOnType applied=true, insert result.cursor
/// into the set; after a successful skip the position is consumed (drop
/// it — though a stale entry is harmless, the `after` text check is the
/// ground truth); shift positions on any other edit. Inputs larger than
/// 16 MiB are refused with skipped=false (fail-closed). Never throws.
SkipOverResult trySkipOverCloser(std::string_view before, std::string_view after,
                                 const std::unordered_set<size_t>& autoInsertedClosers);

/// Balances block-comment delimiters in pasted text. `pasted` is the raw
/// clipboard text; `contextBefore` is the editor text ahead of the paste
/// cursor, so a paste landing inside an open block comment, string, or
/// line comment is handled — delimiters in those positions are inert and
/// never trigger pairing (no double-pairing).
///
/// Pairing rule (JD's spec): delimiters travel in pairs, even through the
/// clipboard. Each lone real `/*` (no matching `*/`) gets `*/` appended;
/// each lone `*/` (no matching `/*`, counting the tokenizer-inert
/// `*/`-outside-a-block as lone) gets `/*` prepended. Balanced input is
/// returned byte-identical. The returned text is balanced as a standalone
/// unit; the insertion context applies on top exactly like any paste.
/// Inputs larger than 16 MiB are returned byte-identical, unpaired
/// (fail-closed). Never throws.
std::string balancePastedDelimiters(std::string_view pasted, std::string_view contextBefore);

}  // namespace litholattice::console
