// LithoKraft's input vocabulary on the engine input SDK: its actions (ActionSet), default
// bindings and input contexts.
#pragma once
#include <cstdint>
#include <string_view>
#include <vector>
#include "lithokraft/game/engine.h"
#include "litholattice/input/action_map.h"
#include "litholattice/input/input_context.h"

namespace lithokraft {

#define LITHOKRAFT_INPUT_ACTIONS(X)                                                                           \
  X(MoveForward) X(MoveBack) X(MoveLeft) X(MoveRight) X(Jump) X(Sneak) X(Crawl) X(Sprint)                      \
  X(Break) X(Place) X(Use) X(PickBlock) X(Inventory) X(Drop)                                          \
  X(Hotbar1) X(Hotbar2) X(Hotbar3) X(Hotbar4) X(Hotbar5) X(Hotbar6) X(Hotbar7) X(Hotbar8) X(Hotbar9) \
  X(HotbarNext) X(HotbarPrev) X(ToggleHUD) X(ToggleDebug) X(ToggleControls) X(CycleTPS) X(SwitchDim) \
  X(ToggleEUV) X(ReloadResources) X(ScriptEditor) X(SaveWorld) X(Pause) X(Look)

#define LITHOKRAFT_INPUT_ENUMERATOR(n) n,
/// Jump / Sneak double as fly up / down in Fly locomotion mode. Crawl asks the
/// locomotion controller for the crawl pose (1-block spaces force it anyway).
/// Look is the only axis action.
enum class Action : ActionId { LITHOKRAFT_INPUT_ACTIONS(LITHOKRAFT_INPUT_ENUMERATOR) Count };
#undef LITHOKRAFT_INPUT_ENUMERATOR

constexpr size_t kActionCount = size_t(Action::Count);

/// Hotbar1 + i.
constexpr Action hotbarAction(int i) { return Action(int(Action::Hotbar1) + i); }

/// The game's ActionSet (names as in config/input.json; Place and Use share right click).
const ActionSet& actionSet();
std::string_view actionName(Action a);

/// Minecraft Java creative keyboard/mouse defaults plus console-style gamepad defaults.
ActionMap defaultActionMap();

/// In priority order (higher = consulted first).
enum class InputContext : uint8_t { Gameplay, Inventory, KeyboardTyping, UiText, Debug, Count };
/// The rules of every InputContext, in enum order (for InputSystem).
std::vector<ContextRules> inputContexts();

}  // namespace lithokraft
