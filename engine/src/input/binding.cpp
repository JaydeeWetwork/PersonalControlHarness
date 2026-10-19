#include "litholattice/input/binding.h"

namespace litholattice {

std::string Binding::toString() const {
  auto join = [](std::string_view a, std::string_view b) { return std::string(a) + ":" + std::string(b); };
  switch (source) {
    case Source::Key: return join("key", inputName(Key(code)));
    case Source::MouseButton: return join("mouse", inputName(MouseButton(code)));
    case Source::WheelUp: return "wheel:Up";
    case Source::WheelDown: return "wheel:Down";
    case Source::MouseMotion: return "mouse:Motion";
    case Source::PadButton: return join("pad", inputName(GamepadButton(code)));
    case Source::PadAxisPositive: return join("padaxis", inputName(GamepadAxis(code))) + "+";
    case Source::PadAxisNegative: return join("padaxis", inputName(GamepadAxis(code))) + "-";
    case Source::PadStick: return Stick(code) == Stick::Left ? "stick:Left" : "stick:Right";
  }
  return {};
}

std::optional<Binding> Binding::parse(std::string_view text) {
  const size_t colon = text.find(':');
  if (colon == std::string_view::npos) return std::nullopt;
  const std::string_view kind = text.substr(0, colon), name = text.substr(colon + 1);
  if (kind == "key") { if (auto k = keyFromName(name)) return key(*k); }
  if (kind == "mouse") {
    if (name == "Motion") return mouseMotion();
    if (auto b = mouseButtonFromName(name)) return mouse(*b);
  }
  if (kind == "wheel") {
    if (name == "Up") return wheelUp();
    if (name == "Down") return wheelDown();
  }
  if (kind == "pad") { if (auto b = gamepadButtonFromName(name)) return pad(*b); }
  if (kind == "padaxis" && name.size() > 1 && (name.back() == '+' || name.back() == '-')) {
    if (auto a = gamepadAxisFromName(name.substr(0, name.size() - 1))) return padAxis(*a, name.back() == '+');
  }
  if (kind == "stick") {
    if (name == "Left") return stick(Stick::Left);
    if (name == "Right") return stick(Stick::Right);
  }
  return std::nullopt;
}

}  // namespace litholattice
