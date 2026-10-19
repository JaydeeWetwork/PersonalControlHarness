#include "lithokraft/game/actions.h"
#include <span>

namespace lithokraft {
namespace {

#define LITHOKRAFT_INPUT_INFO(n) ActionInfo{#n, Action::n == Action::Look ? ActionKind::Axis2D : ActionKind::Button},
const ActionInfo kActionInfo[] = {LITHOKRAFT_INPUT_ACTIONS(LITHOKRAFT_INPUT_INFO)};
#undef LITHOKRAFT_INPUT_INFO
static_assert(std::size(kActionInfo) == kActionCount);

using A = Action;
constexpr A kGameplay[] = {A::MoveForward, A::MoveBack, A::MoveLeft, A::MoveRight, A::Jump, A::Sneak, A::Crawl, A::Sprint,
                           A::Break, A::Place, A::Use, A::PickBlock, A::Inventory, A::Drop, A::Hotbar1, A::Hotbar2,
                           A::Hotbar3, A::Hotbar4, A::Hotbar5, A::Hotbar6, A::Hotbar7, A::Hotbar8, A::Hotbar9,
                           A::HotbarNext, A::HotbarPrev, A::ToggleControls, A::Pause, A::Look};
// Inventory (a UI screen: clicks and keys reach it as UI events): Inventory / Pause close it.
constexpr A kInventory[] = {A::Inventory, A::Pause};
// UI text entry (script editor, focused text fields): every key is for the UI.
constexpr A kUiText[] = {A::Look};
// Typing: letters go to the text channel; only Pause (end typing) and looking around remain.
constexpr A kTyping[] = {A::Pause, A::Look};
// Debug keys (function keys / pad Back, which never produce text) work in every mode.
constexpr A kDebug[] = {A::ToggleHUD, A::ToggleDebug, A::CycleTPS, A::SwitchDim, A::ToggleEUV, A::ReloadResources, A::SaveWorld,
                         A::ScriptEditor};

ContextRules rules(const char* name, bool opaque, bool wantsText, bool atStart, std::span<const Action> actions) {
  ContextRules r{name, opaque, wantsText, atStart, {}};
  for (Action a : actions) r.actions.push_back(actionId(a));
  return r;
}

}  // namespace

const ActionSet& actionSet() {
  // Right click: use a usable block, else place (an intended shared input).
  static const ActionSet set({std::begin(kActionInfo), std::end(kActionInfo)},
                             {{actionId(Action::Place), actionId(Action::Use)}});
  return set;
}

std::string_view actionName(Action a) { return actionSet().name(actionId(a)); }

ActionMap defaultActionMap() {
  using A = Action;
  using B = Binding;
  using GB = GamepadButton;
  using GA = GamepadAxis;
  ActionMap m(actionSet());
  auto bind = [&m](Action a, std::initializer_list<Binding> b) { m.setBindings(a, b); };
  bind(A::MoveForward, {B::key(Key::W), B::padAxis(GA::LeftY, false)});  // stick up = -Y
  bind(A::MoveBack, {B::key(Key::S), B::padAxis(GA::LeftY, true)});
  bind(A::MoveLeft, {B::key(Key::A), B::padAxis(GA::LeftX, false)});
  bind(A::MoveRight, {B::key(Key::D), B::padAxis(GA::LeftX, true)});
  bind(A::Jump, {B::key(Key::Space), B::pad(GB::A)});
  bind(A::Sneak, {B::key(Key::LeftShift), B::pad(GB::B)});
  bind(A::Crawl, {B::key(Key::C), B::pad(GB::DpadRight)});
  bind(A::Sprint, {B::key(Key::LeftCtrl), B::pad(GB::LeftThumb)});
  bind(A::Break, {B::mouse(MouseButton::Left), B::padAxis(GA::RightTrigger, true)});
  bind(A::Place, {B::mouse(MouseButton::Right), B::padAxis(GA::LeftTrigger, true)});
  bind(A::Use, {B::mouse(MouseButton::Right), B::pad(GB::X)});
  bind(A::PickBlock, {B::mouse(MouseButton::Middle), B::pad(GB::DpadUp)});
  bind(A::Inventory, {B::key(Key::E), B::pad(GB::Y)});
  bind(A::Drop, {B::key(Key::Q), B::pad(GB::DpadDown)});
  for (int i = 0; i < 9; ++i) bind(hotbarAction(i), {B::key(Key(int(Key::Num1) + i))});
  bind(A::HotbarNext, {B::wheelDown(), B::pad(GB::RightBumper)});
  bind(A::HotbarPrev, {B::wheelUp(), B::pad(GB::LeftBumper)});
  bind(A::ToggleHUD, {B::key(Key::F1)});
  bind(A::ToggleDebug, {B::key(Key::F3), B::pad(GB::Back)});
  bind(A::ToggleControls, {B::key(Key::H), B::pad(GB::DpadLeft)});
  bind(A::CycleTPS, {B::key(Key::F7)});
  bind(A::SwitchDim, {B::key(Key::F6)});
  bind(A::ToggleEUV, {B::key(Key::F8)});
  bind(A::SaveWorld, {B::key(Key::F5)});
  bind(A::ReloadResources, {B::key(Key::F9)});
  bind(A::ScriptEditor, {B::key(Key::F10)});
  bind(A::Pause, {B::key(Key::Escape), B::pad(GB::Start)});
  bind(A::Look, {B::mouseMotion(), B::stick(Stick::Right)});
  return m;
}

std::vector<ContextRules> inputContexts() {
  // Gameplay and Debug are active from the start; the others are pushed by game modes.
  return {rules("Gameplay", false, false, true, kGameplay), rules("Inventory", true, false, false, kInventory),
          rules("KeyboardTyping", true, true, false, kTyping), rules("UiText", true, true, false, kUiText),
          rules("Debug", false, false, true, kDebug)};
}

}  // namespace lithokraft
