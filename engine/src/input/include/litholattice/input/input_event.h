// Raw input as timestamped events. Backends produce them; InputSystem consumes them once per
// frame. A recorded event stream replays deterministically (ReplayInputBackend).
#pragma once
#include <cstdint>
#include "litholattice/input/input_types.h"

namespace litholattice {

struct InputEvent {
  enum class Type : uint8_t {
    KeyDown, KeyUp, KeyRepeat,   ///< code = Key (repeat only feeds text editing, never actions)
    MouseDown, MouseUp,          ///< code = MouseButton
    MouseMove,                   ///< x, y = motion in pixels since the previous event
    MouseWheel,                  ///< y = wheel steps (+ = up / away from the user)
    CursorPos,                   ///< x, y = cursor in framebuffer pixels (top-left origin)
    PadButtonDown, PadButtonUp,  ///< device = gamepad slot, code = GamepadButton
    PadAxis,                     ///< device, code = GamepadAxis, x = raw value
    PadConnected, PadDisconnected,
    Text,                        ///< code = Unicode code point (from the OS text / char callback)
    FocusLost,                   ///< window lost focus: everything held is released
    FocusGained,                 ///< window got focus back: anything held meanwhile is stale, released too
  };

  Type type = Type::KeyDown;
  double time = 0.0;  ///< seconds on the backend's clock
  uint32_t code = 0;
  uint8_t device = 0;
  float x = 0.f, y = 0.f;

  static InputEvent key(Type t, Key k, double time) { return {t, time, uint32_t(k), 0, 0.f, 0.f}; }
  static InputEvent mouse(Type t, MouseButton b, double time) { return {t, time, uint32_t(b), 0, 0.f, 0.f}; }
  static InputEvent motion(float dx, float dy, double time) { return {Type::MouseMove, time, 0, 0, dx, dy}; }
  static InputEvent wheel(float steps, double time) { return {Type::MouseWheel, time, 0, 0, 0.f, steps}; }
  static InputEvent cursor(float px, float py, double time) { return {Type::CursorPos, time, 0, 0, px, py}; }
  static InputEvent padButton(Type t, int pad, GamepadButton b, double time) { return {t, time, uint32_t(b), uint8_t(pad), 0.f, 0.f}; }
  static InputEvent padAxis(int pad, GamepadAxis a, float v, double time) { return {Type::PadAxis, time, uint32_t(a), uint8_t(pad), v, 0.f}; }
  static InputEvent pad(Type t, int pad, double time) { return {t, time, 0, uint8_t(pad), 0.f, 0.f}; }
  static InputEvent text(char32_t cp, double time) { return {Type::Text, time, uint32_t(cp), 0, 0.f, 0.f}; }
  static InputEvent focusLost(double time) { return {Type::FocusLost, time, 0, 0, 0.f, 0.f}; }
  static InputEvent focusGained(double time) { return {Type::FocusGained, time, 0, 0, 0.f, 0.f}; }
};

}  // namespace litholattice
