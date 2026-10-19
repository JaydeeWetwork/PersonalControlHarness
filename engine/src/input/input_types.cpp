#include "litholattice/input/input_types.h"
#include <array>

namespace litholattice {
namespace {

#define LITHOLATTICE_INPUT_NAME(n) #n,
constexpr std::string_view kKeyNames[] = {LITHOLATTICE_INPUT_KEYS(LITHOLATTICE_INPUT_NAME)};
constexpr std::string_view kMouseNames[] = {LITHOLATTICE_INPUT_MOUSE_BUTTONS(LITHOLATTICE_INPUT_NAME)};
constexpr std::string_view kPadButtonNames[] = {LITHOLATTICE_INPUT_PAD_BUTTONS(LITHOLATTICE_INPUT_NAME)};
constexpr std::string_view kPadAxisNames[] = {LITHOLATTICE_INPUT_PAD_AXES(LITHOLATTICE_INPUT_NAME)};
#undef LITHOLATTICE_INPUT_NAME

static_assert(std::size(kKeyNames) == size_t(Key::Count));
static_assert(std::size(kMouseNames) == size_t(MouseButton::Count));
static_assert(std::size(kPadButtonNames) == size_t(GamepadButton::Count));
static_assert(std::size(kPadAxisNames) == size_t(GamepadAxis::Count));

template <typename E, size_t N>
std::optional<E> fromName(const std::string_view (&names)[N], std::string_view name) {
  for (size_t i = 0; i < N; ++i)
    if (names[i] == name) return E(i);
  return std::nullopt;
}

}  // namespace

std::string_view inputName(Key k) { return kKeyNames[size_t(k)]; }
std::string_view inputName(MouseButton b) { return kMouseNames[size_t(b)]; }
std::string_view inputName(GamepadButton b) { return kPadButtonNames[size_t(b)]; }
std::string_view inputName(GamepadAxis a) { return kPadAxisNames[size_t(a)]; }
std::optional<Key> keyFromName(std::string_view n) { return fromName<Key>(kKeyNames, n); }
std::optional<MouseButton> mouseButtonFromName(std::string_view n) { return fromName<MouseButton>(kMouseNames, n); }
std::optional<GamepadButton> gamepadButtonFromName(std::string_view n) { return fromName<GamepadButton>(kPadButtonNames, n); }
std::optional<GamepadAxis> gamepadAxisFromName(std::string_view n) { return fromName<GamepadAxis>(kPadAxisNames, n); }

}  // namespace litholattice
