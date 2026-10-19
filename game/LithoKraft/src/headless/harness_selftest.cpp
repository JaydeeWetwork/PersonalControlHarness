// Headless harness self-tests: drive the REAL Game and the REAL retained UI through the
// menu state machine and assert the Game-level transitions. Clicks go through UiRoot
// exactly like the app's event loop, so these prove the wiring, not just the widgets.
#include "headless_harness.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include "lithokraft/game/game.h"
#include "lithokraft/game/game_ui.h"
#include "lithokraft/game/settings.h"
#include "litholattice/ui/controls.h"
#include "litholattice/ui/ui_root.h"

namespace lithokraft {
namespace {

class Checker {
 public:
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] headless: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }

 private:
  int fails_ = 0;
};

litholattice::ui::Vec2 center(const litholattice::ui::Widget& w) {
  return {w.rect().x + w.rect().w * 0.5f, w.rect().y + w.rect().h * 0.5f};
}

void click(litholattice::ui::UiRoot& ui, litholattice::ui::Vec2 p) {
  ui.handleEvent(litholattice::ui::UiEvent::move(p));
  ui.handleEvent(litholattice::ui::UiEvent::down(p));
  ui.handleEvent(litholattice::ui::UiEvent::up(p));
}

/// Click a widget by id inside the game's UI (layout must have run: call hg.frame() first).
/// Searches the root tree, then the top modal (pause / settings dialogs live there).
bool clickId(HeadlessGame& hg, const char* id) {
  auto& ui = hg.game().gameUi().root();
  litholattice::ui::Widget* w = ui.root().find(id);
  if (!w && ui.topModal()) w = ui.topModal()->find(id);
  if (!w) return false;
  click(ui, center(*w));
  return true;
}

void testBootToMenu(Checker& check) {
  HeadlessGame hg;
  Game& g = hg.game();
  check(g.ok(), "boot: the game constructs headless (world loaded, no window/GPU)");
  g.enterMenu();  // main.cpp does this on boot (unless --bench)
  hg.frame();
  check(g.inMenu(), "boot: enterMenu puts the game in the Menu state");
  check(g.gameUi().menuOpen(), "boot: the launch menu overlay is open");
  auto* title = g.gameUi().root().root().findAs<litholattice::ui::Label>("menu-title");
  auto* tag = g.gameUi().root().root().findAs<litholattice::ui::Label>("menu-tagline");
  check(title && title->text() == "LITHOKRAFT", "boot: menu shows the LITHOKRAFT title");
  check(tag && tag->text() == "A LITHOLATTICE GAME", "boot: tagline reads 'A LITHOLATTICE GAME'");
}

void testMenuToPlay(Checker& check) {
  HeadlessGame hg;
  Game& g = hg.game();
  g.enterMenu();
  hg.frame();  // layout, so the buttons are hit-testable
  check(clickId(hg, "menu-play"), "menu->play: the Play button exists and is clickable");
  check(!g.inMenu(), "menu->play: clicking Play leaves the Menu state");
  check(!g.paused(), "menu->play: the sim is unpaused in creative/testing");
  check(!g.gameUi().menuOpen(), "menu->play: the menu overlay is closed");
}

void testSettingsApply(Checker& check) {
  HeadlessGame hg;
  Game& g = hg.game();
  g.enterMenu();
  hg.frame();
  // Drive the REAL settings dialog through the game's own callback wiring
  // (GameUi -> Game::onSettingsChanged), not a bare GameUi.
  g.gameUi().openSettings(g.settings());
  hg.frame();
  check(g.gameUi().settingsOpen(), "settings: the dialog opens from the game");
  auto* dlg = g.gameUi().root().topModal();
  auto* vol = dlg ? dlg->findAs<litholattice::ui::Slider>("set-volume") : nullptr;
  check(vol != nullptr, "settings: the volume slider exists");
  if (vol) vol->setValue(40.f, true);  // fires settingsChanged -> Game::onSettingsChanged
  check(std::fabs(g.settings().masterVolume - 0.4f) < 1e-6,
        "settings: the volume change applies to the live game settings");
  auto* mute = dlg ? dlg->findAs<litholattice::ui::Checkbox>("set-mute") : nullptr;
  if (mute) mute->setChecked(true, true);
  check(g.settings().muted, "settings: the mute change applies to the live game settings");
  g.gameUi().closeSettings();
  check(!g.gameUi().settingsOpen(), "settings: the dialog closes");
}

void testPauseFlow(Checker& check) {
  HeadlessGame hg;
  Game& g = hg.game();
  g.enterMenu();
  hg.frame();
  clickId(hg, "menu-play");
  check(!g.inMenu() && !g.paused(), "pause: playing, unpaused");
  g.setPaused(true);
  hg.frame();
  check(g.paused(), "pause: setPaused(true) pauses");
  check(g.gameUi().pauseOpen(), "pause: the pause dialog opens");
  check(clickId(hg, "pause-resume"), "pause: the Resume button exists");
  check(!g.paused(), "pause: Resume unpauses");
  check(!g.gameUi().pauseOpen(), "pause: Resume closes the dialog");
}

void testExitToMenu(Checker& check) {
  HeadlessGame hg;
  Game& g = hg.game();
  g.enterMenu();
  hg.frame();
  clickId(hg, "menu-play");
  g.setPaused(true);
  hg.frame();
  check(clickId(hg, "pause-exit-menu"), "exit-to-menu: the button exists");
  check(g.inMenu(), "exit-to-menu: back in the Menu state");
  check(g.gameUi().menuOpen(), "exit-to-menu: the launch menu overlay is open again");
}

void testQuitFlow(Checker& check) {
  HeadlessGame hg;
  Game& g = hg.game();
  g.enterMenu();
  hg.frame();
  check(!g.quitRequested(), "quit: nothing requested at boot");
  check(clickId(hg, "menu-exit"), "quit: the Exit button exists");
  check(g.quitRequested(), "quit: clicking Exit requests quit");
}

void testFocusLoss(Checker& check) {
  HeadlessGame hg;
  Game& g = hg.game();
  g.enterMenu();
  hg.frame();
  clickId(hg, "menu-play");  // now Playing
  g.onFocusChanged(false);   // window loses focus
  check(g.paused(), "focus-loss: losing focus while Playing pauses");
  check(g.gameUi().pauseOpen(), "focus-loss: the pause menu auto-pops");
  // Back in the menu, focus loss must NOT pop the pause dialog over it.
  g.enterMenu();
  hg.frame();
  const bool pausedBefore = g.paused();
  g.onFocusChanged(false);
  check(g.paused() == pausedBefore && !g.gameUi().pauseOpen(),
        "focus-loss: losing focus in the menu does not pop the pause dialog");
  // Refocus does not unpause by itself.
  g.onFocusChanged(true);
  check(g.paused() == pausedBefore, "focus-loss: refocusing does not unpause by itself");
}

void testFramesRunHeadless(Checker& check) {
  HeadlessGame hg;
  Game& g = hg.game();
  g.enterMenu();
  // Run real frames: sim ticks, mesh jobs upload through the null renderer, the UI updates.
  // This is the "does the game loop survive without a window" test.
  bool meshes = false;
  for (int i = 0; i < 600 && !meshes; ++i) {
    hg.frame();
    meshes = hg.renderer().bytes() > 0;
  }
  check(meshes, "frames: 600 headless frames run; the mesh pipeline uploaded through the null renderer");
  check(hg.renderer().stats().meshes > 0, "frames: the null renderer holds uploaded chunk meshes");
}

/// Drive frames until world column (x, z) has terrain (surfaceY >= 0) or the
/// frame budget runs out; returns the surface height or -1. Probing goes
/// through Game::surfaceYAt (between-ticks read), every few frames.
int waitForSurface(HeadlessGame& hg, Game& g, int x, int z, int maxFrames = 6000) {
  int y = -1;
  for (int f = 0; f < maxFrames && y < 0; ++f) {
    hg.frame();
    if (f % 5 == 0) y = g.surfaceYAt(x, z);
  }
  return y;
}

void testChunkStreamingWalk(Checker& check) {
  // JD's catch (2026-10-12): the Wafer pre-bake only generated a few chunks
  // around spawn. The harness world pre-bakes chunks [0,4) x [0,4) (blocks
  // [0,64) on each axis, spawn at (8, 8)). Teleport-walk the REAL player +x in
  // 32-block steps to x=392 — 328 blocks past the pre-bake edge — running real
  // frames so the game's chunk streamer must generate on demand, and assert
  // terrain underfoot at every sample.
  HeadlessGame hg;  // default Config: worldChunks = 4, no save dir
  Game& g = hg.game();
  check(g.loadedColumnCount() == 16, "streaming: boot loads exactly the 4x4 pre-bake (16 columns)");
  g.enterMenu();
  hg.frame();
  clickId(hg, "menu-play");
  hg.frame();
  bool underfoot = true;
  std::string failedAt;
  for (int step = 0; step <= 12; ++step) {
    const int x = 8 + 32 * step;
    Vec3 p = g.playerPosition();
    p.x = float(x);
    p.y = 160.f;  // well above terrain: position only steers the streamer
    g.setPlayerPosition(p);
    if (waitForSurface(hg, g, x, 8) < 0) {
      underfoot = false;
      failedAt = " at x=" + std::to_string(x);
      break;
    }
  }
  check(underfoot, "streaming: terrain underfoot at every 32-block sample, spawn to +384 blocks" + failedAt);
  check(g.streamedColumnCount() > 100,
        "streaming: columns far beyond the pre-bake were generated on demand (streamed=" +
            std::to_string(g.streamedColumnCount()) + ", loaded=" + std::to_string(g.loadedColumnCount()) + ")");
}

void testInventoryBinding(Checker& check) {
  // P2b-1: ONE inventory. A fresh game holds the Fragment in hotbar slot 0
  // of the canonical Inventory and the creative kit as real stacks in the
  // same store; the HUD hotbar widget renders that store (slot 0 shows the
  // Fragment's item icon); a save/relaunch cycle keeps the Fragment
  // exactly once (persisted inventory, no duplicate boot grant).
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "lithokraft_inv_test_save";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  HeadlessGame::Config cfg;
  cfg.worldChunks = 4;
  cfg.saveDir = dir.string();
  {
    HeadlessGame hg(cfg);
    Game& g = hg.game();
    const Inventory& inv = g.playerInventory();
    check(inv.hotbar()[0].itemId == kItemFragment && inv.countOf(kItemFragment) == 1,
          "inventory: fresh spawn -- the Fragment is in hotbar slot 0 of the canonical inventory");
    check(inv.hotbar()[1].itemId == uint32_t(kWaferSilicon) && inv.hotbar()[1].count == kMaxStack,
          "inventory: the fresh-spawn creative kit is real stacks in the same inventory");
    g.enterMenu();
    hg.frame();
    clickId(hg, "menu-play");
    hg.frame();
    // HUD binding at the widget level: slot 0 of the rendered hotbar shows
    // the Fragment's item icon, and the selected slot is the game's.
    auto* bar = g.gameUi().root().root().find("hotbar");
    const ui::Slot* s0 = bar ? dynamic_cast<const ui::Slot*>(bar->children()[0].get()) : nullptr;
    check(s0 && s0->icon() == itemIconRef(kItemFragment),
          "inventory: the HUD hotbar renders the inventory -- slot 0 shows the Fragment's item icon");
    const ui::Slot* ssel = bar ? dynamic_cast<const ui::Slot*>(bar->children()[size_t(g.selectedHotbarSlot())].get()) : nullptr;
    check(ssel && ssel->selected(), "inventory: the HUD selection is the game's selected inventory slot");
    check(g.saveWorld("inv-test", true), "inventory: saveWorld writes the player (v4 inventory block)");
  }
  {
    HeadlessGame hg(cfg);  // relaunch on the same save dir
    Game& g = hg.game();
    check(g.playerInventory().countOf(kItemFragment) == 1 &&
              g.playerInventory().hotbar()[0].itemId == kItemFragment,
          "inventory: after relaunch the persisted Fragment is kept exactly once -- no duplicate grant");
    check(g.playerInventory().hotbar()[1].itemId == uint32_t(kWaferSilicon) &&
              g.playerInventory().hotbar()[1].count == kMaxStack,
          "inventory: hotbar stacks persist across relaunch (the HUD reads the same store that saved)");
  }
  std::filesystem::remove_all(dir, ec);
}

void testStreamingPersistence(Checker& check) {
  // A streamed column must persist through the existing save path and come
  // back on the next launch (the streamer asks the disk before generating).
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "lithokraft_stream_test_save";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  HeadlessGame::Config cfg;
  cfg.worldChunks = 4;
  cfg.saveDir = dir.string();
  {
    HeadlessGame hg(cfg);
    Game& g = hg.game();
    g.enterMenu();
    hg.frame();
    clickId(hg, "menu-play");
    hg.frame();
    Vec3 p = g.playerPosition();
    p.x = 200.f;  // chunk 12: far outside the [0,4) pre-bake
    p.y = 160.f;
    g.setPlayerPosition(p);
    check(waitForSurface(hg, g, 200, 8) >= 0, "persist: streamed terrain exists at x=200 before saving");
    check(g.saveWorld("stream-test", true), "persist: saveWorld writes the world (incl. streamed columns)");
  }  // game destroyed: exit save runs too
  {
    HeadlessGame hg(cfg);  // same save dir: the player restores at x=200
    Game& g = hg.game();
    g.enterMenu();  // snapshots the restored position; Play hands it back
    hg.frame();
    clickId(hg, "menu-play");
    hg.frame();
    check(waitForSurface(hg, g, 200, 8) >= 0,
          "persist: after relaunch, terrain at x=200 (outside the pre-bake) returns via streaming");
    check(g.streamedColumnCount() > 0, "persist: the relaunch streamed columns in (not just the pre-bake)");
  }
  std::filesystem::remove_all(dir, ec);
}

void testFragmentOwnership(Checker& check) {
  // A3 (brief B1 no-decision subset, 2026-10-13): Game owns the production
  // FragmentState, and Game::renderFragmentText feeds the markdown flag
  // from that state — proven here against the REAL Game.
  HeadlessGame hg;
  Game& g = hg.game();
  check(g.ok(), "fragment (A3): game constructs for the Fragment ownership test");
  check(!g.fragmentState().markdown_unlocked() && g.fragmentState().tier() == 0,
        "fragment (A3): production Game owns a fresh FragmentState (tier 0, markdown locked)");
  const std::string src = "// # Signal Notes\n// plain line\n";
  const auto pre = g.renderFragmentText(src, FragmentView::Rendered);
  bool headingPre = false;
  for (const auto& b : pre.doc.blocks) headingPre = headingPre || b.kind == BlockKind::Heading;
  check(pre.ok && !pre.doc.blocks.empty() && !headingPre,
        "fragment (A3): renderFragmentText feeds markdown_unlocked()=false (plain paragraphs, no heading parse)");
  // No production upgrade trigger exists (JD decision C3 — not invented
  // here); the test drives the owned state's API directly to prove the
  // render path follows the state, which is the wiring being landed.
  g.fragmentState().upgradeFragment();
  check(g.fragmentState().markdown_unlocked() && g.fragmentState().tier() == 1,
        "fragment (A3): upgradeFragment applies on the production-owned state");
  const auto post = g.renderFragmentText(src, FragmentView::Rendered);
  bool headingPost = false;
  for (const auto& b : post.doc.blocks) headingPost = headingPost || b.kind == BlockKind::Heading;
  check(headingPost, "fragment (A3): after upgrade, the same production render path parses markdown (flag from state)");
}

}  // namespace

int runHeadlessHarnessSelfTest() {
  Checker check;
  testBootToMenu(check);
  testMenuToPlay(check);
  testSettingsApply(check);
  testPauseFlow(check);
  testExitToMenu(check);
  testQuitFlow(check);
  testFocusLoss(check);
  testFramesRunHeadless(check);
  testChunkStreamingWalk(check);
  testInventoryBinding(check);
  testStreamingPersistence(check);
  testFragmentOwnership(check);
  return check.fails();
}

}  // namespace lithokraft
