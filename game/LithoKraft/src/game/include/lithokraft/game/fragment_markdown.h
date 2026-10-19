// Fragment markdown renderer: Workstream 2 of the LithoKraft
// Fragment-markdown feature (JD 2026-10-11). The Fragment is the player's
// starting book (journal, unlock log, recipes, chip archive) and holds
// LithoScript source text. Design (locked): the FULL markdown spec is
// available inside comments — `// # Heading`, `// - [ ] todo`,
// `/* multi-line markdown */` — so the script parser never breaks on
// markdown. This module extracts comment bodies (via
// ConsoleRegistry::extractComments, Workstream 1) and renders them as a
// tight, correct markdown SUBSET into a structured model (NOT HTML strings)
// that the future Fragment UI will bind to. No Fragment UI exists yet; this
// is the model that UI will bind to. LF line endings.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace lithokraft {

/// Fragment view mode: the raw LithoScript source, or the rendered book.
enum class FragmentView { Source, Rendered };

/// Inline span kinds for rich text inside blocks. Nesting is allowed:
/// e.g. bold may contain italic spans.
enum class InlineKind { Text, Bold, Italic, Code, Link };

/// One run of inline content. For Link, `text` is the label and `url` the
/// target; for all other kinds `url` is empty. Bold/Italic/Link spans nest:
/// `text` holds the raw inner source and `children` its parsed inline tree
/// (e.g. `**bold *and italic* **` -> a Bold span whose children contain an
/// Italic span). Text and Code spans never have children.
struct InlineSpan {
  InlineKind kind = InlineKind::Text;
  std::string text;
  std::string url;
  std::vector<InlineSpan> children;
};

/// Block kinds in the rendered document. Lists and checklists are grouped:
/// consecutive list items of the same kind form ONE block.
enum class BlockKind {
  Heading,      ///< ATX heading `#`..`######`; `level` = 1..6, `spans` = title
  Paragraph,    ///< plain text lines; `spans` holds the inline runs
  BulletList,   ///< `-`/`*` items; `items` in order
  NumberedList, ///< `1.` items; `items` in order, `start` = first number
  Checklist,    ///< `- [ ]`/`- [x]` items; `items` in order, each with `checked`
  CodeBlock,    ///< fenced block; `language` = info string, `code` = verbatim body
  Quote,        ///< `>` lines; `spans` holds the joined quoted text
  Rule,         ///< `---` / `***` / `___` horizontal rule; no payload
};

/// One item in a BulletList, NumberedList, or Checklist block.
struct ListItem {
  std::vector<InlineSpan> spans;  ///< item text as inline runs
  bool checked = false;           ///< Checklist only: true for `- [x]`
};

/// One block of the rendered document.
struct Block {
  BlockKind kind = BlockKind::Paragraph;
  int level = 0;                   ///< Heading: 1..6; otherwise 0
  int start = 1;                   ///< NumberedList: number of the first item
  std::string language;            ///< CodeBlock: fence info string (may be empty)
  std::string code;                ///< CodeBlock: verbatim body, lines joined with '\n'
  std::vector<InlineSpan> spans;   ///< Heading / Paragraph / Quote text
  std::vector<ListItem> items;     ///< list kinds
};

/// The structured render of the Fragment's markdown.
struct RenderedDoc {
  std::vector<Block> blocks;  ///< in source order; may be empty
};

/// Result of rendering the Fragment. Follows the codebase's bool+err-string
/// convention: no exceptions for control flow.
struct FragmentRenderResult {
  bool ok = true;                     ///< false only when `err` is non-empty
  std::string err;                    ///< extraction error, e.g. "unterminated block comment"
  RenderedDoc doc;                    ///< valid when view == FragmentView::Rendered
  std::vector<std::string> lines;     ///< valid when view == FragmentView::Source
};

/// Extracts the markdown source from the Fragment's full text: comment
/// bodies in source order (see ConsoleRegistry::extractComments), joined
/// with '\n'. On an unterminated `/*`, `err` (when non-null) is set to
/// "unterminated block comment" and the bodies extracted so far are still
/// returned. `err` is set only on error, never cleared.
std::string extractMarkdownSource(const std::string& text, std::string* err);

/// Entry point: renders the Fragment's text for one of the two views.
/// - FragmentView::Source: `lines` = the raw text split on '\n' (comment
///   markers included, unprocessed). `doc` stays empty.
/// - FragmentView::Rendered with markdownEnabled: the extracted comment
///   bodies are parsed as the markdown subset below into `doc`. Non-comment
///   script lines never appear in the markdown — they live in the source
///   view only.
/// - FragmentView::Rendered with markdownEnabled == false (the default
///   pre-Spark Fragment): comment bodies are NOT parsed as markdown. Each
///   line of the markdown source becomes a plain Paragraph block whose only
///   span is InlineKind::Text — no elements, no inline formatting.
/// Rendering proceeds on whatever extractComments returned even when it
/// reported an error; in that case `ok` is false and `err` carries the
/// message, but `doc`/`lines` are still populated from the partial input.
FragmentRenderResult renderFragment(const std::string& text, bool markdownEnabled,
                                    FragmentView view);

/// Checklist interactivity seam. Toggles the `checked` flag of the
/// checklist item at flattened index `index` (checklist items in document
/// order, across all Checklist blocks). Returns false when the index is out
/// of range. NOTE: checked state is NOT persisted — persistence is a future
/// concern; the save system will need to re-serialize this doc (or a diff)
/// when the Fragment gains real storage.
bool toggleChecklistItem(RenderedDoc& doc, size_t index);

/// Counts checklist items in document order (the index space used by
/// toggleChecklistItem).
size_t checklistItemCount(const RenderedDoc& doc);

// -- Markdown subset ---------------------------------------------------------
// Block elements: ATX headings (#..######, 1-3 leading spaces, optional
// closing #'s), bullet lists (-/*), numbered lists (N.), checklists
// (- [ ] / - [x]), fenced code blocks (```, info string kept; an UNCLOSED
// fence becomes a CodeBlock running to end of input — CommonMark rule),
// horizontal rules (---, ***, ___ — 3+ of one marker, spaces allowed),
// blockquotes (>, single level; consecutive > lines join one Quote block).
// Paragraphs are runs of non-blank lines joined with a space.
// Inline elements: `code`, **bold**, *italic*, _italic_, [text](url) links,
// backslash escapes (\* -> literal *). Emphasis needs word boundaries: a
// lone * inside a word (2*3, a*b) is literal text, never italic.
// Deliberately NOT supported: nested lists (flattened to one level),
// multi-level quotes, setext headings, tables, images, reference links,
// footnotes, strikethrough, HTML. The bar is correctness over exhaustiveness.

}  // namespace lithokraft
