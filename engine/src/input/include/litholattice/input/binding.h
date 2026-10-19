// One physical input bound to an action, with its config-file text form:
//   key:W  mouse:Left  wheel:Up  wheel:Down  mouse:Motion  pad:A  padaxis:LeftY-  padaxis:RightTrigger+  stick:Right
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include "litholattice/input/input_types.h"

namespace litholattice {

struct Binding {
  enum class Source : uint8_t {
    Key, MouseButton, WheelUp, WheelDown, MouseMotion,  // keyboard + mouse
    PadButton, PadAxisPositive, PadAxisNegative, PadStick  // gamepad (any connected pad)
  };
  Source source = Source::Key;
  uint16_t code = 0;  ///< Key / MouseButton / GamepadButton / GamepadAxis / Stick

  static Binding key(Key k) { return {Source::Key, uint16_t(k)}; }
  static Binding mouse(MouseButton b) { return {Source::MouseButton, uint16_t(b)}; }
  static Binding wheelUp() { return {Source::WheelUp, 0}; }
  static Binding wheelDown() { return {Source::WheelDown, 0}; }
  static Binding mouseMotion() { return {Source::MouseMotion, 0}; }
  static Binding pad(GamepadButton b) { return {Source::PadButton, uint16_t(b)}; }
  static Binding padAxis(GamepadAxis a, bool positive) {
    return {positive ? Source::PadAxisPositive : Source::PadAxisNegative, uint16_t(a)};
  }
  static Binding stick(Stick s) { return {Source::PadStick, uint16_t(s)}; }

  bool isGamepad() const { return source >= Source::PadButton; }
  /// Text form for the config file.
  std::string toString() const;
  /// Parses the text form; nullopt if malformed or unknown.
  static std::optional<Binding> parse(std::string_view text);

  bool operator==(const Binding&) const = default;
};

}  // namespace litholattice
