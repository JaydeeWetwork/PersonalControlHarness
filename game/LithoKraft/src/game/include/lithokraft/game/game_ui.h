// The game's screens on the retained UI: HUD (debug panel, controls panel, keyboard-capture
// banner), the hotbar and the block palette (a modal dialog). The game hands in a
// HudModel each frame and reacts to callbacks; GameUi owns the widgets and the UiRoot.
#pragma once
#include "lithokraft/game/engine.h"
#include <array>
#include <functional>
#include <string>
#include <vector>
#include "litholattice/render/block_materials.h"
#include "litholattice/render/renderer.h"
#include "litholattice/ui/controls.h"
#include "litholattice/ui/dialog.h"
#include "litholattice/ui/ui_root.h"
#include "litholattice/world/block.h"
#include "lithokraft/game/inventory.h"  // HudModel hotbar = the inventory's real ItemStacks (P2b-1)
#include "lithokraft/game/settings.h"

namespace lithokraft {

constexpr int kHotbarSlots = 8;

/// What the HUD shows this frame. `hotbar` is a snapshot of the player
/// inventory's hotbar slots (P2b-1): stacks, not bare BlockIds — a tool or
/// the Fragment in a slot renders with its own item icon and name.
struct HudModel {
  std::array<ItemStack, kHotbarSlots> hotbar{};
  int selected = 0;
  bool visible = true;                     ///< F1 hides everything
  std::vector<std::string> debugLines;     ///< empty: no debug panel (F3)
  std::vector<std::string> controlLines;   ///< controls panel (H)
  std::vector<std::string> bannerLines;    ///< keyboard capture banner (empty: none)
};

class GameUi {
 public:
  struct Callbacks {
    std::function<void(int slot)> selectSlot;              ///< hotbar slot clicked
    std::function<void(BlockId, bool shift)> pickBlock;    ///< inventory cell clicked
    std::function<void()> inventoryClosed;                 ///< the dialog closed itself (Esc / E)
    std::function<void()> pauseClosed;                     ///< pause menu closed (Resume / Esc)
    std::function<void()> quit;                            ///< Quit to desktop
    std::function<void()> play;                            ///< menu Play button: enter creative/testing
    std::function<void()> exitToMenu;                      ///< pause menu: back to the launch menu
    std::function<void()> openSettings;                    ///< menu / pause: open the settings screen
    std::function<void(const GameSettings&)> settingsChanged;  ///< a settings control changed
  };

  explicit GameUi(Callbacks callbacks);
  ui::UiRoot& root() { return ui_; }
  const ui::UiRoot& root() const { return ui_; }

  /// Re-applies a theme (after a resource reload); the HUD scale stays.
  void applyTheme(const ui::Theme& theme);
  void openInventory(int selectedSlot);
  void closeInventory();
  bool inventoryOpen() const { return inventory_ != nullptr; }
  void openPause();
  void closePause();
  bool pauseOpen() const { return pause_ != nullptr; }
  /// The launch menu overlay (title, tagline, divider, Play / Settings / Exit, version
  /// footer) over the cinematic wafer backdrop. `version` is shown in the footer.
  void openMenu(const std::string& version);
  void closeMenu();
  bool menuOpen() const { return menu_ != nullptr; }
  /// The settings screen (a modal over the menu or the pause dialog).
  void openSettings(const GameSettings& current);
  void closeSettings();
  bool settingsOpen() const { return settings_ != nullptr; }
  /// 0 = auto (from the window height), else a fixed HUD scale (1 or 2).
  void setHudScalePref(int pref) { hudScalePref_ = pref < 0 ? 0 : pref > 2 ? 2 : pref; }

  /// Syncs widgets with `model`, lays out for `viewport` and advances UI time.
  void update(const HudModel& model, ui::Vec2 viewport, float dt);
  /// Paints into `hud.draws`; block icons ("block:<id>") use the material
  /// swatches / block icons, item icons ("item:<itemId>") use `itemIcon`
  /// (the ResourceManager's per-item icon atlas, P2b-1).
  void paint(HudLayer& hud, const BlockMaterials& materials, const std::function<const Image*(BlockId)>& icon = {},
             const std::function<const Image*(uint32_t)>& itemIcon = {}) const;

 private:
  void rebuildHud();
  float s() const { return scale_; }

  Callbacks cb_;
  ui::UiRoot ui_;
  float scale_ = 0;  // HUD scale the widgets were built for (0 = not built)
  ui::Widget* hud_ = nullptr;
  ui::Panel* debugPanel_ = nullptr;
  ui::Label* debugText_ = nullptr;
  ui::Panel* controlsPanel_ = nullptr;
  ui::Label* controlsText_ = nullptr;
  ui::Panel* banner_ = nullptr;
  ui::Label* bannerTitle_ = nullptr;
  ui::Label* bannerText_ = nullptr;
  std::array<ui::Slot*, kHotbarSlots> slots_{};
  ui::Label* slotName_ = nullptr;
  ui::Label* splash_ = nullptr;  // "Powered by LithoLattice", bottom right for the first seconds
  float age_ = 0;
  ui::Dialog* inventory_ = nullptr;
  ui::Dialog* pause_ = nullptr;
  ui::Label* inventoryHover_ = nullptr;
  ui::Widget* menu_ = nullptr;
  float menuScale_ = 0;  // UI scale the menu was built for (0 = not built)
  std::string version_;
  ui::Dialog* settings_ = nullptr;
  int hudScalePref_ = 0;  // 0 = auto

  void rebuildMenu();
};

/// "block:<id>" for a block icon.
std::string blockIcon(BlockId b);

}  // namespace lithokraft
