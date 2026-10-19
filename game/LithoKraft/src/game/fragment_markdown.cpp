// Fragment markdown renderer implementation. See fragment_markdown.h for
// the API contract and the supported-subset notes. LF line endings.
#include "lithokraft/game/fragment_markdown.h"

#include <cctype>

#include "litholattice/console/concmd.h"

namespace lithokraft {
namespace {

bool isSpace(char c) { return c == ' ' || c == '\t'; }
bool isWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

// Splits on '\n'; keeps '\r' as content (Source view returns raw lines).
std::vector<std::string> splitLines(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == '\n') {
      out.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && isSpace(s[b])) ++b;
  while (e > b && isSpace(s[e - 1])) --e;
  return s.substr(b, e - b);
}

bool isBlank(const std::string& s) { return trim(s).empty(); }

// Leading-space count, capped at 4 (4+ means "more than 3" for our rules).
size_t indentOf(const std::string& s) {
  size_t n = 0;
  while (n < s.size() && n < 4 && isSpace(s[n])) ++n;
  return n;
}

// -- Inline parsing ----------------------------------------------------------

// Forward declaration for recursion (nested emphasis inside bold/italic/link).
std::vector<InlineSpan> parseInline(const std::string& s);

void pushText(std::vector<InlineSpan>& out, const std::string& t) {
  if (t.empty()) return;
  if (!out.empty() && out.back().kind == InlineKind::Text && out.back().url.empty()) {
    out.back().text += t;
    return;
  }
  InlineSpan sp;
  sp.kind = InlineKind::Text;
  sp.text = t;
  out.push_back(sp);
}

// Finds the closing run for an inline-code span starting at `open` (the
// backtick-run length is `runLen`). Returns npos when unclosed.
size_t findCodeClose(const std::string& s, size_t open, size_t runLen) {
  for (size_t i = open + runLen; i < s.size();) {
    if (s[i] == '`') {
      size_t j = i;
      while (j < s.size() && s[j] == '`') ++j;
      if (j - i == runLen) return i;
      i = j;
    } else {
      ++i;
    }
  }
  return std::string::npos;
}

// Finds the closing `**` for a bold span starting at `open` (after the
// opening `**`). Returns npos when unclosed. A lone `*` inside does not
// close; a closing run of 3+ stars closes the bold with the first two
// stars and leaves the remainder literal — deterministic and simple.
size_t findBoldClose(const std::string& s, size_t open) {
  for (size_t i = open; i + 1 < s.size(); ++i) {
    if (s[i] == '*' && s[i + 1] == '*') return i;
  }
  return std::string::npos;
}

// Finds the closing single-char delimiter `d` ('*' or '_') starting the
// scan at `open`. Returns npos when unclosed.
size_t findEmClose(const std::string& s, size_t open, char d) {
  for (size_t i = open; i < s.size(); ++i) {
    if (s[i] == d) return i;
  }
  return std::string::npos;
}

// Balanced-bracket link parse at position `open` (s[open] == '[').
// Returns the span end (one past ')') and fills text/url; end == npos when
// malformed. No nested '[' inside the label.
bool parseLink(const std::string& s, size_t open, size_t& end, std::string& label,
               std::string& url) {
  size_t rb = std::string::npos;
  for (size_t i = open + 1; i < s.size(); ++i) {
    if (s[i] == ']') {
      rb = i;
      break;
    }
    if (s[i] == '[') return false;
  }
  if (rb == std::string::npos || rb + 1 >= s.size() || s[rb + 1] != '(') return false;
  size_t rp = std::string::npos;
  for (size_t i = rb + 2; i < s.size(); ++i) {
    if (s[i] == ')') {
      rp = i;
      break;
    }
  }
  if (rp == std::string::npos) return false;
  label = s.substr(open + 1, rb - open - 1);
  url = s.substr(rb + 2, rp - rb - 2);
  end = rp + 1;
  return true;
}

std::vector<InlineSpan> parseInline(const std::string& s) {
  std::vector<InlineSpan> out;
  size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    // Backslash escape: \X -> literal X for punctuation.
    if (c == '\\' && i + 1 < s.size() && std::ispunct(static_cast<unsigned char>(s[i + 1]))) {
      pushText(out, s.substr(i + 1, 1));
      i += 2;
      continue;
    }
    // Inline code: `...` (matching run length).
    if (c == '`') {
      size_t j = i;
      while (j < s.size() && s[j] == '`') ++j;
      const size_t run = j - i;
      const size_t close = findCodeClose(s, i, run);
      if (close != std::string::npos) {
        InlineSpan sp;
        sp.kind = InlineKind::Code;
        sp.text = s.substr(i + run, close - i - run);
        out.push_back(sp);
        i = close + run;
        continue;
      }
      pushText(out, s.substr(i, run));  // unclosed: literal
      i = j;
      continue;
    }
    // Link: [text](url).
    if (c == '[') {
      size_t end = std::string::npos;
      std::string label, url;
      if (parseLink(s, i, end, label, url)) {
        InlineSpan sp;
        sp.kind = InlineKind::Link;
        sp.text = label;
        sp.url = url;
        sp.children = parseInline(label);  // nested emphasis/code in the label
        out.push_back(sp);
        i = end;
        continue;
      }
      pushText(out, "[");
      ++i;
      continue;
    }
    // Bold: **text**. Opener must not be followed by whitespace; the
    // closing ** must not be followed by a word char (keeps "a**b**c"
    // from mis-firing on intra-word stars).
    if (c == '*' && i + 1 < s.size() && s[i + 1] == '*') {
      const bool openOk = (i + 2 >= s.size()) || !isSpace(s[i + 2]);
      const size_t close = openOk ? findBoldClose(s, i + 2) : std::string::npos;
      const bool closeOk = close != std::string::npos && (close + 2 >= s.size() || !isWordChar(s[close + 2]));
      if (closeOk) {
        InlineSpan sp;
        sp.kind = InlineKind::Bold;
        const std::string inner = s.substr(i + 2, close - i - 2);
        sp.text = inner;
        sp.children = parseInline(inner);  // nested emphasis/code/links
        out.push_back(sp);
        i = close + 2;
        continue;
      }
      pushText(out, "**");
      i += 2;
      continue;
    }
    // Italic: *text* or _text_. A lone * is literal inside words
    // (2*3, a*b stay text); _ behaves the same.
    if ((c == '*' || c == '_')) {
      const bool beforeOk = (i == 0) || !isWordChar(s[i - 1]);
      const bool afterOk = (i + 1 < s.size()) && !isSpace(s[i + 1]);
      if (beforeOk && afterOk) {
        const size_t close = findEmClose(s, i + 1, c);
        const bool closeBeforeOk = close != std::string::npos && close > i + 1 &&
                                   !isSpace(s[close - 1]) &&
                                   (close + 1 >= s.size() || !isWordChar(s[close + 1]));
        if (closeBeforeOk) {
          InlineSpan sp;
          sp.kind = InlineKind::Italic;
          const std::string inner = s.substr(i + 1, close - i - 1);
          sp.text = inner;
          sp.children = parseInline(inner);  // nested emphasis/code/links
          out.push_back(sp);
          i = close + 1;
          continue;
        }
      }
      pushText(out, s.substr(i, 1));
      ++i;
      continue;
    }
    pushText(out, s.substr(i, 1));
    ++i;
  }
  return out;
}

// -- Block parsing -----------------------------------------------------------

struct LineView {
  std::string text;    // raw line
  std::string stripped;  // trim()
  size_t indent;         // indentOf(text), capped at 4
};

// ATX heading: returns level 0 when not a heading.
int headingLevel(const LineView& l) {
  if (l.indent > 3) return 0;
  size_t i = l.indent;
  size_t hashes = 0;
  while (i < l.text.size() && l.text[i] == '#') {
    ++hashes;
    ++i;
  }
  if (hashes == 0 || hashes > 6) return 0;
  // '#' must be followed by space/tab or end of line.
  if (i < l.text.size() && l.text[i] != ' ' && l.text[i] != '\t') return 0;
  return static_cast<int>(hashes);
}

// Heading text: drop the hashes, strip optional closing #'s.
std::string headingText(const LineView& l, int level) {
  std::string t = l.text.substr(l.indent + static_cast<size_t>(level));
  t = trim(t);
  // Closing sequence: spaces then #'s then spaces at the very end.
  size_t e = t.size();
  while (e > 0 && isSpace(t[e - 1])) --e;
  size_t h = e;
  while (h > 0 && t[h - 1] == '#') --h;
  if (h < e && h > 0 && isSpace(t[h - 1])) {
    t = trim(t.substr(0, h));
  }
  return t;
}

bool isRule(const LineView& l) {
  if (l.stripped.size() < 3) return false;
  char marker = 0;
  int count = 0;
  for (char c : l.stripped) {
    if (isSpace(c)) continue;
    if (c != '-' && c != '*' && c != '_') return false;
    if (marker == 0) marker = c;
    if (c != marker) return false;
    ++count;
  }
  return count >= 3;
}

bool isFence(const LineView& l) {
  if (l.indent > 3) return false;
  size_t i = l.indent;
  size_t ticks = 0;
  while (i < l.text.size() && l.text[i] == '`') {
    ++ticks;
    ++i;
  }
  return ticks >= 3;
}

bool isQuote(const LineView& l) {
  if (l.indent > 3) return false;
  return l.text.size() > l.indent && l.text[l.indent] == '>';
}

std::string quoteText(const LineView& l) {
  size_t i = l.indent + 1;  // past '>'
  if (i < l.text.size() && l.text[i] == ' ') ++i;
  return l.text.substr(i);
}

// Bullet item: returns true and fills `content` when the line is a
// `-`/`*` item. Checklist markers are detected separately.
bool bulletItem(const LineView& l, std::string& content) {
  if (l.indent > 3 || isBlank(l.stripped)) return false;
  size_t i = l.indent;
  if (l.text[i] != '-' && l.text[i] != '*') return false;
  // Rule lines are rules, not list items: "---" vs "- " — a lone marker
  // followed by space/end is a list item; 3+ markers is a rule.
  size_t m = i;
  while (m < l.text.size() && l.text[m] == l.text[i]) ++m;
  if (m - i >= 3 && isRule(l)) return false;
  if (m - i > 1) return false;
  if (m >= l.text.size()) {
    content.clear();
    return true;
  }
  if (l.text[m] != ' ' && l.text[m] != '\t') return false;
  content = trim(l.text.substr(m + 1));
  return true;
}

// Numbered item: returns true and fills `number`/`content`.
bool numberedItem(const LineView& l, int& number, std::string& content) {
  if (l.indent > 3 || isBlank(l.stripped)) return false;
  size_t i = l.indent;
  size_t d = i;
  while (d < l.text.size() && std::isdigit(static_cast<unsigned char>(l.text[d]))) ++d;
  if (d == i || d >= l.text.size() || l.text[d] != '.') return false;
  const size_t after = d + 1;
  if (after < l.text.size() && l.text[after] != ' ' && l.text[after] != '\t') return false;
  number = 0;
  for (size_t k = i; k < d; ++k) number = number * 10 + (l.text[k] - '0');
  content = after < l.text.size() ? trim(l.text.substr(after + 1)) : std::string();
  return true;
}

// Checklist item: bullet content of the form "[ ] rest" / "[x] rest".
// Fills `checked` and `content`; returns false when not a checklist item.
bool checklistItem(const std::string& bulletContent, bool& checked, std::string& content) {
  if (bulletContent.size() < 4 || bulletContent[0] != '[') return false;
  const char mark = bulletContent[1];
  if (bulletContent[2] != ']' || (bulletContent[3] != ' ' && bulletContent[3] != '\t')) return false;
  if (mark == ' ') {
    checked = false;
  } else if (mark == 'x' || mark == 'X') {
    checked = true;
  } else {
    return false;
  }
  content = trim(bulletContent.substr(4));
  return true;
}

// Plain-text render used when markdown is DISABLED: every line becomes a
// Paragraph with one Text span — no elements, no inline formatting.
RenderedDoc renderPlain(const std::string& mdSource) {
  RenderedDoc doc;
  for (const std::string& line : splitLines(mdSource)) {
    if (isBlank(line)) continue;
    Block b;
    b.kind = BlockKind::Paragraph;
    InlineSpan sp;
    sp.kind = InlineKind::Text;
    sp.text = line;
    b.spans.push_back(sp);
    doc.blocks.push_back(b);
  }
  return doc;
}

RenderedDoc renderMarkdown(const std::string& mdSource) {
  RenderedDoc doc;
  const std::vector<std::string> raw = splitLines(mdSource);
  std::vector<LineView> lines;
  lines.reserve(raw.size());
  for (const std::string& t : raw) {
    LineView l;
    l.text = t;
    l.stripped = trim(t);
    l.indent = indentOf(t);
    lines.push_back(l);
  }

  std::vector<std::string> para;  // pending paragraph lines
  auto flushPara = [&]() {
    if (para.empty()) return;
    std::string joined;
    for (size_t k = 0; k < para.size(); ++k) {
      if (k) joined += ' ';
      joined += trim(para[k]);
    }
    Block b;
    b.kind = BlockKind::Paragraph;
    b.spans = parseInline(joined);
    doc.blocks.push_back(b);
    para.clear();
  };

  size_t i = 0;
  while (i < lines.size()) {
    const LineView& l = lines[i];
    if (isBlank(l.text)) {
      flushPara();
      ++i;
      continue;
    }
    // Fenced code block.
    if (isFence(l)) {
      flushPara();
      std::string info = trim(l.text.substr(l.indent));
      size_t t = 0;
      while (t < info.size() && info[t] == '`') ++t;
      info = trim(info.substr(t));
      std::string body;
      bool first = true;
      ++i;
      while (i < lines.size() && !isFence(lines[i])) {
        if (!first) body += '\n';
        body += lines[i].text;
        first = false;
        ++i;
      }
      if (i < lines.size()) ++i;  // consume the closing fence
      // UNCLOSED fence: per the documented CommonMark-style rule, the
      // block runs to end of input — still a CodeBlock.
      Block b;
      b.kind = BlockKind::CodeBlock;
      b.language = info;
      b.code = body;
      doc.blocks.push_back(b);
      continue;
    }
    // ATX heading.
    const int hl = headingLevel(l);
    if (hl > 0) {
      flushPara();
      Block b;
      b.kind = BlockKind::Heading;
      b.level = hl;
      b.spans = parseInline(headingText(l, hl));
      doc.blocks.push_back(b);
      ++i;
      continue;
    }
    // Horizontal rule.
    if (isRule(l)) {
      flushPara();
      Block b;
      b.kind = BlockKind::Rule;
      doc.blocks.push_back(b);
      ++i;
      continue;
    }
    // Blockquote: consecutive '>' lines join one Quote.
    if (isQuote(l)) {
      flushPara();
      std::string q;
      bool first = true;
      while (i < lines.size() && isQuote(lines[i])) {
        if (!first) q += ' ';
        q += trim(quoteText(lines[i]));
        first = false;
        ++i;
      }
      Block b;
      b.kind = BlockKind::Quote;
      b.spans = parseInline(q);
      doc.blocks.push_back(b);
      continue;
    }
    // List runs: checklist, bullet, numbered — consecutive same-kind items
    // group into one block.
    {
      std::string content;
      bool isCheck = false, checked = false;
      std::string checkContent;
      int number = 0;
      std::string numContent;
      if (bulletItem(l, content) && checklistItem(content, checked, checkContent)) {
        isCheck = true;
      }
      const bool isBullet = bulletItem(l, content);
      const bool isNum = !isBullet && numberedItem(l, number, numContent);
      if (isCheck || isBullet || isNum) {
        flushPara();
        Block b;
        if (isCheck) {
          b.kind = BlockKind::Checklist;
          size_t j = i;
          while (j < lines.size()) {
            std::string c2;
            bool ck = false;
            std::string cc;
            if (!bulletItem(lines[j], c2) || !checklistItem(c2, ck, cc)) break;
            ListItem item;
            item.spans = parseInline(cc);
            item.checked = ck;
            b.items.push_back(item);
            ++j;
          }
          i = j;
        } else if (isBullet) {
          b.kind = BlockKind::BulletList;
          size_t j = i;
          while (j < lines.size()) {
            std::string c2;
            bool ck = false;
            std::string cc;
            if (!bulletItem(lines[j], c2)) break;
            if (checklistItem(c2, ck, cc)) break;  // checklist starts its own block
            ListItem item;
            item.spans = parseInline(c2);
            b.items.push_back(item);
            ++j;
          }
          i = j;
        } else {
          b.kind = BlockKind::NumberedList;
          b.start = number;
          size_t j = i;
          while (j < lines.size()) {
            int n2 = 0;
            std::string c2;
            if (!numberedItem(lines[j], n2, c2)) break;
            ListItem item;
            item.spans = parseInline(c2);
            b.items.push_back(item);
            ++j;
          }
          i = j;
        }
        doc.blocks.push_back(b);
        continue;
      }
    }
    // Otherwise: paragraph line.
    para.push_back(l.text);
    ++i;
  }
  flushPara();
  return doc;
}

}  // namespace

std::string extractMarkdownSource(const std::string& text, std::string* err) {
  std::string local;
  std::string* errPtr = err ? err : &local;
  const std::vector<std::string> bodies =
      litholattice::console::ConsoleRegistry::extractComments(text, errPtr);
  std::string out;
  for (size_t k = 0; k < bodies.size(); ++k) {
    if (k) out += '\n';
    out += bodies[k];
  }
  return out;
}

FragmentRenderResult renderFragment(const std::string& text, bool markdownEnabled,
                                    FragmentView view) {
  FragmentRenderResult r;
  std::string err;
  const std::string mdSource = extractMarkdownSource(text, &err);
  if (!err.empty()) {
    r.ok = false;
    r.err = err;
  }
  if (view == FragmentView::Source) {
    r.lines = splitLines(text);
  } else if (markdownEnabled) {
    r.doc = renderMarkdown(mdSource);
  } else {
    r.doc = renderPlain(mdSource);
  }
  return r;
}

bool toggleChecklistItem(RenderedDoc& doc, size_t index) {
  size_t seen = 0;
  for (Block& b : doc.blocks) {
    if (b.kind != BlockKind::Checklist) continue;
    for (ListItem& item : b.items) {
      if (seen == index) {
        item.checked = !item.checked;
        return true;
      }
      ++seen;
    }
  }
  return false;
}

size_t checklistItemCount(const RenderedDoc& doc) {
  size_t n = 0;
  for (const Block& b : doc.blocks) {
    if (b.kind == BlockKind::Checklist) n += b.items.size();
  }
  return n;
}

}  // namespace lithokraft
