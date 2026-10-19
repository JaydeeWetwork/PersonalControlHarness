#include "litholattice/ui/ui_selftest.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include "litholattice/ui/containers.h"
#include "litholattice/ui/controls.h"
#include "litholattice/ui/dialog.h"
#include "litholattice/ui/lists.h"
#include "litholattice/ui/menus.h"
#include "litholattice/ui/text_edit.h"
#include "litholattice/ui/ui_root.h"
#include "litholattice/ui/widget_factory.h"
#include "litholattice/ui/mod_ui.h"
#include <type_traits>

namespace litholattice::ui {
namespace {

class Checker {
 public:
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] ui: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }

 private:
  int fails_ = 0;
};

/// Records draw calls; checks the clip stack stays balanced.
class RecordingRenderer final : public IUiRenderer {
 public:
  void fillRect(const Rect&, Color) override { ++rects; }
  void drawText(Vec2, std::string_view t, Color, float) override { texts.emplace_back(t); }
  void drawImage(const Rect&, std::string_view, Color) override { ++images; }
  void pushClip(const Rect&) override { ++depth; maxDepth = std::max(maxDepth, depth); }
  void popClip() override { --depth; }
  bool drewText(std::string_view t) const {
    for (const auto& s : texts)
      if (s == t) return true;
    return false;
  }
  int rects = 0, images = 0, depth = 0, maxDepth = 0;
  std::vector<std::string> texts;
};

/// Records script handler calls.
class RecordingScripts final : public IScriptHost {
 public:
  bool callHandler(std::string_view decl, std::string_view id) override {
    calls.push_back(std::string(decl) + "|" + std::string(id));
    return true;
  }
  std::vector<std::string> calls;
};

MenuItem item(std::string label, std::function<void()> action = {}) {
  MenuItem m;
  m.label = std::move(label);
  m.action = std::move(action);
  return m;
}

TreeNode node(std::string text, std::vector<TreeNode> children = {}) {
  TreeNode n;
  n.text = std::move(text);
  n.children = std::move(children);
  return n;
}

bool near(float a, float b) { return std::abs(a - b) < 0.01f; }
bool same(const Rect& r, float x, float y, float w, float h) { return near(r.x, x) && near(r.y, y) && near(r.w, w) && near(r.h, h); }
std::string str(const Rect& r) {
  char buf[96];
  std::snprintf(buf, sizeof buf, "(%g,%g %gx%g)", double(r.x), double(r.y), double(r.w), double(r.h));
  return buf;
}

void click(UiRoot& ui, Vec2 p, PointerButton b = PointerButton::Left) {
  ui.handleEvent(UiEvent::move(p));
  ui.handleEvent(UiEvent::down(p, b));
  ui.handleEvent(UiEvent::up(p, b));
}
Vec2 center(const Widget& w) { return {w.rect().x + w.rect().w * 0.5f, w.rect().y + w.rect().h * 0.5f}; }
void type(UiRoot& ui, std::string_view s) {
  for (const char c : s) ui.handleEvent(UiEvent::text(char32_t(c)));
}
void key(UiRoot& ui, Key k, uint8_t mods = kModNone) { ui.handleEvent(UiEvent::keyDown(k, mods)); }

void testLayout(Checker& check) {
  UiRoot ui;
  Widget& root = ui.root();
  auto& filled = root.add<Panel>().setLayout(Layout::fill({10, 20, 30, 40}));
  auto& boxed = root.add<Widget>().setLayout(Layout::at({0.5f, 1.f}, {0.5f, 1.f}, {0, -8}, {100, 50}));
  auto& stack = root.add<StackPanel>(Orientation::Vertical, 5.f).setLayout(Layout::at({0, 0}, {0, 0}, {0, 0}, {200, 300}));
  auto& a = stack.add<Widget>().setLayout(Layout::sized({0, 40}));
  Layout lb = Layout::sized({0, 20}, 1.f);
  lb.margin = {2, 3, 4, 5};
  auto& b = stack.add<Widget>().setLayout(lb);
  auto& c = stack.add<Widget>().setLayout(Layout::sized({0, 10}, 3.f));
  auto& row = root.add<StackPanel>(Orientation::Horizontal, 0.f).setLayout(Layout::at({1, 0}, {1, 0}, {}, {90, 20}));
  auto& r0 = row.add<Widget>().setLayout(Layout::sized({30, 0}));
  auto& r1 = row.add<Widget>().setLayout(Layout::sized({0, 0}, 1.f));
  auto& grid = root.add<GridPanel>(3, Vec2{10, 10}, 2.f).setLayout(Layout::at({0, 0}, {0, 0}, {500, 500}));
  for (int i = 0; i < 5; ++i) grid.add<Widget>();
  ui.layout({800, 600});

  check(same(filled.rect(), 10, 20, 760, 540), "anchors: fill with insets " + str(filled.rect()));
  check(same(boxed.rect(), 350, 542, 100, 50), "anchors: pivot + offset (bottom centre) " + str(boxed.rect()));
  // 300 - (40 + 20 + 8 margin + 10) - 2 * 5 spacing = 212 leftover, split 1 : 3.
  check(same(a.rect(), 0, 0, 200, 40) && same(b.rect(), 2, 48, 194, 73) && same(c.rect(), 0, 131, 200, 169),
        "vertical stack: sizes, flex share, margins, spacing " + str(a.rect()) + str(b.rect()) + str(c.rect()));
  check(same(r0.rect(), 710, 0, 30, 20) && same(r1.rect(), 740, 0, 60, 20), "horizontal stack: fixed + flex child, right anchored");
  const Rect g4 = grid.children()[4]->rect();
  check(same(g4, 512, 512, 10, 10) && near(grid.measure(ui.theme()).x, 34), "grid: row-major cells with spacing " + str(g4));
  auto& padded = root.add<Panel>().setLayout(Layout::at({0, 0}, {0, 0}, {}, {100, 100}));
  auto& inner = padded.add<Widget>();
  ui.layout({800, 600});
  check(same(inner.rect(), 8, 8, 84, 84), "panel padding (theme) insets children " + str(inner.rect()));
  Label& lbl = root.add<Label>("ab\nabcd");
  check(near(lbl.measure(ui.theme()).x, 32) && near(lbl.measure(ui.theme()).y, 32), "label measures multi-line text");
}

void testTheme(Checker& check) {
  Theme t;
  std::string err;
  const bool ok = Theme::parse(std::string(R"({"glyph": [8, 16], "scale": 2,
      "styles": {"default": {"text": "#ff8000"}, "button": {"background": [0.1, 0.2, 0.3], "padding": [1, 2, 3, 4]},
                 "fancy": {"accent": "#00ff0080", "border_width": 3}}})"),
                               t, err);
  check(ok && near(t.scale(), 2) && near(t.glyphSize().x, 16), "theme: parse glyph + scale " + err);
  const Style& b = t.style("button");
  check(near(b.background.b, 0.3f) && near(b.background.a, 1) && b.padding == Edges{1, 2, 3, 4}, "theme: colour arrays and padding");
  const Style& f = t.style("fancy");
  check(near(f.text.r, 1) && near(f.text.g, 128.f / 255) && near(f.accent.a, 128.f / 255) && near(f.borderWidth, 3),
        "theme: new class inherits the default, hex colours with alpha");
  check(near(t.style("unknown-class").text.r, 1), "theme: unknown class falls back to default");
  const bool badKey = !Theme::parse(std::string(R"({"styles": {"button": {"colour": [1, 1, 1]}}})"), t, err) &&
                      err.find("colour") != std::string::npos;
  check(badKey, "theme: unknown key rejected (" + err + ")");
  const bool badValue = !Theme::parse(std::string(R"({"styles": {"button": {"text": [1, 1]}}})"), t, err) &&
                        err.find("text") != std::string::npos;
  check(badValue, "theme: bad colour rejected (" + err + ")");
}

void testFocus(Checker& check) {
  UiRoot ui;
  auto& stack = ui.root().add<StackPanel>().setLayout(Layout::at({0, 0}, {0, 0}, {}, {100, 200}));
  auto& b1 = stack.add<Button>("one");
  auto& label = stack.add<Label>("not focusable");
  auto& b2 = stack.add<Button>("two");
  auto& b3 = stack.add<Button>("three");
  ui.layout({800, 600});
  (void)label;
  b1.focus();
  key(ui, Key::Tab);
  const bool t1 = ui.focus() == &b2;
  key(ui, Key::Tab);
  key(ui, Key::Tab);
  const bool wrap = ui.focus() == &b1;
  key(ui, Key::Tab, kModShift);
  check(t1 && wrap && ui.focus() == &b3, "focus: Tab skips non-focusable, wraps; Shift+Tab goes back");
  b3.setEnabled(false);
  key(ui, Key::Tab, kModShift);
  check(ui.focus() == &b2, "focus: disabled widgets are skipped");
  key(ui, Key::Up);
  check(ui.focus() == &b1, "focus: Up moves to the widget above");
}

void testNavigation(Checker& check) {
  UiRoot ui;
  auto& grid = ui.root().add<GridPanel>(3, Vec2{40, 40}, 4.f);
  std::vector<Slot*> slots;
  for (int i = 0; i < 9; ++i) slots.push_back(&grid.add<Slot>());
  ui.layout({800, 600});
  slots[4]->focus();
  key(ui, Key::Right);
  const bool right = ui.focus() == slots[5];
  key(ui, Key::Down);
  const bool down = ui.focus() == slots[8];
  key(ui, Key::Left);
  key(ui, Key::Up);
  check(right && down && ui.focus() == slots[4], "navigation: arrows move between grid cells");
  click(ui, center(*slots[0]));
  check(ui.focus() == slots[0], "focus: click focuses the focusable target");
  slots[0]->setVisible(false);
  check(ui.focus() == nullptr, "focus: hiding the focused widget clears focus");
  key(ui, Key::Escape);
  slots[1]->focus();
  key(ui, Key::Escape);
  check(ui.focus() == nullptr && !ui.wantsKeyboard(), "focus: Escape releases keyboard focus");
}

void testText(Checker& check) {
  TextBuffer t;
  t.setText("hello\nworld");
  check(t.lineCount() == 2 && t.offsetOf({1, 2}) == 8 && t.posAt(8) == TextPos{1, 2} && t.posAt(99) == TextPos{1, 5},
        "text: line/column <-> offset");
  t.setCaret({0, 5});
  t.insert("!");
  t.insert("!");
  check(t.text() == "hello!!\nworld", "text: insert at caret");
  t.undo();
  check(t.text() == "hello\nworld" && t.caret() == TextPos{0, 5}, "text: typing coalesces into one undo step");
  t.redo();
  check(t.text() == "hello!!\nworld", "text: redo");
  t.setCaret({1, 0});
  t.backspace();
  check(t.text() == "hello!!world" && t.caret() == TextPos{0, 7}, "text: backspace joins lines");
  t.setCaret({0, 0});
  t.moveRight(true, true);
  check(t.selectedText() == "hello", "text: Ctrl+Shift+Right selects a word");
  t.insert("A\nB");
  check(t.text() == "A\nB!!world" && t.caret() == TextPos{1, 1}, "text: multi-line insert replaces the selection");
  t.setText("    indented");
  t.moveEnd(false);
  t.newline();
  check(t.text() == "    indented\n    " && t.caret() == TextPos{1, 4}, "text: newline keeps indentation");
  t.insert("x");
  t.moveHome(false);
  const bool firstBlank = t.caret() == TextPos{1, 4};
  t.moveHome(false);
  check(firstBlank && t.caret() == TextPos{1, 0}, "text: Home toggles first non-blank / column 0");
  t.setText("abc\nx\nlonger line");
  t.setCaret({0, 3});
  t.moveDown(false);
  t.moveDown(false);
  check(t.caret() == TextPos{2, 3}, "text: vertical moves keep the preferred column");
  TextBuffer single(false);
  single.setText("a\nb");
  single.insert("c\nd");
  check(single.text() == "cdab", "text: single-line buffer drops newlines (" + single.text() + ")");

  UiRoot ui;
  MemoryClipboard clip;
  ui.setClipboard(&clip);
  auto& ed = ui.root().add<TextEditor>().setLayout(Layout::at({0, 0}, {0, 0}, {}, {400, 160}));
  auto& editor = static_cast<TextEditor&>(ed);
  int changes = 0, saves = 0;
  editor.on("change", [&](Widget&) { ++changes; });
  editor.on("save", [&](Widget&) { ++saves; });
  ui.layout({800, 600});
  editor.focus();
  type(ui, "void f()");
  key(ui, Key::Enter);
  key(ui, Key::Tab);
  type(ui, "x");
  check(editor.text() == "void f()\n    x" && changes == 11, "editor: typing, Enter, Tab inserts spaces");
  key(ui, Key::A, kModCtrl);
  key(ui, Key::C, kModCtrl);
  key(ui, Key::End, kModCtrl);
  key(ui, Key::V, kModCtrl);
  check(clip.text() == "void f()\n    x" && editor.buffer().lineCount() == 3, "editor: select all, copy, paste");
  key(ui, Key::Z, kModCtrl);
  check(editor.text() == "void f()\n    x", "editor: Ctrl+Z undoes the paste");
  key(ui, Key::S, kModCtrl);
  check(saves == 1, "editor: Ctrl+S emits save");
  const Rect cr = editor.caretRect();
  const TextPos hit = editor.positionAt({cr.x, cr.y + 2});
  check(hit == editor.buffer().caret(), "editor: positionAt(caretRect) round-trips");
  editor.buffer().setCaret({0, 0});
  key(ui, Key::Right, kModShift);
  key(ui, Key::Right, kModShift);
  key(ui, Key::X, kModCtrl);
  check(clip.text() == "vo" && editor.text().rfind("id f()", 0) == 0, "editor: Shift+Right selects, Ctrl+X cuts");
  RecordingRenderer r;
  ui.paint(r);
  check(r.drewText("1") && r.drewText("2") && r.depth == 0 && r.maxDepth >= 1, "editor: line numbers drawn, clip balanced");

  auto& box = ui.root().add<TextBox>("").setLayout(Layout::at({0, 0}, {0, 0}, {0, 300}, {120, 24}));
  auto& tb = static_cast<TextBox&>(box);
  int submits = 0;
  tb.on("submit", [&](Widget&) { ++submits; });
  ui.layout({800, 600});
  click(ui, center(tb));
  type(ui, "42");
  key(ui, Key::Enter);
  key(ui, Key::Backspace);
  check(ui.focus() == &tb && tb.text() == "4" && submits == 1, "textbox: click focuses, type, Enter submits, Backspace");
}

void testEvents(Checker& check) {
  UiRoot ui;
  RecordingScripts scripts;
  ui.setScriptHost(&scripts);
  auto& stack = ui.root().add<StackPanel>().setLayout(Layout::at({0, 0}, {0, 0}, {}, {200, 400}));
  auto& btn = stack.add<Button>("Go");
  btn.setId("go");
  int clicks = 0;
  btn.on("click", [&](Widget&) { ++clicks; });
  btn.onScript("click", "void onGo(const string &in)");
  auto& cb = stack.add<Checkbox>("check");
  auto& slider = stack.add<Slider>(0.f, 10.f, 0.f, 1.f);
  auto& num = stack.add<NumberField>(0, 64, 5);
  ui.layout({800, 600});

  click(ui, center(btn));
  ui.handleEvent(UiEvent::down(center(btn)));
  ui.handleEvent(UiEvent::up({700, 500}));  // released outside: no click
  btn.focus();
  key(ui, Key::Enter);
  check(clicks == 2 && scripts.calls.size() == 2 && scripts.calls[0] == "void onGo(const string &in)|go",
        "button: click on release inside + Enter; script handler gets the id");
  click(ui, center(cb));
  check(cb.checked(), "checkbox: click toggles");
  const Rect sr = slider.rect();
  ui.handleEvent(UiEvent::move({sr.x + 1, sr.y + 2}));
  ui.handleEvent(UiEvent::down({sr.x + 1, sr.y + 2}));
  ui.handleEvent(UiEvent::move({sr.right() + 300, sr.y + 300}));  // captured: still drags
  const float dragged = slider.value();
  ui.handleEvent(UiEvent::up({sr.right() + 300, sr.y + 300}));
  check(near(dragged, 10.f) && ui.capture() == nullptr, "slider: pointer capture keeps dragging outside, clamps to max");
  num.focus();
  type(ui, "70");
  key(ui, Key::Enter);
  const bool clamped = num.value() == 64;
  key(ui, Key::Down);
  ui.handleEvent(UiEvent::scroll(center(num), -1.f));
  check(clamped && num.value() == 62, "number field: typing clamps to 0..64, Down and wheel step");

  ListView& list = ui.root().add<ListView>();
  list.setLayout(Layout::at({0, 0}, {0, 0}, {300, 0}, {200, 100}));
  list.setItems({"a", "b", "c"});
  int selects = 0, activates = 0;
  list.on("select", [&](Widget&) { ++selects; });
  list.on("activate", [&](Widget&) { ++activates; });
  ui.layout({800, 600});
  list.focus();
  key(ui, Key::Down);
  key(ui, Key::Down);
  key(ui, Key::Enter);
  check(list.selected() == 1 && selects == 2 && activates == 1, "list: keyboard selection and activate");

  TreeView& tree = ui.root().add<TreeView>();
  tree.setLayout(Layout::at({0, 0}, {0, 0}, {300, 150}, {200, 200}));
  tree.setRoots({node("scripts", {node("a.as"), node("b.as")}), node("other")});
  ui.layout({800, 600});
  tree.focus();
  key(ui, Key::Down);  // selects "scripts"
  key(ui, Key::Right);  // expands
  key(ui, Key::Right);  // into the first child
  check(tree.visibleRowCount() == 4 && tree.selected() && tree.selected()->text == "a.as", "tree: expand and enter children");
  key(ui, Key::Left);
  key(ui, Key::Left);
  check(tree.visibleRowCount() == 2 && tree.selected()->text == "scripts", "tree: Left goes to parent, collapses");

  Dropdown& dd = ui.root().add<Dropdown>(std::vector<std::string>{"red", "green", "blue"});
  dd.setLayout(Layout::at({0, 0}, {0, 0}, {550, 0}, {120, 24}));
  int ddChanges = 0;
  dd.on("change", [&](Widget&) { ++ddChanges; });
  ui.layout({800, 600});
  click(ui, center(dd));
  const bool opened = ui.popupOpen();
  key(ui, Key::Down);
  key(ui, Key::Down);
  key(ui, Key::Down);
  key(ui, Key::Enter);
  check(opened && !ui.popupOpen() && dd.selected() == 2 && ddChanges == 1, "dropdown: popup list, keyboard choose");
  RecordingRenderer r;
  ui.paint(r);
  check(r.depth == 0 && r.drewText("blue"), "paint: clip stack balanced, current values drawn");
}

void testLayers(Checker& check) {
  UiRoot ui;
  RecordingScripts scripts;
  ui.setScriptHost(&scripts);
  ui.layout({800, 600});
  auto& under = ui.root().add<Button>("under").setLayout(Layout::at({0, 0}, {0, 0}, {}, {100, 30}));
  int underClicks = 0;
  under.on("click", [&](Widget&) { ++underClicks; });
  bool saved = false, opened = false;
  MenuBar& bar = ui.root().add<MenuBar>();
  bar.setLayout(Layout::at({0, 0}, {0, 0}, {0, 100}, {400, 24}));
  MenuItem save = item("Save", [&] { saved = true; });
  save.shortcut = "Ctrl+S";
  MenuItem recent = item("Recent");
  recent.submenu = {item("a.as", [&] { opened = true; })};
  MenuItem quit = item("Quit");
  quit.script = "void onQuit(const string &in)";
  bar.addMenu("File", {save, recent, MenuItem::separatorItem(), quit});
  bar.addMenu("Edit", {item("Undo")});
  ui.layout({800, 600});

  click(ui, {10, 110});
  const bool open = bar.openIndex() == 0;
  key(ui, Key::Down);
  key(ui, Key::Enter);
  check(open && saved && !ui.popupOpen(), "menubar: click opens, keyboard runs an item and closes");
  click(ui, {10, 110});
  key(ui, Key::Down);
  key(ui, Key::Down);
  key(ui, Key::Right);  // opens the submenu
  key(ui, Key::Enter);
  check(opened && !ui.popupOpen(), "menu: submenu by keyboard");
  click(ui, {10, 110});
  key(ui, Key::Right);
  const bool switched = bar.openIndex() == 1;
  click(ui, {700, 500});  // outside: closes, not passed on
  check(switched && !ui.popupOpen(), "menubar: Right switches menus, outside click closes");
  Menu& ctx = ContextMenu::open(ui, {300, 300}, {quit});
  ctx.activate(0);
  check(scripts.calls.size() == 1 && scripts.calls[0] == "void onQuit(const string &in)|Quit", "context menu: script item");

  Dialog& dlg = Dialog::show(ui, "Confirm");
  dlg.body().add<Label>("Really?");
  bool ok = false;
  dlg.addButton("OK", [&] { ok = true; });
  Button& cancel = dlg.addButton("Cancel");
  ui.layout({800, 600});
  click(ui, center(under));
  check(underClicks == 0 && ui.modalOpen() && ui.wantsKeyboard(), "modal: blocks input below");
  key(ui, Key::Tab);
  const bool first = ui.focus() && ui.focus()->parent() && ui.focus()->parent()->parent() == &dlg;
  key(ui, Key::Tab);
  key(ui, Key::Tab);
  key(ui, Key::Tab);
  check(first && ui.focus() == &cancel, "modal: Tab cycles inside the dialog only");
  key(ui, Key::Enter);
  check(!ui.modalOpen() && ui.focus() == nullptr, "dialog: button closes it");
  Dialog::show(ui, "Esc");
  key(ui, Key::Escape);
  check(!ui.modalOpen() && !ok, "dialog: Escape closes");
  click(ui, center(under));
  check(underClicks == 1, "modal: input below works again after closing");

  under.setTooltip("hint");
  ui.handleEvent(UiEvent::move(center(under)));
  RecordingRenderer r1, r2;
  ui.paint(r1);
  ui.update(1.f);
  ui.paint(r2);
  check(!r1.drewText("hint") && r2.drewText("hint"), "tooltip: shown after the hover delay");
}

void testFactory(Checker& check) {
  std::string err;
  auto w = buildWidget(std::string(R"json({"type": "panel", "id": "root", "layout": {"size": [300, 200]},
      "children": [
        {"type": "stack", "orientation": "horizontal", "children": [
          {"type": "button", "id": "play", "text": "Play", "on": {"click": "void onPlay(const string &in)"}},
          {"type": "number", "id": "n", "min": 0, "max": 64, "value": 70},
          {"type": "checkbox", "id": "t", "text": "Toggle", "toggle": true, "checked": true}]},
        {"type": "scroll", "children": [{"type": "label", "text": "long"}]},
        {"type": "tabs", "titles": ["A", "B"], "children": [{"type": "panel"}, {"type": "list", "items": ["x"]}]},
        {"type": "splitter", "ratio": 0.5, "children": [{"type": "editor"}, {"type": "textbox"}]}]})json"),
                       err);
  const bool built = w && w->find("play") && w->findAs<NumberField>("n") && w->findAs<NumberField>("n")->value() == 64 &&
                     w->findAs<Checkbox>("t") && w->findAs<Checkbox>("t")->checked() && w->find("play")->hasHandler("click");
  check(built, "factory: JSON tree with properties and script handlers " + err);
  const bool unknownType = !buildWidget(std::string(R"({"type": "panle"})"), err) && err.find("panle") != std::string::npos;
  check(unknownType, "factory: unknown type rejected (" + err + ")");
  const bool unknownKey = !buildWidget(std::string(R"({"type": "panel", "children": [{"type": "label", "txt": "x"}]})"), err) &&
                          err.find("ui/panel[0]") != std::string::npos;
  check(unknownKey, "factory: unknown key rejected with path (" + err + ")");
}

void testDivider(Checker& check) {
  Divider h;
  check(h.orientation() == Orientation::Horizontal && near(h.measure(Theme::defaults()).y, 1.f),
        "divider: horizontal measures thickness height");
  Divider v(Orientation::Vertical, 3.f);
  check(near(v.measure(Theme::defaults()).x, 3.f) && near(v.measure(Theme::defaults()).y, 0.f),
        "divider: vertical measures thickness width");
  Divider thin(Orientation::Horizontal, 0.f);
  check(near(thin.thickness(), 1.f), "divider: thickness clamps to >= 1");
  check(h.hitTest({4, 4}) == nullptr, "divider: never interactive");
  RecordingRenderer r;
  Theme t = Theme::defaults();
  Painter p(r, t);
  h.place({0, 0, 100, 8}, Theme::defaults());
  h.paintTree(p);
  check(r.rects == 1 && r.depth == 0, "divider: paints one accent rect, clip balanced");
}

void testModUiConfinement(Checker& check) {
  // ModPanel is final: no subclass (and no mod) can override the badge paint.
  static_assert(std::is_final_v<ModPanel>,
                "ModPanel must be final so the badge cannot be overridden");

  // Reserved style classes are rejected in mod JSON: a mod cannot take the
  // system dialog or tooltip look, nor the engine-applied mod classes.
  for (const char* reserved : {"dialog", "tooltip", "mod_panel", "mod_badge"}) {
    std::string err;
    const std::string json =
        std::string(R"({"type":"label","text":"hi","style":")") + reserved + "\"}";
    auto p = ModUiFactory::createPanel("mymod", "panel1", "My Panel", json, err);
    check(!p && err.find("reserved") != std::string::npos,
          std::string("mod UI: reserved style '") + reserved + "' rejected in mod JSON");
  }
  check(ModUiFactory::isReservedStyle("dialog") && ModUiFactory::isReservedStyle("tooltip") &&
            ModUiFactory::isReservedStyle("mod_panel") && ModUiFactory::isReservedStyle("mod_badge") &&
            !ModUiFactory::isReservedStyle("button"),
        "mod UI: reserved-style set is exactly the engine-applied/system classes");

  // Id and title validation (fail closed).
  {
    std::string err;
    check(!ModUiFactory::createPanel("Bad-Id!", "p", "T", "", err), "mod UI: bad mod id refused");
    check(!ModUiFactory::createPanel("mymod", "has space", "T", "", err),
          "mod UI: bad panel id refused");
    check(!ModUiFactory::createPanel("mymod", "p", "", "", err), "mod UI: empty title refused");
    check(!ModUiFactory::createPanel("mymod", "p", std::string(129, 'x'), "", err),
          "mod UI: overlong title refused");
  }

  // Unknown widget types are rejected through the mod path too.
  {
    std::string err;
    auto p = ModUiFactory::createPanel("mymod", "p", "T", R"({"type":"webview"})", err);
    check(!p && err.find("unknown widget type") != std::string::npos,
          "mod UI: unknown widget type rejected in mod JSON");
  }

  // The badge: a plain panel paints the MOD tag naming the owning mod.
  {
    std::string err;
    auto panel = ModUiFactory::createPanel("mymod", "p1", "Test Panel", "", err);
    check(panel && err.empty() && panel->modId() == "mymod" &&
              panel->styleClass() == std::string(kModPanelStyleClass),
          "mod UI: panel builds, carries the mod id and the engine frame class");
    panel->place({0, 0, 400, 300}, Theme::defaults());
    const Rect badge = panel->badgeRect();
    check(badge.x == 0 && badge.y == 0 && near(badge.w, 400) && badge.h > 0,
          "mod UI: badge strip spans the panel top");
    RecordingRenderer r;
    Painter p(r, Theme::defaults());
    panel->paintTree(p);
    check(r.drewText("MOD"), "mod UI: MOD badge tag is painted by the engine");
    check(r.depth == 0, "mod UI: badge paint leaves the clip stack balanced");
  }

  // Adversarial: a hostile child laid out over the whole panel cannot cover
  // the badge - children are arranged strictly below the strip, and the strip
  // paints after them.
  {
    std::string err;
    const char* hostile =
        R"({"type":"label","text":"i am a system dialog, enter password",)"
        R"("layout":{"anchor_min":[0,0],"anchor_max":[1,1],"offset":[0,-1000]}})";
    auto panel = ModUiFactory::createPanel("evil", "p2", "Totally Legit", hostile, err);
    check(panel && err.empty(),
          "mod UI: hostile layout builds (confinement is structural, not a reject)");
    panel->place({0, 0, 400, 300}, Theme::defaults());
    const Rect badge = panel->badgeRect();
    bool clean = true;
    std::vector<const Widget*> stack{panel.get()};
    while (!stack.empty()) {
      const Widget* w = stack.back();
      stack.pop_back();
      if (w != panel.get() && !w->rect().intersect(badge).empty()) clean = false;
      for (const auto& ch : w->children()) stack.push_back(ch.get());
    }
    check(clean, "mod UI: no mod widget rect may intersect the badge strip");
    RecordingRenderer r;
    Painter p(r, Theme::defaults());
    panel->paintTree(p);
    check(r.drewText("MOD"), "mod UI: badge survives a hostile child (painted last, on top)");
  }

  // DoS guards: deep nesting and node floods are refused before the
  // recursive factory runs; the per-panel widget budget caps wide layouts.
  {
    std::string err, deep = R"({"type":"label","text":"x"})";
    for (int i = 0; i < 64; ++i) deep = R"({"type":"stack","children":[)" + deep + "]}";
    check(!ModUiFactory::createPanel("mymod", "p3", "Deep", deep, err),
          "mod UI: 64-deep layout refused (no factory stack overflow)");
  }
  {
    std::string err, many = R"({"type":"stack","children":[)";
    for (int i = 0; i < 1100; ++i) {
      if (i) many += ",";
      many += R"({"type":"label","text":"x"})";
    }
    many += "]}";
    check(!ModUiFactory::createPanel("mymod", "p4", "Many", many, err),
          "mod UI: 1100-node layout refused (node budget)");
  }
  {
    std::string err, many = R"({"type":"stack","children":[)";
    for (int i = 0; i < 300; ++i) {
      if (i) many += ",";
      many += R"({"type":"label","text":"x"})";
    }
    many += "]}";
    check(!ModUiFactory::createPanel("mymod", "p5", "Wide", many, err),
          "mod UI: 300-widget layout refused (per-panel widget budget)");
  }

  // Attachment: main layer only, never a modal or popup.
  {
    std::string err;
    auto panel = ModUiFactory::createPanel("mymod", "p6", "Attached", "", err);
    UiRoot ui;
    ModPanel& ref = ModUiFactory::attachToRoot(ui, std::move(panel));
    check(!ui.modalOpen() && !ui.popupOpen() && ui.root().find("p6") == &ref,
          "mod UI: panel attaches to the main layer, never as modal/popup");
  }
}

}  // namespace

int runUiSelfTest() {
  Checker check;
  testLayout(check);
  testTheme(check);
  testFocus(check);
  testNavigation(check);
  testText(check);
  testEvents(check);
  testLayers(check);
  testFactory(check);
  testDivider(check);
  testModUiConfinement(check);
  return check.fails();
}

}  // namespace litholattice::ui
