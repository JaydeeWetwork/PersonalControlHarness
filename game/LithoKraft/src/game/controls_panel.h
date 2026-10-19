// Private to lithokraft_game: the on-screen controls panel, generated from the live bindings.
#pragma once
#include "lithokraft/game/engine.h"
#include <string>
#include <vector>
#include "litholattice/input/action_map.h"
#include "lithokraft/game/actions.h"

namespace lithokraft {

/// Short label for a binding as shown on the HUD ("W", "LMB", "RT", "LS").
std::string bindingLabel(const Binding& b);
/// Panel lines: one row per control group with its keyboard/mouse and gamepad inputs.
std::vector<std::string> controlsPanelLines(const ActionMap& map);
/// "H" (or whatever ToggleControls is bound to) for the collapsed panel hint.
std::string controlsToggleLabel(const ActionMap& map);

}  // namespace lithokraft
