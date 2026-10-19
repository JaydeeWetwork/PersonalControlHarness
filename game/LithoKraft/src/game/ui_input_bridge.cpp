#include "ui_input_bridge.h"
#include <optional>

namespace lithokraft {
namespace {

std::optional<ui::Key> uiKey(Key k) {
  switch (k) {
    case Key::Tab: return ui::Key::Tab;
    case Key::Enter: return ui::Key::Enter;
    case Key::Escape: return ui::Key::Escape;
    case Key::Backspace: return ui::Key::Backspace;
    case Key::Delete: return ui::Key::Delete;
    case Key::Left: return ui::Key::Left;
    case Key::Right: return ui::Key::Right;
    case Key::Up: return ui::Key::Up;
    case Key::Down: return ui::Key::Down;
    case Key::Home: return ui::Key::Home;
    case Key::End: return ui::Key::End;
    case Key::PageUp: return ui::Key::PageUp;
    case Key::PageDown: return ui::Key::PageDown;
    case Key::Space: return ui::Key::Space;
    case Key::A: return ui::Key::A;
    case Key::C: return ui::Key::C;
    case Key::V: return ui::Key::V;
    case Key::X: return ui::Key::X;
    case Key::Z: return ui::Key::Z;
    case Key::Y: return ui::Key::Y;
    case Key::S: return ui::Key::S;
    case Key::F10: return ui::Key::F10;
    default: return std::nullopt;
  }
}

std::optional<ui::PointerButton> uiButton(MouseButton b) {
  switch (b) {
    case MouseButton::Left: return ui::PointerButton::Left;
    case MouseButton::Right: return ui::PointerButton::Right;
    case MouseButton::Middle: return ui::PointerButton::Middle;
    default: return std::nullopt;
  }
}

}  // namespace

void translateUiEvents(std::span<const InputEvent> events, const DeviceState& devices, std::vector<ui::UiEvent>& out) {
  uint8_t mods = ui::kModNone;
  if (devices.keyDown(Key::LeftShift) || devices.keyDown(Key::RightShift)) mods |= ui::kModShift;
  if (devices.keyDown(Key::LeftCtrl) || devices.keyDown(Key::RightCtrl)) mods |= ui::kModCtrl;
  if (devices.keyDown(Key::LeftAlt) || devices.keyDown(Key::RightAlt)) mods |= ui::kModAlt;
  ui::Vec2 cursor{devices.cursorX(), devices.cursorY()};
  using T = InputEvent::Type;
  for (const InputEvent& e : events) {
    ui::UiEvent u;
    bool emit = true;
    switch (e.type) {
      case T::CursorPos:
        cursor = {e.x, e.y};
        u = ui::UiEvent::move(cursor);
        break;
      case T::MouseDown:
      case T::MouseUp:
        if (const auto b = uiButton(MouseButton(e.code)))
          u = e.type == T::MouseDown ? ui::UiEvent::down(cursor, *b) : ui::UiEvent::up(cursor, *b);
        else emit = false;
        break;
      case T::MouseWheel: u = ui::UiEvent::scroll(cursor, e.y); break;
      case T::KeyDown:
      case T::KeyRepeat:
        // Alt+Enter toggles the display mode (Window::pollDisplayToggle): never a UI Enter.
        if (Key(e.code) == Key::Enter && (mods & ui::kModAlt)) emit = false;
        else if (const auto k = uiKey(Key(e.code))) u = ui::UiEvent::keyDown(*k);
        else emit = false;
        break;
      case T::Text: u = ui::UiEvent::text(char32_t(e.code)); break;
      default: emit = false; break;
    }
    if (!emit) continue;
    u.mods = mods;
    out.push_back(u);
  }
}

}  // namespace lithokraft
