#include "lithokraft/game/game_ui_selftest.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#ifdef _WIN32
#include <process.h>  // _getpid: per-run unique temp dirs (G-M7, P2b-2)
#define getpid _getpid
#else
#include <unistd.h>  // getpid: per-run unique temp dirs (G-M7, P2b-2)
#endif
#include "lithokraft/game/game_ui.h"
#include "lithokraft/content/items.h"  // kItemFragment / kItemBashor / itemIconRef (P2b-1 HUD binding)
#include "lithokraft/game/script_editor.h"
#include "lithokraft/game/ui_script_bindings.h"
#include "litholattice/mods/mod_interface.h"  // kPermUI for the script sandbox
#include "litholattice/render/hud_ui_renderer.h"
#include "litholattice/render/overlay_geometry.h"
#include "litholattice/ui/containers.h"
#include "litholattice/ui/controls.h"
#include "litholattice/ui/lists.h"
#include "litholattice/ui/text_edit.h"

namespace lithokraft {
namespace {

class Checker {
 public:
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] game ui: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }

 private:
  int fails_ = 0;
};

ui::Vec2 center(const ui::Widget& w) { return {w.rect().x + w.rect().w * 0.5f, w.rect().y + w.rect().h * 0.5f}; }
void click(ui::UiRoot& ui, ui::Vec2 p, uint8_t mods = ui::kModNone) {
  ui::UiEvent down = ui::UiEvent::down(p), up = ui::UiEvent::up(p);
  down.mods = up.mods = mods;
  ui.handleEvent(ui::UiEvent::move(p));
  ui.handleEvent(down);
  ui.handleEvent(up);
}

std::string readFile(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void writeFile(const std::filesystem::path& p, const std::string& text) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
}

void testHudRenderer(Checker& check) {
  HudLayer hud;
  hud.rects.push_back({0, 0, 10, 10, 1, 1, 1, 1});
  HudUiRenderer r(hud, [](std::string_view name) -> std::optional<ui::Color> {
    if (name == "red") return ui::Color{1, 0, 0, 1};
    return std::nullopt;
  });
  r.fillRect({0, 0, 100, 100}, {0, 0, 1, 1});
  r.pushClip({10, 10, 20, 20});
  r.pushClip({20, 0, 100, 100});  // intersects to (20,10)-(30,30)
  r.fillRect({0, 0, 100, 100}, {0, 1, 0, 1});
  r.drawText({0, 12}, "ABCDEFGH", {1, 1, 1, 1}, 1.f);
  r.popClip();
  r.popClip();
  r.drawImage({0, 0, 4, 4}, "red", {1, 1, 1, 0.5f});
  r.drawImage({0, 0, 4, 4}, "nope", {1, 1, 1, 1});
  std::vector<HudVertex> v;
  buildHudVertices(hud, v);
  // legacy rect (6) + full rect (6) + clipped rect (6) + 2 glyphs inside x 20..30 at 8 px (6 each) + 2 images (12).
  bool clipped = v.size() == 6 + 6 + 6 + 12 + 12;
  for (size_t i = 12; i < 30 && i < v.size(); ++i) clipped = clipped && v[i].x >= 20 && v[i].x <= 30 && v[i].y >= 10 && v[i].y <= 30;
  check(clipped, "HudUiRenderer: draws after the legacy HUD, nested clips intersect, glyphs cut (" + std::to_string(v.size()) + " verts)");
  const bool order = v.size() >= 12 && v[6].b == 1.f && v[0].r == 1.f && v[0].b == 1.f;
  const bool images = hud.draws.size() == 5 && hud.draws[3].r == 1 && hud.draws[3].a == 0.5f && hud.draws[4].g == 0 &&
                      hud.draws[4].r == 1 && hud.draws[4].b == 1;
  check(order && images, "HudUiRenderer: paint order kept; images resolve to swatches (magenta if unknown)");
}

void testScriptBindings(Checker& check) {
  ScriptEngine scripts;
  ui::UiRoot ui;
  UiScriptBindings bindings(scripts, ui);
  const bool ready = [&] {
    if (!scripts.init()) return false;
    scripts.setModPermissions(kPermUI);  // the ui_* bindings require the UI permission
    return bindings.registerApi();
  }();
  ui.setScriptHost(&bindings);
  auto& stack = ui.root().add<ui::StackPanel>();
  stack.setLayout(ui::Layout::at({0, 0}, {0, 0}, {}, {200, 200}));
  auto& btn = stack.add<ui::Button>("Go");
  btn.setId("go");
  btn.onScript("click", "void onGo(const string &in)");
  stack.add<ui::Label>("").setId("lbl");
  stack.add<ui::NumberField>(0, 64, 0).setId("n");
  const bool built = scripts.loadSource("ui_test", R"(
    void onGo(const string &in id) {
      ui_setText("lbl", "clicked " + id);
      ui_setValue("n", ui_value("n") + 7);
    }
    void onClose(const string &in id) { ui_close("popup"); }
    void onLoad() {
      ui_show("{\"type\": \"panel\", \"id\": \"popup\", \"children\": [{\"type\": \"button\", \"id\": \"x\", \"text\": \"X\", \"on\": {\"click\": \"void onClose(const string &in)\"}}]}");
    })");
  ui.layout({800, 600});
  click(ui, center(btn));
  click(ui, center(btn));
  auto* lbl = ui.root().findAs<ui::Label>("lbl");
  auto* n = ui.root().findAs<ui::NumberField>("n");
  check(ready && built && lbl && lbl->text() == "clicked go" && n && n->value() == 14,
        "script bindings: widget event calls the script handler, which reads / writes widgets by id");
  scripts.callHook("void onLoad()");
  ui.layout({800, 600});
  ui::Widget* x = ui.root().find("x");
  const bool shown = x != nullptr;
  if (x) click(ui, center(*x));
  check(shown && ui.root().find("popup") == nullptr, "script bindings: ui_show builds JSON UI; its handler closes itself");
}

void testScriptEditor(Checker& check) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("lithokraft_ui_selftest_scripts-" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  writeFile(dir / "a.as", "void onLoad() { log(\"v1\"); }\n");
  writeFile(dir / "b.as", "int helper() { return 1; }\n");
  ScriptEngine scripts;
  scripts.init();
  scripts.loadDirectory(dir.string());
  scripts.callHook("void onLoad()");
  ui::UiRoot ui;
  ui.layout({1280, 720});
  int reloads = 0;
  ScriptEditor editor(ui, scripts, dir, [&] {
    ++reloads;
    scripts.callHook("void onLoad()");
  });
  editor.open();
  ui.layout({1280, 720});
  const bool opened = editor.isOpen() && editor.currentFile() == "a.as" && editor.editor() &&
                      editor.editor()->text().find("v1") != std::string::npos && !editor.modified();
  check(opened && scripts.lastLog() == "v1", "script editor: opens with the first scripts/*.as file");

  ui::TextEditor& ed = *editor.editor();
  ed.focus();
  ed.buffer().selectAll();
  ed.buffer().insert("void onLoad() { log(\"v2\"); }\n");
  ui.handleEvent(ui::UiEvent::keyDown(ui::Key::S, ui::kModCtrl));
  check(readFile(dir / "a.as").find("v2") != std::string::npos && reloads == 1 && scripts.lastLog() == "v2" && !editor.modified(),
        "script editor: Ctrl+S saves and hot-reloads (onLoad ran again)");

  ed.buffer().setCaret({0, 0});
  ed.buffer().selectAll();
  ed.buffer().insert("void onLoad() {\n  log(\"v3\")\n  oops;\n}\n");
  const bool saved = editor.save();
  scripts.callHook("void onLoad()");
  check(!saved && !scripts.buildErrors().empty() && scripts.lastLog() == "v2" && ed.buffer().caret().line >= 1 &&
            editor.status().find("compile failed") != std::string::npos,
        "script editor: a compile error keeps the old scripts running, jumps to the error (" +
            (scripts.buildErrors().empty() ? std::string("-") : scripts.buildErrors().front()) + ")");

  ed.buffer().insert("x");
  ui.handleEvent(ui::UiEvent::keyDown(ui::Key::Escape));
  const bool asked = editor.isOpen() && ui.topModal() && ui.topModal()->id() != "script-editor";
  ui.handleEvent(ui::UiEvent::keyDown(ui::Key::Escape));  // cancel the question
  const bool stillOpen = editor.isOpen() && editor.modified();
  editor.close(true);
  check(asked && stillOpen && !editor.isOpen() && !ui.modalOpen(), "script editor: Esc with unsaved changes asks first");
  fs::remove_all(dir, ec);
}

void testGameUi(Checker& check) {
  int selected = -1, closed = 0;
  BlockId picked = kAir;
  bool shift = false;
  GameUi gui({[&](int s) { selected = s; }, [&](BlockId b, bool sh) { picked = b; shift = sh; }, [&] { ++closed; },
              {}, {}, {}, {}, {}, {}});
  HudModel m;
  m.hotbar = {ItemStack{1, 1}, ItemStack{2, 1}, ItemStack{3, 1}, {}, {}, {}, {}, {}};
  m.selected = 1;
  m.debugLines = {"FPS 60", "TPS 32"};
  m.controlLines = {"H show controls"};
  gui.update(m, {1280, 720}, 0.016f);
  ui::Widget* bar = gui.root().root().find("hotbar");
  const bool hud = bar && bar->children().size() == kHotbarSlots && bar->rect().bottom() <= 720 && bar->rect().x > 0;
  if (bar) click(gui.root(), center(*bar->children()[4]));
  check(hud && selected == 4, "HUD: hotbar is bottom-centred; clicking a slot selects it");

  // P2b-1 HUD binding: the model carries the inventory's real stacks, and
  // the widgets render THEM -- the Fragment and a tool show their item
  // icons (not block swatches), a block stack shows its block icon, and
  // the selected slot's name is the stack's real name.
  {
    HudModel im;
    im.hotbar = {ItemStack{kItemFragment, 1}, ItemStack{kItemBashor, 1}, ItemStack{uint32_t(kLamp), 33},
                 {}, {}, {}, {}, {}};
    im.selected = 0;
    gui.update(im, {1280, 720}, 0.016f);
    ui::Widget* bar2 = gui.root().root().find("hotbar");
    const ui::Slot* s0 = bar2 ? dynamic_cast<const ui::Slot*>(bar2->children()[0].get()) : nullptr;
    const ui::Slot* s1 = bar2 ? dynamic_cast<const ui::Slot*>(bar2->children()[1].get()) : nullptr;
    const ui::Slot* s2 = bar2 ? dynamic_cast<const ui::Slot*>(bar2->children()[2].get()) : nullptr;
    check(s0 && s0->icon() == itemIconRef(kItemFragment) && s0->selected(),
          "HUD: hotbar slot 0 renders the Fragment stack with its item icon, selected");
    check(s1 && s1->icon() == itemIconRef(kItemBashor), "HUD: a tool stack renders with its item icon");
    check(s2 && s2->icon() == itemIconRef(uint32_t(kLamp)), "HUD: a block stack renders with its block icon");
    // The selected slot's name label (the label laid out above the bar).
    bool nameOk = false;
    const auto findName = [&nameOk](const auto& self, const ui::Widget& w) -> void {
      if (const auto* l = dynamic_cast<const ui::Label*>(&w); l && l->text() == "The Fragment") nameOk = true;
      for (const auto& c : w.children()) self(self, *c);
    };
    findName(findName, gui.root().root());
    check(nameOk, "HUD: the selected slot's name reads 'The Fragment' (the real stack, not a block)");
  }
  gui.update(m, {1600, 1000}, 0.016f);
  const bool scaled = bar != gui.root().root().find("hotbar") && gui.root().theme().scale() == 2.f;
  check(scaled, "HUD: rebuilt at scale 2 for tall windows");

  gui.openInventory(1);
  gui.update(m, {1600, 1000}, 0.016f);
  ui::Widget* inv = gui.root().topModal();
  ui::Widget* firstCell = nullptr;
  // The first slot of the inventory grid (block 1).
  const auto findSlot = [](const auto& self, ui::Widget& w) -> ui::Widget* {
    if (dynamic_cast<ui::Slot*>(&w)) return &w;
    for (const auto& c : w.children())
      if (ui::Widget* s = self(self, *c)) return s;
    return nullptr;
  };
  if (inv) firstCell = findSlot(findSlot, *inv);
  if (firstCell) click(gui.root(), center(*firstCell), ui::kModShift);
  check(gui.inventoryOpen() && picked == 1 && shift, "inventory: modal dialog; shift-click picks the block");
  // P2b-1 honesty: the dialog is the creative BLOCK PALETTE (it gives full
  // stacks into the real inventory hotbar), and it says so in its title.
  {
    const auto* dlg = dynamic_cast<const ui::Dialog*>(inv);
    check(dlg && dlg->title().find("Block palette") != std::string::npos &&
              dlg->title().find("Creative inventory") == std::string::npos,
          "palette: the dialog title honestly reads 'Block palette (creative)', not 'Creative inventory'");
    check(inv && inv->find("palette-note") != nullptr, "palette: the dialog explains the full-stack give");
  }
  gui.root().handleEvent(ui::UiEvent::keyDown(ui::Key::Escape));
  check(!gui.inventoryOpen() && closed == 1, "inventory: Escape closes it and tells the game");
  gui.openInventory(0);
  gui.closeInventory();
  check(!gui.inventoryOpen() && closed == 1 && !gui.root().modalOpen(), "inventory: closing from the game sends no callback");
}

void testSettingsFile(Checker& check) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("lithokraft_settings_selftest-" + std::to_string(::getpid()));
  const fs::path p = dir / "settings.json";
  std::error_code ec;
  fs::remove_all(dir, ec);

  GameSettings s;
  s.masterVolume = 0.25f; s.muted = true; s.hudScale = 2; s.displayMode = 3;
  s.vsync = true; s.tps = 256; s.tickRadius = 64; s.euv = false; s.pbrTier = 3;
  check(s.save(p.string()), "settings: save writes config/settings.json (creating directories)");
  const GameSettings r = GameSettings::load(p.string());
  const bool roundTrip = std::fabs(r.masterVolume - 0.25f) < 1e-6 && r.muted && r.hudScale == 2 &&
                         r.displayMode == 3 && r.vsync && r.tps == 256 && r.tickRadius == 64 && !r.euv &&
                         r.pbrTier == 3;
  check(roundTrip, "settings: load round-trips every field");

  writeFile(p, "{ not json");
  const GameSettings corrupt = GameSettings::load(p.string());
  check(corrupt.tps == 32 && corrupt.tickRadius == 8 && !corrupt.muted && corrupt.euv &&
        corrupt.pbrTier == 1,
        "settings: a corrupt file falls back to defaults");

  writeFile(p, R"({"masterVolume": 9, "tps": 100, "tickRadius": -5, "hudScale": 7, "displayMode": 42, "pbrTier": 99})");
  const GameSettings c = GameSettings::load(p.string());
  check(c.masterVolume == 1.f && c.tps == 128 && c.tickRadius == 0 && c.hudScale == 2 && c.displayMode == 3 &&
        c.pbrTier == 3,
        "settings: out-of-range values clamp to the nearest valid (volume 1, tps 128, radius 0, pbrTier 3)");

  const GameSettings missing = GameSettings::load((dir / "nope.json").string());
  check(missing.tps == 32 && missing.masterVolume == 0.6f, "settings: a missing file loads defaults");
  fs::remove_all(dir, ec);
}

void testLaunchMenu(Checker& check) {
  bool played = false, quit = false, settingsOpened = false;
  GameUi gui({{}, {}, {}, {}, [&] { quit = true; }, [&] { played = true; }, {}, [&] { settingsOpened = true; }, {}});
  HudModel m;
  m.visible = false;  // the game hides the HUD under the menu overlay
  gui.openMenu("9.9.9-test");
  gui.update(m, {1280, 720}, 0.016f);
  check(gui.menuOpen(), "menu: openMenu builds the overlay");
  check(gui.root().theme().style("title").textScale == 4.5f, "menu: the 'title' style gives the big title text");
  ui::Label* title = gui.root().root().findAs<ui::Label>("menu-title");
  ui::Label* tag = gui.root().root().findAs<ui::Label>("menu-tagline");
  ui::Label* ver = gui.root().root().findAs<ui::Label>("menu-version");
  check(title && title->text() == "LITHOKRAFT", "menu: big LITHOKRAFT title");
  check(tag && tag->text() == "A LITHOLATTICE GAME", "menu: tagline reads 'A LITHOLATTICE GAME'");
  check(ver && ver->text().find("9.9.9-test") != std::string::npos, "menu: version footer shows the plumbed version");
  check(gui.root().root().find("menu-divider") != nullptr, "menu: divider under the tagline");
  ui::Button* play = gui.root().root().findAs<ui::Button>("menu-play");
  ui::Button* set = gui.root().root().findAs<ui::Button>("menu-settings");
  ui::Button* exit = gui.root().root().findAs<ui::Button>("menu-exit");
  check(play && set && exit, "menu: Play / Settings / Exit buttons");
  if (play) click(gui.root(), center(*play));
  check(played, "menu: Play dispatches the play callback");
  if (set) click(gui.root(), center(*set));
  check(settingsOpened, "menu: Settings dispatches openSettings");
  if (exit) click(gui.root(), center(*exit));
  check(quit, "menu: Exit dispatches quit");
  played = false;
  if (play) {
    gui.root().setFocus(play);  // keyboard-first: Enter on the focused button
    gui.root().handleEvent(ui::UiEvent::keyDown(ui::Key::Enter));
  }
  check(played, "menu: Enter on the focused Play button starts the game");
  gui.setHudScalePref(2);
  gui.update(m, {1280, 720}, 0.016f);
  check(gui.root().theme().scale() == 2.f, "menu: HUD scale preference overrides the auto scale");
  gui.setHudScalePref(0);
  gui.update(m, {1280, 720}, 0.016f);
  check(gui.root().theme().scale() == 1.f, "menu: HUD scale 0 falls back to auto");
  gui.closeMenu();
  check(!gui.menuOpen() && gui.root().root().find("menu") == nullptr, "menu: closeMenu removes the overlay");
}

void testPauseMenu(Checker& check) {
  bool resumed = false, quit = false, toMenu = false, settingsOpened = false;
  GameUi gui({{}, {}, {}, [&] { resumed = true; }, [&] { quit = true; },
              {}, [&] { toMenu = true; }, [&] { settingsOpened = true; }, {}});
  HudModel m;
  const auto pauseButton = [&](const char* id) {
    ui::Widget* modal = gui.root().topModal();
    return modal ? modal->findAs<ui::Button>(id) : nullptr;
  };
  gui.openPause();
  gui.update(m, {1280, 720}, 0.016f);
  ui::Button* resume = pauseButton("pause-resume");
  ui::Button* set = pauseButton("pause-settings");
  ui::Button* exitMenu = pauseButton("pause-exit-menu");
  ui::Button* quitBtn = pauseButton("pause-quit");
  const bool four = resume && resume->text() == "Resume" && set && set->text() == "Settings" &&
                    exitMenu && exitMenu->text() == "Exit to menu" && quitBtn && quitBtn->text() == "Quit to desktop";
  check(four, "pause: Resume / Settings / Exit to menu / Quit to desktop");
  if (resume) click(gui.root(), center(*resume));
  check(resumed && !gui.pauseOpen(), "pause: Resume closes the dialog and reports it");
  gui.openPause();
  gui.update(m, {1280, 720}, 0.016f);
  if (ui::Button* s2 = pauseButton("pause-settings")) click(gui.root(), center(*s2));
  check(settingsOpened && gui.pauseOpen(), "pause: Settings reports openSettings and keeps the pause dialog");
  if (ui::Button* e2 = pauseButton("pause-exit-menu")) click(gui.root(), center(*e2));
  check(toMenu, "pause: Exit to menu dispatches exitToMenu");
  gui.openPause();
  gui.update(m, {1280, 720}, 0.016f);
  if (ui::Button* q2 = pauseButton("pause-quit")) click(gui.root(), center(*q2));
  check(quit, "pause: Quit to desktop dispatches quit");
}

void testSettingsDialog(Checker& check) {
  GameSettings changed;
  bool fired = false;
  GameUi gui({{}, {}, {}, {}, {}, {}, {}, {}, [&](const GameSettings& s) { changed = s; fired = true; }});
  HudModel m;
  GameSettings cur;
  cur.masterVolume = 0.75f; cur.muted = true; cur.hudScale = 2; cur.displayMode = 3;
  cur.vsync = true; cur.tps = 128; cur.tickRadius = 12; cur.euv = false;
  gui.openSettings(cur);
  gui.update(m, {1280, 720}, 0.016f);
  check(gui.settingsOpen(), "settings: openSettings shows the dialog");
  ui::Widget* dlg = gui.root().topModal();
  check(dlg && dlg->id() == "settings", "settings: the dialog is the top modal");
  ui::TabView* tabs = dlg ? dlg->findAs<ui::TabView>("settings-tabs") : nullptr;
  check(tabs && tabs->count() == 3, "settings: Audio / Video / Sim tabs");
  ui::Slider* vol = dlg ? dlg->findAs<ui::Slider>("set-volume") : nullptr;
  check(vol && std::fabs(vol->value() - 75.f) < 0.01f, "settings: volume slider initialised from the current settings");
  if (vol) vol->setValue(40.f, true);
  check(fired && std::fabs(changed.masterVolume - 0.4f) < 1e-6, "settings: the volume slider reports changes");
  fired = false;
  ui::Checkbox* mute = dlg ? dlg->findAs<ui::Checkbox>("set-mute") : nullptr;
  check(mute && mute->checked(), "settings: mute checkbox initialised");
  if (mute) mute->setChecked(false, true);
  check(fired && !changed.muted, "settings: the mute checkbox reports changes");
  fired = false;
  ui::Dropdown* disp = dlg ? dlg->findAs<ui::Dropdown>("set-display") : nullptr;
  check(disp && disp->selected() == 3, "settings: display mode dropdown initialised (Fullscreen)");
  if (disp) disp->setSelected(1, true);
  check(fired && changed.displayMode == 1, "settings: the display dropdown reports changes");
  fired = false;
  ui::Dropdown* hud = dlg ? dlg->findAs<ui::Dropdown>("set-hud") : nullptr;
  if (hud) hud->setSelected(0, true);
  check(fired && changed.hudScale == 0, "settings: the HUD scale dropdown reports changes");
  fired = false;
  ui::Dropdown* tps = dlg ? dlg->findAs<ui::Dropdown>("set-tps") : nullptr;
  check(tps && tps->selected() == 2, "settings: tick rate dropdown initialised (128)");
  if (tps) tps->setSelected(4, true);
  check(fired && changed.tps == 512, "settings: the tick rate dropdown reports changes");
  fired = false;
  ui::NumberField* radius = dlg ? dlg->findAs<ui::NumberField>("set-radius") : nullptr;
  check(radius && radius->value() == 12, "settings: tick radius field initialised");
  if (radius) radius->setValue(16, true);
  check(fired && changed.tickRadius == 16, "settings: the tick radius field reports changes");
  fired = false;
  ui::Checkbox* euv = dlg ? dlg->findAs<ui::Checkbox>("set-euv") : nullptr;
  if (euv) euv->setChecked(true, true);
  check(fired && changed.euv, "settings: the EUV checkbox reports changes");
  fired = false;
  ui::Dropdown* pbr = dlg ? dlg->findAs<ui::Dropdown>("set-pbrtier") : nullptr;
  check(pbr && pbr->selected() == cur.pbrTier, "settings: PBR transparency dropdown initialised from the current settings");
  if (pbr) pbr->setSelected(3, true);
  check(fired && changed.pbrTier == 3, "settings: the PBR transparency dropdown reports changes (live tier)");
  fired = false;
  ui::Label* pbrHint = dlg ? dlg->findAs<ui::Label>("pbr-tier-hint") : nullptr;
  check(pbrHint != nullptr, "settings: PBR tier pairing hint label present");
  gui.root().handleEvent(ui::UiEvent::keyDown(ui::Key::Escape));
  check(!gui.settingsOpen(), "settings: Escape closes the dialog");
}

}  // namespace

int runGameUiSelfTest() {
  Checker check;
  testHudRenderer(check);
  testScriptBindings(check);
  testScriptEditor(check);
  testGameUi(check);
  testSettingsFile(check);
  testLaunchMenu(check);
  testPauseMenu(check);
  testSettingsDialog(check);
  return check.fails();
}

}  // namespace lithokraft
