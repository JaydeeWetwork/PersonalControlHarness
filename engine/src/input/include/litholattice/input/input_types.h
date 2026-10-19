// Device-independent input codes (keyboard, mouse, gamepad) and their config-file names.
// Backends translate OS codes into these; nothing above the backend sees OS codes.
#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

namespace litholattice {

// X-macro lists keep each enum and its name table in one place. (A list that contains the
// letter X must not call its parameter X: X(X) would name the entry after the parameter.)
#define LITHOLATTICE_INPUT_KEYS(K_)                                                                                    \
  K_(Unknown) K_(A) K_(B) K_(C) K_(D) K_(E) K_(F) K_(G) K_(H) K_(I) K_(J) K_(K) K_(L) K_(M) K_(N) K_(O) K_(P) K_(Q) K_(R)     \
  K_(S) K_(T) K_(U) K_(V) K_(W) K_(X) K_(Y) K_(Z) K_(Num0) K_(Num1) K_(Num2) K_(Num3) K_(Num4) K_(Num5) K_(Num6) K_(Num7)  \
  K_(Num8) K_(Num9) K_(F1) K_(F2) K_(F3) K_(F4) K_(F5) K_(F6) K_(F7) K_(F8) K_(F9) K_(F10) K_(F11) K_(F12) K_(Space)      \
  K_(Enter) K_(Escape) K_(Backspace) K_(Tab) K_(LeftShift) K_(RightShift) K_(LeftCtrl) K_(RightCtrl) K_(LeftAlt)    \
  K_(RightAlt) K_(Up) K_(Down) K_(Left) K_(Right) K_(Insert) K_(Delete) K_(Home) K_(End) K_(PageUp) K_(PageDown)      \
  K_(Minus) K_(Equal) K_(LeftBracket) K_(RightBracket) K_(Semicolon) K_(Apostrophe) K_(Comma) K_(Period) K_(Slash)  \
  K_(Backslash) K_(GraveAccent)
#define LITHOLATTICE_INPUT_MOUSE_BUTTONS(X) X(Left) X(Right) X(Middle) X(Back) X(Forward)
// Gamepad buttons and axes follow the SDL / GLFW standard gamepad layout (Xbox names).
// (The parameter is not called X here: the button X would be substituted too.)
#define LITHOLATTICE_INPUT_PAD_BUTTONS(M_)                                                                    \
  M_(A) M_(B) M_(X) M_(Y) M_(LeftBumper) M_(RightBumper) M_(Back) M_(Start) M_(Guide) M_(LeftThumb) \
  M_(RightThumb) M_(DpadUp) M_(DpadRight) M_(DpadDown) M_(DpadLeft)
#define LITHOLATTICE_INPUT_PAD_AXES(X) X(LeftX) X(LeftY) X(RightX) X(RightY) X(LeftTrigger) X(RightTrigger)

#define LITHOLATTICE_INPUT_ENUMERATOR(n) n,
enum class Key : uint16_t { LITHOLATTICE_INPUT_KEYS(LITHOLATTICE_INPUT_ENUMERATOR) Count };
enum class MouseButton : uint8_t { LITHOLATTICE_INPUT_MOUSE_BUTTONS(LITHOLATTICE_INPUT_ENUMERATOR) Count };
enum class GamepadButton : uint8_t { LITHOLATTICE_INPUT_PAD_BUTTONS(LITHOLATTICE_INPUT_ENUMERATOR) Count };
enum class GamepadAxis : uint8_t { LITHOLATTICE_INPUT_PAD_AXES(LITHOLATTICE_INPUT_ENUMERATOR) Count };  ///< sticks -1..1 (Y down), triggers 0..1
#undef LITHOLATTICE_INPUT_ENUMERATOR

/// Analog sticks (pairs of axes) for radial deadzones.
enum class Stick : uint8_t { Left, Right };

constexpr int kMaxGamepads = 4;

/// Config-file names ("W", "LeftShift", "Middle", "RightBumper", "LeftTrigger").
std::string_view inputName(Key k);
std::string_view inputName(MouseButton b);
std::string_view inputName(GamepadButton b);
std::string_view inputName(GamepadAxis a);
std::optional<Key> keyFromName(std::string_view name);
std::optional<MouseButton> mouseButtonFromName(std::string_view name);
std::optional<GamepadButton> gamepadButtonFromName(std::string_view name);
std::optional<GamepadAxis> gamepadAxisFromName(std::string_view name);

}  // namespace litholattice
