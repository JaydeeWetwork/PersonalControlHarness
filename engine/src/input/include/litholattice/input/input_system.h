// InputSystem: the one place the game asks about input. Each frame (main thread):
//   backend.poll() -> InputEvent queue -> DeviceState -> bindings -> actions -> context gating
// Context pushes / pops take effect at the next frame, so the key that opens a mode is never
// seen again by the mode it opened.
#pragma once
#include <array>
#include <bitset>
#include <span>
#include <mutex>
#include <vector>
#include "litholattice/input/action_map.h"
#include "litholattice/input/device_state.h"
#include "litholattice/input/input_backend.h"
#include "litholattice/input/input_context.h"

namespace litholattice {

class InputSystem {
 public:
  /// `contexts`: the game's input contexts in priority order (see input_context.h).
  InputSystem(ActionMap map, std::vector<ContextRules> contexts);

  /// Once per frame: polls `backend` and processes its events.
  void update(IInputBackend& backend);
  /// Processes one frame of already-collected events (replay, tests).
  void processFrame(std::span<const InputEvent> events);
  /// Stage synthetic input events (model control via MCP player_control).
  /// Thread-safe: may be called from the MCP server thread. Staged
  /// events merge with the backend's events at the next update() —
  /// the exact point keyboard input enters the system — and are then
  /// cleared. At most one staged batch is consumed per frame, so the
  /// effective synthetic input rate can never exceed the frame rate.
  void stageSyntheticEvents(std::vector<InputEvent> events);

  // ---- contexts (those marked activeAtStart are active from the start)
  template <InputId C>
  void push(C c) { if (size_t(c) < pending_.size()) pending_[size_t(c)] = 1; }
  template <InputId C>
  void pop(C c) { if (size_t(c) < pending_.size()) pending_[size_t(c)] = 0; }
  /// True if `c` is active this frame (pushes apply at the next frame).
  template <InputId C>
  bool active(C c) const { return size_t(c) < active_.size() && active_[size_t(c)]; }

  // ---- actions this frame (after context gating)
  template <InputId A>
  bool pressed(A a) const { return state(actionId(a)).pressed; }
  template <InputId A>
  bool held(A a) const { return state(actionId(a)).held; }
  template <InputId A>
  bool released(A a) const { return state(actionId(a)).released; }
  /// Button actions: 0..1 (analog sources keep their deflection). 0 when gated.
  template <InputId A>
  float value(A a) const { return state(actionId(a)).value; }
  /// Axis actions: radians to turn this frame (mouse motion + stick speed x `dt`).
  template <InputId A>
  AxisValue axis(A a, float dt) const { return axisOf(actionId(a), dt); }

  /// Text typed this frame, delivered only while a text-wanting context is on top.
  const TextInput& text() const { return textEnabled_ ? devices_.text() : kNoText; }
  /// Cursor in framebuffer pixels (for UI clicks).
  float cursorX() const { return devices_.cursorX(); }
  float cursorY() const { return devices_.cursorY(); }
  const DeviceState& devices() const { return devices_; }
  /// This frame's raw events in order (the UI translates them into its own events).
  std::span<const InputEvent> frameEvents() const { return lastFrame_; }

  const ActionMap& map() const { return map_; }
  void setMap(ActionMap m);

  // ---- recording (for replay / bug reports)
  void startRecording() { recording_ = true; recorded_.clear(); }
  std::vector<InputEvent> stopRecording() { recording_ = false; return std::move(recorded_); }

 private:
  struct ActionState {
    bool pressed = false, held = false, released = false;
    float value = 0.f;
  };
  const ActionState& state(ActionId a) const { return a < out_.size() ? out_[a] : kNoAction; }
  AxisValue axisOf(ActionId a, float dt) const;
  void applyContexts();
  ActionState resolveButton(ActionId a, bool& rawHeld) const;
  float bindingValue(const Binding& b) const;

  static const TextInput kNoText;
  static const ActionState kNoAction;
  ActionMap map_;
  DeviceState devices_;
  std::vector<ContextRules> contexts_;
  std::vector<uint8_t> active_, pending_;    // per context
  std::vector<uint8_t> enabled_, rawHeld_;   // per action
  std::vector<ActionState> out_;
  bool textEnabled_ = false;
  std::vector<InputEvent> frame_, lastFrame_, recorded_;
  bool recording_ = false;
  mutable std::mutex syntheticMutex_;
  std::vector<InputEvent> synthetic_;  ///< staged by stageSyntheticEvents
};

}  // namespace litholattice
