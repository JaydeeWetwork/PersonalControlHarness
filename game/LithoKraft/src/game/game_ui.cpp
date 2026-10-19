#include "lithokraft/game/game_ui.h"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "lithokraft/content/blocks.h"
#include "lithokraft/content/items.h"  // itemDisplayName / itemIconRef (P2b-1 stack rendering)
#include "litholattice/render/hud_ui_renderer.h"
#include "litholattice/ui/containers.h"
#include "litholattice/ui/lists.h"

namespace lithokraft {
namespace {

std::string joinLines(const std::vector<std::string>& lines) {
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) out += (i ? "\n" : "") + lines[i];
  return out;
}

const char* blockName(BlockId b) { return b == kAir ? "empty" : blockRegistry().def(b).name.data(); }

std::string blockDescription(BlockId b) {
  std::string text = blockName(b);
  if (const BlockDef& d = blockRegistry().def(b); d.tier)
    text += " - tier " + std::to_string(d.tier) + " palette, wrap mapping (colour = (s-1) mod " + std::to_string(d.tier) + ")";
  return text;
}

}  // namespace

constexpr float kSplashSeconds = 5.f;

std::string blockIcon(BlockId b) { return "block:" + std::to_string(int(b)); }

GameUi::GameUi(Callbacks callbacks) : cb_(std::move(callbacks)) {}

void GameUi::applyTheme(const ui::Theme& theme) {
  ui::Theme t = theme;
  t.setScale(ui_.theme().scale());
  ui_.setTheme(std::move(t));
}

void GameUi::rebuildHud() {
  if (hud_) ui_.destroyLater(ui_.root().removeChild(*hud_));
  hud_ = &ui_.root().add<ui::Widget>();
  hud_->setId("hud");
  const float k = s();

  debugPanel_ = &hud_->add<ui::Panel>("hud");
  debugPanel_->setLayout(ui::Layout::at({0, 0}, {0, 0}, {8 * k, 8 * k}));
  debugText_ = &debugPanel_->add<ui::Label>();

  controlsPanel_ = &hud_->add<ui::Panel>("hud");
  controlsPanel_->setLayout(ui::Layout::at({0, 1}, {0, 1}, {8 * k, -8 * k}));
  controlsText_ = &controlsPanel_->add<ui::Label>();
  controlsText_->setColor({0.9f, 0.95f, 1.f, 1.f});

  banner_ = &hud_->add<ui::Panel>("hud");
  banner_->setLayout(ui::Layout::at({0.5f, 0.3f}, {0.5f, 0}));
  auto& bannerStack = banner_->add<ui::StackPanel>(ui::Orientation::Vertical, 0.f);
  bannerTitle_ = &bannerStack.add<ui::Label>();
  bannerTitle_->setColor({1.f, 0.85f, 0.4f, 1.f});
  bannerText_ = &bannerStack.add<ui::Label>();
  bannerText_->setColor({0.6f, 0.9f, 1.f, 1.f});

  splash_ = &hud_->add<ui::Label>("Powered by LithoLattice");
  splash_->setId("splash");
  splash_->setColor({0.75f, 0.85f, 1.f, 1.f});
  splash_->setLayout(ui::Layout::at({1, 1}, {1, 1}, {-8 * k, -8 * k}));

  auto& bar = hud_->add<ui::StackPanel>(ui::Orientation::Horizontal, 4 * k, "hud");
  bar.setId("hotbar");
  bar.setLayout(ui::Layout::at({0.5f, 1}, {0.5f, 1}, {0, -6 * k}));
  for (int i = 0; i < kHotbarSlots; ++i) {
    ui::Slot& slot = bar.add<ui::Slot>(ui::Vec2{40 * k, 40 * k});
    slot.setCaption(std::to_string(i + 1)).setData(i);
    slot.on("click", [this](ui::Widget& w) {
      if (cb_.selectSlot) cb_.selectSlot(static_cast<ui::Slot&>(w).data());
    });
    slots_[size_t(i)] = &slot;
  }
  slotName_ = &hud_->add<ui::Label>();
  slotName_->setLayout(ui::Layout::at({0.5f, 1}, {0.5f, 1}, {0, -(40 + 6 + 24) * k}));
}

void GameUi::openInventory(int selectedSlot) {
  if (inventory_) return;
  const float k = std::max(1.f, s());
  // P2b-1 honesty: this dialog is a creative BLOCK PALETTE, not the player
  // inventory -- it lists every block in the registry and GIVES a full
  // stack of the clicked block into the player's real inventory hotbar
  // (the same inventory the HUD shows and player.dat persists).
  const std::string title = "Block palette (creative) - click: give a full stack to hotbar slot " +
                            std::to_string(selectedSlot + 1) +
                            ", shift-click: first free slot, E/Esc: close";
  ui::Dialog& dlg = ui::Dialog::show(ui_, title);
  dlg.setId("inventory");
  auto& note = dlg.body().add<ui::Label>(
      "Creative palette: every block in the registry. A click gives a full stack (512) "
      "into your inventory hotbar; placing blocks consumes from the stack.");
  note.setId("palette-note");
  const int cols = 9, count = int(blockRegistry().size()) - 1;  // every block type except air
  const int rows = (count + cols - 1) / cols;
  const float cell = 40 * k;
  auto& scroll = dlg.body().add<ui::ScrollView>();
  const float gridH = float(rows) * cell;
  scroll.setLayout(ui::Layout::sized({float(cols) * cell + ui::ScrollBar::kThickness, std::min(gridH, 6 * cell)}));
  auto& grid = scroll.emplaceContent<ui::GridPanel>(cols, ui::Vec2{cell - 4 * k, cell - 4 * k}, 4 * k);
  for (int i = 0; i < count; ++i) {
    const BlockId b = BlockId(i + 1);
    ui::Slot& slot = grid.add<ui::Slot>(ui::Vec2{cell - 4 * k, cell - 4 * k});
    slot.setIcon(blockIcon(b)).setData(int(b));
    slot.on("click", [this](ui::Widget& w) {
      if (cb_.pickBlock) cb_.pickBlock(BlockId(static_cast<ui::Slot&>(w).data()), (ui_.modifiers() & ui::kModShift) != 0);
    });
  }
  inventoryHover_ = &dlg.body().add<ui::Label>(" ");
  inventoryHover_->setCentered(true);
  dlg.on("close", [this](ui::Widget&) {
    inventory_ = nullptr;
    inventoryHover_ = nullptr;
    if (cb_.inventoryClosed) cb_.inventoryClosed();
  });
  inventory_ = &dlg;
}

void GameUi::openPause() {
  if (pause_) return;
  ui::Dialog& dlg = ui::Dialog::show(ui_, "Paused");
  dlg.setId("pause");
  dlg.addButton("Resume").setId("pause-resume");  // closes -> "close" -> pauseClosed
  dlg.addButton("Settings", [this] { if (cb_.openSettings) cb_.openSettings(); }, false).setId("pause-settings");
  dlg.addButton("Exit to menu", [this] { if (cb_.exitToMenu) cb_.exitToMenu(); }, false).setId("pause-exit-menu");
  dlg.addButton("Quit to desktop", [this] { if (cb_.quit) cb_.quit(); }).setId("pause-quit");
  dlg.on("close", [this](ui::Widget&) {
    pause_ = nullptr;
    if (cb_.pauseClosed) cb_.pauseClosed();
  });
  pause_ = &dlg;
}

void GameUi::openMenu(const std::string& version) {
  if (menu_) return;
  version_ = version;
  const float k = std::max(1.f, s());
  menuScale_ = k;
  ui::Widget& m = ui_.root().add<ui::Widget>();
  m.setId("menu");
  m.setLayout(ui::Layout::fill());
  auto& center = m.add<ui::StackPanel>(ui::Orientation::Vertical, 10 * k);
  center.setId("menu-center");
  center.setLayout(ui::Layout::at({0.5f, 0.44f}, {0.5f, 0.5f}));
  auto& title = center.add<ui::Label>("LITHOKRAFT", "title");
  title.setId("menu-title");
  title.setCentered(true);
  auto& tagline = center.add<ui::Label>("A LITHOLATTICE GAME");
  tagline.setId("menu-tagline");
  tagline.setCentered(true);
  tagline.setColor({0.55f, 0.65f, 0.8f, 1.f});
  center.add<ui::Divider>(ui::Orientation::Horizontal, 2.f * k).setId("menu-divider");
  auto& play = center.add<ui::Button>("Play");
  play.setId("menu-play");
  play.setLayout(ui::Layout::sized({600 * k, 52 * k}));
  play.on("click", [this](ui::Widget&) { if (cb_.play) cb_.play(); });
  auto& settings = center.add<ui::Button>("Settings");
  settings.setId("menu-settings");
  settings.setLayout(ui::Layout::sized({600 * k, 52 * k}));
  settings.on("click", [this](ui::Widget&) { if (cb_.openSettings) cb_.openSettings(); });
  auto& exit = center.add<ui::Button>("Exit");
  exit.setId("menu-exit");
  exit.setLayout(ui::Layout::sized({600 * k, 52 * k}));
  exit.on("click", [this](ui::Widget&) { if (cb_.quit) cb_.quit(); });
  auto& footerL = m.add<ui::Label>("v" + version_);
  footerL.setId("menu-version");
  footerL.setLayout(ui::Layout::at({0, 1}, {0, 1}, {12 * k, -12 * k}));
  auto& footerR = m.add<ui::Label>("singleplayer");
  footerR.setId("menu-mode");
  footerR.setLayout(ui::Layout::at({1, 1}, {1, 1}, {-12 * k, -12 * k}));
  menu_ = &m;
  ui_.setFocus(&play);
}

void GameUi::closeMenu() {
  if (!menu_) return;
  ui::Widget* m = menu_;
  menu_ = nullptr;
  ui_.destroyLater(ui_.root().removeChild(*m));
}

void GameUi::rebuildMenu() {
  closeMenu();
  openMenu(version_);
}

void GameUi::openSettings(const GameSettings& cur) {
  if (settings_) return;
  const float k = std::max(1.f, s());
  auto state = std::make_shared<GameSettings>(cur);
  ui::Dialog& dlg = ui::Dialog::show(ui_, "Settings");
  dlg.setId("settings");
  auto& tabs = dlg.body().add<ui::TabView>();
  tabs.setId("settings-tabs");

  // One vertical page per tab; each row is a fixed-width label plus the control.
  auto page = [&tabs](const char* name) -> ui::StackPanel& {
    auto p = std::make_unique<ui::StackPanel>(ui::Orientation::Vertical, 8.f);
    ui::StackPanel* raw = p.get();
    tabs.addTab(name, std::move(p));
    return *raw;
  };
  auto row = [k](ui::StackPanel& pg, const char* name) -> ui::StackPanel& {
    auto& r = pg.add<ui::StackPanel>(ui::Orientation::Horizontal, 8.f);
    r.setStretch(false);
    r.add<ui::Label>(name).setLayout(ui::Layout::sized({250 * k, 0}));
    return r;
  };
  auto changed = [this, state] {
    if (cb_.settingsChanged) cb_.settingsChanged(*state);
  };

  // ---- Audio: the engine's synth buses through the master gain.
  {
    ui::StackPanel& pg = page("Audio");
    ui::StackPanel& vr = row(pg, "Master volume");
    ui::Slider& vol = vr.add<ui::Slider>(0.f, 100.f, cur.masterVolume * 100.f, 1.f);
    vol.setId("set-volume");
    ui::Label& volText = vr.add<ui::Label>();
    volText.setId("set-volume-text");
    auto showVol = [&volText](float v) {
      char buf[16];
      std::snprintf(buf, sizeof buf, "%d%%", int(v + 0.5f));
      volText.setText(buf);
    };
    showVol(vol.value());
    vol.on("change", [changed, state, showVol, &volText](ui::Widget& w) {
      const float v = static_cast<ui::Slider&>(w).value();
      state->masterVolume = v / 100.f;
      showVol(v);
      changed();
    });
    ui::Checkbox& mute = pg.add<ui::Checkbox>("Mute audio", cur.muted);
    mute.setId("set-mute");
    mute.on("change", [changed, state](ui::Widget& w) {
      state->muted = static_cast<ui::Checkbox&>(w).checked();
      changed();
    });
  }

  // ---- Video: the window (display mode live, vsync at boot) and the UI scale.
  {
    ui::StackPanel& pg = page("Video");
    ui::StackPanel& dr = row(pg, "Display mode");
    const std::vector<std::string> dispItems{kSettingsDisplayModes, kSettingsDisplayModes + kSettingsDisplayModeCount};
    ui::Dropdown& disp = dr.add<ui::Dropdown>(dispItems, cur.displayMode);
    disp.setId("set-display");
    disp.on("change", [changed, state](ui::Widget& w) {
      state->displayMode = static_cast<ui::Dropdown&>(w).selected();
      changed();
    });
    ui::Checkbox& vsync = pg.add<ui::Checkbox>("VSync (applies on restart)", cur.vsync);
    vsync.setId("set-vsync");
    vsync.on("change", [changed, state](ui::Widget& w) {
      state->vsync = static_cast<ui::Checkbox&>(w).checked();
      changed();
    });
    ui::StackPanel& hr = row(pg, "HUD scale");
    const std::vector<std::string> hudItems{"Auto", "1x", "2x"};
    ui::Dropdown& hud = hr.add<ui::Dropdown>(hudItems, cur.hudScale);
    hud.setId("set-hud");
    hud.on("change", [changed, state](ui::Widget& w) {
      state->hudScale = static_cast<ui::Dropdown&>(w).selected();
      changed();
    });
    // PBR phase 2 (Worker 3): BinaryQuad/PackedVertex coexistence. Boot-only like the
    // tick radius - switching mesh formats needs a world (re)load.
    ui::StackPanel& mr = row(pg, "Mesher (restart)");
    const std::vector<std::string> mesherItems{kSettingsMesherNames, kSettingsMesherNames + kSettingsMesherCount};
    ui::Dropdown& mesher = mr.add<ui::Dropdown>(mesherItems, cur.mesher);
    mesher.setId("set-mesher");
    mesher.on("change", [changed, state](ui::Widget& w) {
      state->mesher = static_cast<ui::Dropdown&>(w).selected();
      changed();
    });
    // PBR phase 2 (Worker 4): the PBR transparency tier applies live via
    // setPbrTier on the active renderer - no restart needed, unlike the mesher
    // above. The tier ladder is independent of GraphicsTier by design (JD locked
    // all four tiers user-selectable); the hint only suggests a pairing.
    ui::StackPanel& pr = row(pg, "PBR transparency (live)");
    const std::vector<std::string> pbrItems{kSettingsPbrTierNames,
                                           kSettingsPbrTierNames + kSettingsPbrTierCount};
    ui::Dropdown& pbr = pr.add<ui::Dropdown>(pbrItems, cur.pbrTier);
    pbr.setId("set-pbrtier");
    pbr.on("change", [changed, state](ui::Widget& w) {
      state->pbrTier = static_cast<ui::Dropdown&>(w).selected();
      changed();
    });
    ui::Label& pbrHint =
        pg.add<ui::Label>("Pairs well with GraphicsTier High/Ultra; independent setting.");
    pbrHint.setId("pbr-tier-hint");
  }

  // ---- Sim: the tick rate and the EUV domain apply live; the tick radius at boot.
  {
    ui::StackPanel& pg = page("Sim");
    ui::StackPanel& tr = row(pg, "Tick rate (live)");
    std::vector<std::string> tpsItems;
    int tpsSel = 0;
    for (int i = 0; i < kSettingsTpsModeCount; ++i) {
      tpsItems.push_back(std::to_string(kSettingsTpsModes[i]));
      if (kSettingsTpsModes[i] == cur.tps) tpsSel = i;
    }
    ui::Dropdown& tps = tr.add<ui::Dropdown>(tpsItems, tpsSel);
    tps.setId("set-tps");
    tps.on("change", [changed, state](ui::Widget& w) {
      state->tps = kSettingsTpsModes[static_cast<ui::Dropdown&>(w).selected()];
      changed();
    });
    ui::StackPanel& rr = row(pg, "Tick radius, chunks (restart)");
    ui::NumberField& radius = rr.add<ui::NumberField>(0, 512, cur.tickRadius);
    radius.setId("set-radius");
    radius.on("change", [changed, state](ui::Widget& w) {
      state->tickRadius = static_cast<ui::NumberField&>(w).value();
      changed();
    });
    ui::Checkbox& euv = pg.add<ui::Checkbox>("EUV domain (live)", cur.euv);
    euv.setId("set-euv");
    euv.on("change", [changed, state](ui::Widget& w) {
      state->euv = static_cast<ui::Checkbox&>(w).checked();
      changed();
    });
  }

  dlg.addButton("Close").setId("settings-close");
  dlg.on("close", [this](ui::Widget&) { settings_ = nullptr; });
  settings_ = &dlg;
}

void GameUi::closeSettings() {
  if (!settings_) return;
  ui::Dialog* d = settings_;
  settings_ = nullptr;  // closeModal emits no "close": no callback for a close the game asked for
  ui_.closeModal(*d);
}

void GameUi::closePause() {
  if (!pause_) return;
  ui::Dialog* d = pause_;
  pause_ = nullptr;
  ui_.closeModal(*d);
}

void GameUi::closeInventory() {
  if (!inventory_) return;
  ui::Dialog* d = inventory_;
  inventory_ = nullptr;  // close() emits "close": no callback for a close the game asked for
  inventoryHover_ = nullptr;
  ui_.closeModal(*d);
}

void GameUi::update(const HudModel& m, ui::Vec2 viewport, float dt) {
  const float k = hudScalePref_ > 0 ? float(hudScalePref_) : (viewport.y >= 900 ? 2.f : 1.f);
  if (k != scale_) {
    scale_ = k;
    ui::Theme t = ui_.theme();
    t.setScale(k);
    ui_.setTheme(std::move(t));
    rebuildHud();
  }
  if (menu_ && k != menuScale_) rebuildMenu();
  hud_->setVisible(m.visible);
  age_ += dt;
  splash_->setVisible(age_ < kSplashSeconds);
  debugPanel_->setVisible(!m.debugLines.empty());
  debugText_->setText(joinLines(m.debugLines));
  controlsText_->setText(joinLines(m.controlLines));
  banner_->setVisible(!m.bannerLines.empty());
  bannerTitle_->setText(m.bannerLines.empty() ? std::string() : m.bannerLines.front());
  bannerText_->setText(m.bannerLines.size() > 1 ? m.bannerLines[1] : std::string());
  for (int i = 0; i < kHotbarSlots; ++i) {
    ui::Slot& slot = *slots_[size_t(i)];
    const ItemStack& st = m.hotbar[size_t(i)];
    // Real stacks (P2b-1): block-backed stacks render as blocks, tools and
    // the Fragment render with their own item icons -- the Fragment in
    // slot 0 is finally visible in the HUD.
    slot.setIcon(itemIconRef(st.itemId));
    slot.setSelected(i == m.selected);
  }
  {
    const ItemStack& sel = m.hotbar[size_t(std::clamp(m.selected, 0, kHotbarSlots - 1))];
    std::string name = itemDisplayName(sel.itemId);
    if (!sel.empty() && sel.count > 1) name += " x" + std::to_string(sel.count);
    slotName_->setText(name);
  }
  if (inventoryHover_) {
    const auto* over = dynamic_cast<const ui::Slot*>(ui_.hover());
    const bool inInventory = over && ui_.topModal() == inventory_;
    inventoryHover_->setText(inInventory ? blockDescription(BlockId(over->data())) : std::string(" "));
  }
  ui_.update(dt);
  ui_.layout(viewport);
}

void GameUi::paint(HudLayer& hud, const BlockMaterials& materials, const std::function<const Image*(BlockId)>& icon,
                   const std::function<const Image*(uint32_t)>& itemIcon) const {
  HudUiRenderer renderer(hud, [&materials](std::string_view image) -> std::optional<ui::Color> {
    constexpr std::string_view kPrefix = "block:";
    if (image.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
    int id = 0;
    const std::string_view num = image.substr(kPrefix.size());
    if (std::from_chars(num.data(), num.data() + num.size(), id).ec != std::errc() || id <= 0 ||
        id >= int(blockRegistry().size()))
      return std::nullopt;
    const auto& c = materials.swatch(BlockId(id));
    return ui::Color{c[0], c[1], c[2], 1.f};
  }, [&icon, &itemIcon](std::string_view image) -> const Image* {
    constexpr std::string_view kItemPrefix = "item:";
    if (image.substr(0, kItemPrefix.size()) == kItemPrefix) {
      // Non-block item icon (a tool, the Fragment): the item icon atlas.
      if (!itemIcon) return nullptr;
      int id = 0;
      const std::string_view num = image.substr(kItemPrefix.size());
      if (std::from_chars(num.data(), num.data() + num.size(), id).ec != std::errc() || id <= 0)
        return nullptr;
      return itemIcon(uint32_t(id));
    }
    constexpr std::string_view kPrefix = "block:";
    if (!icon || image.substr(0, kPrefix.size()) != kPrefix) return nullptr;
    int id = 0;
    const std::string_view num = image.substr(kPrefix.size());
    if (std::from_chars(num.data(), num.data() + num.size(), id).ec != std::errc() || id <= 0 || id >= int(blockRegistry().size()))
      return nullptr;
    return icon(BlockId(id));  // textured icon; the swatch above is the fallback
  });
  ui_.paint(renderer);
}

}  // namespace lithokraft
