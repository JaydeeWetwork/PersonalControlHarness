// Fragment markdown renderer self-test: extraction via extractComments,
// every supported block/inline element, documented edge cases (unclosed
// fence, * inside words, empty input, unterminated /*), the Source/Rendered
// view toggle, markdownEnabled=false, and the checklist toggle seam.
// Headless: no window, no GL context. LF line endings.
#include "lithokraft/game/fragment_markdown_selftest.h"

#include <cstdio>
#include <string>
#include <vector>

#include "lithokraft/game/fragment_markdown.h"

namespace lithokraft {
namespace {

int fails = 0;
void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++fails;
}

const Block* findBlock(const RenderedDoc& doc, BlockKind kind, size_t skip = 0) {
  for (const Block& b : doc.blocks) {
    if (b.kind == kind) {
      if (skip == 0) return &b;
      --skip;
    }
  }
  return nullptr;
}

std::string spanText(const std::vector<InlineSpan>& spans) {
  std::string out;
  for (const InlineSpan& s : spans) out += s.text;
  return out;
}

// -- Extraction -------------------------------------------------------------
void testExtraction() {
  std::string err;
  const std::string src = extractMarkdownSource(
      "int x = 1; // the note\n"
      "// # Title\n"
      "/* multi\n"
      "line */\n"
      "echo \"/* quoted */\" // after\n",
      &err);
  // Bodies in source order, markers stripped, joined with '\n'; the
  // quoted /* */ yields nothing.
  check(err.empty() && src == " the note\n # Title\n multi\nline \n after",
        "extract: mixed // and /* */ bodies in order, markers stripped, quoted markers ignored");
  // No comments: empty source, no error.
  err.clear();
  const std::string empty = extractMarkdownSource("int x = 1;\n", &err);
  check(err.empty() && empty.empty(), "extract: no comments -> empty markdown source, no error");
  // Unterminated /*: error surfaced, bodies so far kept.
  err.clear();
  const std::string partial = extractMarkdownSource("// kept\n/* dangling", &err);
  check(err == "unterminated block comment" && partial == " kept",
        "extract: unterminated /* -> err set to \"unterminated block comment\", prior bodies kept");
}

// -- Block elements ----------------------------------------------------------
void testBlocks() {
  const char* text =
      "// # Title One\n"
      "// ### Deep ###\n"
      "//\n"
      "// - apple\n"
      "// - banana\n"
      "//\n"
      "// 3. third\n"
      "// 4. fourth\n"
      "//\n"
      "// - [ ] open\n"
      "// - [x] done\n"
      "//\n"
      "// ```lithoscript\n"
      "// etch(\"wafer\");\n"
      "// ```\n"
      "//\n"
      "// ---\n"
      "//\n"
      "// > quoted words\n"
      "// > still quoted\n";
  FragmentRenderResult r = renderFragment(text, true, FragmentView::Rendered);
  bool ok = r.ok && r.err.empty() && r.doc.blocks.size() == 8;
  const Block* h1 = findBlock(r.doc, BlockKind::Heading);
  ok = ok && h1 && h1->level == 1 && spanText(h1->spans) == "Title One";
  const Block* h3 = findBlock(r.doc, BlockKind::Heading, 1);
  ok = ok && h3 && h3->level == 3 && spanText(h3->spans) == "Deep";
  check(ok, "blocks: two ATX headings (levels 1 and 3, closing #'s stripped)");

  const Block* bl = findBlock(r.doc, BlockKind::BulletList);
  ok = bl && bl->items.size() == 2 && spanText(bl->items[0].spans) == "apple" &&
       spanText(bl->items[1].spans) == "banana";
  check(ok, "blocks: bullet list of two items");

  const Block* nl = findBlock(r.doc, BlockKind::NumberedList);
  ok = nl && nl->start == 3 && nl->items.size() == 2 &&
       spanText(nl->items[0].spans) == "third" && spanText(nl->items[1].spans) == "fourth";
  check(ok, "blocks: numbered list keeps the first number as start (3)");

  const Block* cl = findBlock(r.doc, BlockKind::Checklist);
  ok = cl && cl->items.size() == 2 && !cl->items[0].checked && cl->items[1].checked &&
       spanText(cl->items[0].spans) == "open" && spanText(cl->items[1].spans) == "done";
  check(ok, "blocks: checklist carries checked state per item ([ ]/[x])");

  const Block* cb = findBlock(r.doc, BlockKind::CodeBlock);
  ok = cb && cb->language == "lithoscript" && cb->code == " etch(\"wafer\");";
  check(ok, "blocks: fenced code block keeps info string and verbatim body");

  const Block* rule = findBlock(r.doc, BlockKind::Rule);
  check(rule != nullptr, "blocks: --- is a horizontal rule");

  const Block* q = findBlock(r.doc, BlockKind::Quote);
  ok = q && spanText(q->spans) == "quoted words still quoted";
  check(ok, "blocks: consecutive > lines join into one Quote");
}

// -- Inline elements ----------------------------------------------------------
void testInline() {
  const char* text =
      "// **bold** and *italic* and _also italic_ and `code()` here\n"
      "//\n"
      "// [label](https://example.test/x)\n";
  FragmentRenderResult r = renderFragment(text, true, FragmentView::Rendered);
  bool ok = r.ok && r.doc.blocks.size() == 2 && r.doc.blocks[0].kind == BlockKind::Paragraph;
  const std::vector<InlineSpan>& sp = r.doc.blocks[0].spans;
  // [Bold, Text, Italic, Text, Italic, Text, Code, Text] (paragraph join trims line edges)
  ok = ok && sp.size() == 8;
  ok = ok && sp[0].kind == InlineKind::Bold && sp[0].text == "bold";
  ok = ok && sp[2].kind == InlineKind::Italic && sp[2].text == "italic";
  ok = ok && sp[4].kind == InlineKind::Italic && sp[4].text == "also italic";
  ok = ok && sp[6].kind == InlineKind::Code && sp[6].text == "code()";
  check(ok, "inline: **bold**, *italic*, _italic_, `code` parse to typed spans");

  // The link line is its own paragraph (blank // line separates).
  FragmentRenderResult r2 = renderFragment("// [label](https://example.test/x)\n", true,
                                           FragmentView::Rendered);
  bool ok2 = r2.ok && r2.doc.blocks.size() == 1;
  const std::vector<InlineSpan>& ls = r2.doc.blocks[0].spans;
  ok2 = ok2 && ls.size() == 1 && ls[0].kind == InlineKind::Link &&
        ls[0].text == "label" && ls[0].url == "https://example.test/x";
  check(ok2, "inline: [text](url) parses to a Link span with label and url");

  // Nested emphasis: bold containing italic.
  FragmentRenderResult r3 = renderFragment("// **bold *and italic* end**\n", true,
                                           FragmentView::Rendered);
  bool ok3 = r3.ok && r3.doc.blocks.size() == 1;
  const std::vector<InlineSpan>& ns = r3.doc.blocks[0].spans;
  ok3 = ok3 && ns.size() == 1 && ns[0].kind == InlineKind::Bold &&
        ns[0].text == "bold *and italic* end";
  bool nested = false;
  for (const InlineSpan& c : ns[0].children) {
    if (c.kind == InlineKind::Italic && c.text == "and italic") nested = true;
  }
  check(ok3 && nested, "inline: **bold *and italic* end** nests an Italic span inside Bold");

  // Backslash escape.
  FragmentRenderResult r4 = renderFragment("// \\*not italic\\*\n", true, FragmentView::Rendered);
  bool ok4 = r4.ok && r4.doc.blocks.size() == 1;
  const std::vector<InlineSpan>& es = r4.doc.blocks[0].spans;
  ok4 = ok4 && es.size() == 1 && es[0].kind == InlineKind::Text && es[0].text == "*not italic*";
  check(ok4, "inline: backslash escapes turn \\* into literal text");
}

// -- Edge cases ---------------------------------------------------------------
void testEdgeCases() {
  // A lone * inside words is literal text, never italic.
  FragmentRenderResult r = renderFragment("// 2*3=6 and a*b stay\n", true, FragmentView::Rendered);
  bool ok = r.ok && r.doc.blocks.size() == 1;
  const std::vector<InlineSpan>& sp = r.doc.blocks[0].spans;
  ok = ok && sp.size() == 1 && sp[0].kind == InlineKind::Text &&
       sp[0].text == "2*3=6 and a*b stay";
  check(ok, "edge: * inside words (2*3, a*b) stays literal text");

  // Unclosed fence: documented rule — becomes a CodeBlock to end of input.
  FragmentRenderResult r2 = renderFragment("// ```\n// tail line\n", true, FragmentView::Rendered);
  bool ok2 = r2.ok && r2.doc.blocks.size() == 1 &&
             r2.doc.blocks[0].kind == BlockKind::CodeBlock &&
             r2.doc.blocks[0].code == " tail line";
  check(ok2, "edge: unclosed fence -> CodeBlock running to end of input (documented rule)");

  // Empty input: empty doc, still ok.
  FragmentRenderResult r3 = renderFragment("", true, FragmentView::Rendered);
  check(r3.ok && r3.err.empty() && r3.doc.blocks.empty(),
        "edge: empty input -> ok, empty doc");

  // Comment-only whitespace: no blocks.
  FragmentRenderResult r4 = renderFragment("//\n//   \n", true, FragmentView::Rendered);
  check(r4.ok && r4.doc.blocks.empty(), "edge: blank comment lines -> no blocks");

  // Non-comment script lines never become markdown.
  FragmentRenderResult r5 = renderFragment("etch(\"x\"); // # real\n", true, FragmentView::Rendered);
  bool ok5 = r5.ok && r5.doc.blocks.size() == 1 &&
             r5.doc.blocks[0].kind == BlockKind::Heading &&
             spanText(r5.doc.blocks[0].spans) == "real";
  check(ok5, "edge: non-comment script lines are excluded from the markdown source");

  // Unterminated /*: rendering proceeds on comments so far; error surfaced.
  FragmentRenderResult r6 = renderFragment("// # kept\n/* dangling", true, FragmentView::Rendered);
  bool ok6 = !r6.ok && r6.err == "unterminated block comment" &&
             r6.doc.blocks.size() == 1 && r6.doc.blocks[0].kind == BlockKind::Heading;
  check(ok6, "edge: unterminated /* -> err surfaced, render proceeds on comments extracted so far");
}

// -- View modes ---------------------------------------------------------------
void testViewModes() {
  const char* text = "etch(\"wafer\");\n// # Title\n// - [ ] todo\n";
  // Source view: raw lines, comment markers included, unprocessed.
  FragmentRenderResult src = renderFragment(text, true, FragmentView::Source);
  bool ok = src.ok && src.doc.blocks.empty() && src.lines.size() == 4 &&
            src.lines[0] == "etch(\"wafer\");" && src.lines[1] == "// # Title" &&
            src.lines[2] == "// - [ ] todo" && src.lines[3].empty();
  check(ok, "view: Source returns raw lines incl. comment markers, unprocessed");

  // Rendered view: structured doc.
  FragmentRenderResult ren = renderFragment(text, true, FragmentView::Rendered);
  bool ok2 = ren.ok && ren.lines.empty() && ren.doc.blocks.size() == 2 &&
             ren.doc.blocks[0].kind == BlockKind::Heading &&
             ren.doc.blocks[1].kind == BlockKind::Checklist;
  check(ok2, "view: Rendered returns the structured RenderedDoc");
}

// -- markdownEnabled = false ---------------------------------------------------
void testDisabled() {
  const char* text = "// # Title\n// **bold** and `code`\n// - [ ] todo\n";
  FragmentRenderResult r = renderFragment(text, false, FragmentView::Rendered);
  bool ok = r.ok && r.err.empty();
  bool noElements = true;
  for (const Block& b : r.doc.blocks) {
    if (b.kind != BlockKind::Paragraph) noElements = false;
    if (b.spans.size() != 1 || b.spans[0].kind != InlineKind::Text) noElements = false;
  }
  ok = ok && noElements && r.doc.blocks.size() == 3 &&
       r.doc.blocks[0].spans[0].text == " # Title" &&
       r.doc.blocks[1].spans[0].text == " **bold** and `code`" &&
       r.doc.blocks[2].spans[0].text == " - [ ] todo";
  check(ok, "disabled: markdownEnabled=false -> comment bodies as plain Text paragraphs, no elements");
}

// -- Checklist toggle seam ------------------------------------------------------
void testToggle() {
  FragmentRenderResult r = renderFragment("// - [ ] a\n// - [x] b\n//\n// - [ ] c\n", true,
                                           FragmentView::Rendered);
  bool ok = r.ok && checklistItemCount(r.doc) == 3;
  check(ok, "toggle: checklistItemCount counts items across Checklist blocks in doc order");
  // Index 1 is the second item ("b", checked) across the two blocks.
  bool t1 = toggleChecklistItem(r.doc, 1);
  const Block* cl1 = findBlock(r.doc, BlockKind::Checklist);
  const Block* cl2 = findBlock(r.doc, BlockKind::Checklist, 1);
  ok = t1 && cl1 && cl1->items[1].checked == false && cl2 && cl2->items[0].checked == false;
  check(ok, "toggle: toggleChecklistItem(1) flips checked -> unchecked");
  bool t2 = toggleChecklistItem(r.doc, 1);
  ok = t2 && cl1->items[1].checked == true;
  check(ok, "toggle: toggling again flips back");
  check(!toggleChecklistItem(r.doc, 99), "toggle: out-of-range index returns false");
}

}  // namespace

int runFragmentMarkdownSelfTest() {
  fails = 0;
  std::printf("fragment markdown selftest:\n");
  testExtraction();
  testBlocks();
  testInline();
  testEdgeCases();
  testViewModes();
  testDisabled();
  testToggle();
  std::printf("%s\n", fails ? "FRAGMENT MARKDOWN SELFTEST: FAILED" : "FRAGMENT MARKDOWN SELFTEST: OK");
  return fails;
}

}  // namespace lithokraft
