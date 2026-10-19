// Translates the frame's raw input events into ui::UiEvents (the ui module does not know the
// input module). Key repeat becomes repeated KeyDowns, so held arrows / Backspace work in text.
#pragma once
#include "lithokraft/game/engine.h"
#include <span>
#include <vector>
#include "litholattice/input/device_state.h"
#include "litholattice/input/input_event.h"
#include "litholattice/ui/events.h"

namespace lithokraft {

/// Appends the UI events for `events` to `out`. Pointer positions come from CursorPos events
/// (and `devices` for button / wheel events); modifiers from the keys held in `devices`.
void translateUiEvents(std::span<const InputEvent> events, const DeviceState& devices, std::vector<ui::UiEvent>& out);

}  // namespace lithokraft
