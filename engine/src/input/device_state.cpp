#include "litholattice/input/device_state.h"

namespace litholattice {

template <size_t N>
void DeviceState::Buttons<N>::set(size_t i, bool on) {
  if (i >= N) return;
  if (on && !down[i]) pressed.set(i);
  if (!on && down[i]) released.set(i);
  down.set(i, on);
}

void DeviceState::beginFrame() {
  keys_.pressed.reset(); keys_.released.reset();
  mouse_.pressed.reset(); mouse_.released.reset();
  for (Pad& p : pads_) { p.pressed.reset(); p.released.reset(); }
  mouseDx_ = mouseDy_ = wheel_ = 0.f;
  text_ = {};
}

void DeviceState::releaseAll() {
  keys_.releaseAll();
  mouse_.releaseAll();
  for (Pad& p : pads_) { p.released |= p.down; p.down.reset(); p.axes.fill(0.f); }
}

int DeviceState::connectedPads() const {
  int n = 0;
  for (const Pad& p : pads_) n += p.connected;
  return n;
}

void DeviceState::apply(const InputEvent& e) {
  using T = InputEvent::Type;
  Pad* pad = e.device < kMaxGamepads ? &pads_[e.device] : nullptr;
  switch (e.type) {
    case T::KeyDown:
    case T::KeyRepeat:
      if (e.type == T::KeyDown) keys_.set(e.code, true);
      // Alt+Enter is the window's display toggle, not a text Enter.
      if (Key(e.code) == Key::Enter && !keyDown(Key::LeftAlt) && !keyDown(Key::RightAlt))
        ++text_.enter;
      if (Key(e.code) == Key::Backspace) ++text_.backspace;
      break;
    case T::KeyUp: keys_.set(e.code, false); break;
    case T::MouseDown: mouse_.set(e.code, true); break;
    case T::MouseUp: mouse_.set(e.code, false); break;
    case T::MouseMove: mouseDx_ += e.x; mouseDy_ += e.y; break;
    case T::MouseWheel: wheel_ += e.y; break;
    case T::CursorPos: cursorX_ = e.x; cursorY_ = e.y; break;
    case T::PadButtonDown:
    case T::PadButtonUp:
      if (pad && e.code < size_t(GamepadButton::Count)) {
        const bool on = e.type == T::PadButtonDown;
        if (on && !pad->down[e.code]) pad->pressed.set(e.code);
        if (!on && pad->down[e.code]) pad->released.set(e.code);
        pad->down.set(e.code, on);
      }
      break;
    case T::PadAxis:
      if (pad && e.code < size_t(GamepadAxis::Count)) pad->axes[e.code] = e.x;
      break;
    case T::PadConnected: if (pad) pad->connected = true; break;
    case T::PadDisconnected:  // nothing stays held on a pad that went away
      if (pad) { pad->released |= pad->down; pad->down.reset(); pad->axes.fill(0.f); pad->connected = false; }
      break;
    case T::Text: text_.chars.push_back(char32_t(e.code)); break;
    case T::FocusLost:
    case T::FocusGained: releaseAll(); break;
  }
}

}  // namespace litholattice
