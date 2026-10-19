#include "litholattice/input/input_system.h"
#include <algorithm>
#include <cmath>

namespace litholattice {

const TextInput InputSystem::kNoText{};
const InputSystem::ActionState InputSystem::kNoAction{};

void ReplayInputBackend::poll(std::vector<InputEvent>& out) {
  while (next_ < events_.size() && events_[next_].time <= now_) out.push_back(events_[next_++]);
}

InputSystem::InputSystem(ActionMap map, std::vector<ContextRules> contexts)
    : map_(std::move(map)), contexts_(std::move(contexts)) {
  active_.assign(contexts_.size(), 0);
  pending_.assign(contexts_.size(), 0);
  for (size_t c = 0; c < contexts_.size(); ++c) pending_[c] = contexts_[c].activeAtStart;
  setMap(std::move(map_));
  applyContexts();
}

void InputSystem::setMap(ActionMap m) {
  map_ = std::move(m);
  const size_t n = map_.actions().size();
  enabled_.resize(n, 0);
  rawHeld_.resize(n, 0);
  out_.resize(n);
}

void InputSystem::update(IInputBackend& backend) {
  frame_.clear();
  backend.poll(frame_);
  {  // Model control (MCP player_control): synthetic events merge here,
     // the exact point keyboard input enters. Never in processFrame:
     // replay/test streams stay pure and deterministic.
    std::lock_guard<std::mutex> lk(syntheticMutex_);
    frame_.insert(frame_.end(), synthetic_.begin(), synthetic_.end());
    synthetic_.clear();
  }
  processFrame(frame_);
}

void InputSystem::stageSyntheticEvents(std::vector<InputEvent> events) {
  std::lock_guard<std::mutex> lk(syntheticMutex_);
  synthetic_.insert(synthetic_.end(), events.begin(), events.end());
}

void InputSystem::processFrame(std::span<const InputEvent> events) {
  applyContexts();
  devices_.beginFrame();
  for (const InputEvent& e : events) devices_.apply(e);
  lastFrame_.assign(events.begin(), events.end());
  if (recording_) recorded_.insert(recorded_.end(), events.begin(), events.end());
  for (size_t i = 0; i < out_.size(); ++i) {
    const ActionId a = ActionId(i);
    if (map_.actions().kind(a) != ActionKind::Button) continue;
    bool raw = false;
    ActionState s = resolveButton(a, raw);
    rawHeld_[i] = raw;
    out_[i] = enabled_[i] ? s : ActionState{};
  }
}

// Snapshot the context stack for this frame: each action belongs to the highest active context
// listing it, unless an opaque context above hides it. Text goes to the highest text context.
void InputSystem::applyContexts() {
  active_ = pending_;
  std::fill(enabled_.begin(), enabled_.end(), uint8_t(0));
  std::vector<uint8_t> decided(enabled_.size(), 0);
  textEnabled_ = false;
  bool textDecided = false;
  for (int c = int(contexts_.size()) - 1; c >= 0; --c) {
    if (!active_[size_t(c)]) continue;
    const ContextRules& r = contexts_[size_t(c)];
    for (ActionId a : r.actions)
      if (a < decided.size() && !decided[a]) { decided[a] = 1; enabled_[a] = 1; }
    if (!textDecided && r.wantsText) { textEnabled_ = true; textDecided = true; }
    if (r.opaque) break;
  }
}

float InputSystem::bindingValue(const Binding& b) const {
  const AnalogSettings& s = map_.analog();
  using S = Binding::Source;
  switch (b.source) {
    case S::Key: return devices_.keyDown(Key(b.code)) ? 1.f : 0.f;
    case S::MouseButton: return devices_.mouseDown(MouseButton(b.code)) ? 1.f : 0.f;
    case S::PadButton: {
      float v = 0.f;
      for (int p = 0; p < kMaxGamepads; ++p) v = std::max(v, devices_.pad(p).down[b.code] ? 1.f : 0.f);
      return v;
    }
    case S::PadAxisPositive:
    case S::PadAxisNegative: {
      const auto axis = GamepadAxis(b.code);
      const float sign = b.source == S::PadAxisPositive ? 1.f : -1.f;
      float v = 0.f;
      for (int p = 0; p < kMaxGamepads; ++p) {
        const DeviceState::Pad& pad = devices_.pad(p);
        if (!pad.connected) continue;
        float a;
        if (axis == GamepadAxis::LeftTrigger || axis == GamepadAxis::RightTrigger) {
          a = applyDeadzone(pad.axes[b.code], s.triggerDeadzone, 1.f);
        } else {  // stick axis: radial deadzone on the whole stick, then take this component
          const bool left = axis == GamepadAxis::LeftX || axis == GamepadAxis::LeftY;
          const size_t ix = left ? size_t(GamepadAxis::LeftX) : size_t(GamepadAxis::RightX);
          const AxisValue st = applyRadialDeadzone({pad.axes[ix], pad.axes[ix + 1]}, s.stickDeadzone, s.stickCurve);
          a = (axis == GamepadAxis::LeftX || axis == GamepadAxis::RightX) ? st.x : st.y;
        }
        v = std::max(v, std::max(0.f, a * sign));
      }
      return v;
    }
    case S::WheelUp:
    case S::WheelDown:
    case S::MouseMotion:
    case S::PadStick: return 0.f;  // pulses / axes, handled separately
  }
  return 0.f;
}

InputSystem::ActionState InputSystem::resolveButton(ActionId a, bool& rawHeld) const {
  const float threshold = map_.analog().pressThreshold;
  ActionState s;
  bool edgeDown = false, edgeUp = false, pulse = false;
  for (const Binding& b : map_.bindings(a)) {
    using S = Binding::Source;
    s.value = std::max(s.value, bindingValue(b));
    switch (b.source) {  // edges that can start and end within one frame
      case S::Key: edgeDown |= devices_.keyPressed(Key(b.code)); edgeUp |= devices_.keyReleased(Key(b.code)); break;
      case S::MouseButton:
        edgeDown |= devices_.mousePressed(MouseButton(b.code));
        edgeUp |= devices_.mouseReleased(MouseButton(b.code));
        break;
      case S::PadButton:
        for (int p = 0; p < kMaxGamepads; ++p) {
          edgeDown |= devices_.pad(p).pressed[b.code];
          edgeUp |= devices_.pad(p).released[b.code];
        }
        break;
      case S::WheelUp: pulse |= devices_.wheel() > 0.f; break;
      case S::WheelDown: pulse |= devices_.wheel() < 0.f; break;
      default: break;
    }
  }
  const bool wasHeld = rawHeld_[a] != 0;
  rawHeld = s.value >= threshold;
  s.held = rawHeld;
  s.pressed = (rawHeld && !wasHeld) || edgeDown || pulse;
  s.released = (!rawHeld && wasHeld) || (edgeUp && !rawHeld);
  if (pulse) s.value = std::max(s.value, 1.f);
  return s;
}

AxisValue InputSystem::axisOf(ActionId a, float dt) const {
  if (a >= enabled_.size() || !enabled_[a]) return {};
  const AnalogSettings& s = map_.analog();
  AxisValue v;
  for (const Binding& b : map_.bindings(a)) {
    if (b.source == Binding::Source::MouseMotion) {
      v.x += devices_.mouseDx() * s.mouseSensitivity;
      v.y += devices_.mouseDy() * s.mouseSensitivity;
    } else if (b.source == Binding::Source::PadStick) {
      const size_t ix = Stick(b.code) == Stick::Left ? size_t(GamepadAxis::LeftX) : size_t(GamepadAxis::RightX);
      for (int p = 0; p < kMaxGamepads; ++p) {
        const DeviceState::Pad& pad = devices_.pad(p);
        if (!pad.connected) continue;
        const AxisValue st = applyRadialDeadzone({pad.axes[ix], pad.axes[ix + 1]}, s.stickDeadzone, s.stickCurve);
        v.x += st.x * s.stickLookSpeed * dt;
        v.y += st.y * s.stickLookSpeed * dt;
      }
    }
  }
  if (s.invertY) v.y = -v.y;
  return v;
}

}  // namespace litholattice
