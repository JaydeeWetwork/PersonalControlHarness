#include "glfw_input_backend.h"
#include <GLFW/glfw3.h>
#include <cmath>
#include <optional>
#include "litholattice/core/log.h"

namespace litholattice {
namespace {

Key fromGlfwKey(int k) {
  if (k >= GLFW_KEY_A && k <= GLFW_KEY_Z) return Key(int(Key::A) + (k - GLFW_KEY_A));
  if (k >= GLFW_KEY_0 && k <= GLFW_KEY_9) return Key(int(Key::Num0) + (k - GLFW_KEY_0));
  if (k >= GLFW_KEY_F1 && k <= GLFW_KEY_F12) return Key(int(Key::F1) + (k - GLFW_KEY_F1));
  switch (k) {
    case GLFW_KEY_SPACE: return Key::Space;
    case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER: return Key::Enter;
    case GLFW_KEY_ESCAPE: return Key::Escape;
    case GLFW_KEY_BACKSPACE: return Key::Backspace;
    case GLFW_KEY_TAB: return Key::Tab;
    case GLFW_KEY_LEFT_SHIFT: return Key::LeftShift;
    case GLFW_KEY_RIGHT_SHIFT: return Key::RightShift;
    case GLFW_KEY_LEFT_CONTROL: return Key::LeftCtrl;
    case GLFW_KEY_RIGHT_CONTROL: return Key::RightCtrl;
    case GLFW_KEY_LEFT_ALT: return Key::LeftAlt;
    case GLFW_KEY_RIGHT_ALT: return Key::RightAlt;
    case GLFW_KEY_UP: return Key::Up;
    case GLFW_KEY_DOWN: return Key::Down;
    case GLFW_KEY_LEFT: return Key::Left;
    case GLFW_KEY_RIGHT: return Key::Right;
    case GLFW_KEY_INSERT: return Key::Insert;
    case GLFW_KEY_DELETE: return Key::Delete;
    case GLFW_KEY_HOME: return Key::Home;
    case GLFW_KEY_END: return Key::End;
    case GLFW_KEY_PAGE_UP: return Key::PageUp;
    case GLFW_KEY_PAGE_DOWN: return Key::PageDown;
    case GLFW_KEY_MINUS: return Key::Minus;
    case GLFW_KEY_EQUAL: return Key::Equal;
    case GLFW_KEY_LEFT_BRACKET: return Key::LeftBracket;
    case GLFW_KEY_RIGHT_BRACKET: return Key::RightBracket;
    case GLFW_KEY_SEMICOLON: return Key::Semicolon;
    case GLFW_KEY_APOSTROPHE: return Key::Apostrophe;
    case GLFW_KEY_COMMA: return Key::Comma;
    case GLFW_KEY_PERIOD: return Key::Period;
    case GLFW_KEY_SLASH: return Key::Slash;
    case GLFW_KEY_BACKSLASH: return Key::Backslash;
    case GLFW_KEY_GRAVE_ACCENT: return Key::GraveAccent;
    default: return Key::Unknown;
  }
}

std::optional<MouseButton> fromGlfwButton(int b) {
  switch (b) {
    case GLFW_MOUSE_BUTTON_LEFT: return MouseButton::Left;
    case GLFW_MOUSE_BUTTON_RIGHT: return MouseButton::Right;
    case GLFW_MOUSE_BUTTON_MIDDLE: return MouseButton::Middle;
    case GLFW_MOUSE_BUTTON_4: return MouseButton::Back;
    case GLFW_MOUSE_BUTTON_5: return MouseButton::Forward;
    default: return std::nullopt;
  }
}

static_assert(GLFW_GAMEPAD_BUTTON_LAST + 1 == int(GamepadButton::Count), "GLFW gamepad button layout");
static_assert(GLFW_GAMEPAD_AXIS_LAST + 1 == int(GamepadAxis::Count), "GLFW gamepad axis layout");

}  // namespace

GlfwInputBackend* GlfwInputBackend::from(GLFWwindow* w) { return static_cast<GlfwInputBackend*>(glfwGetWindowUserPointer(w)); }

GlfwInputBackend::GlfwInputBackend(GLFWwindow* window) : window_(window) {
  glfwSetWindowUserPointer(window_, this);
  glfwSetKeyCallback(window_, &onKey);
  glfwSetCharCallback(window_, &onChar);
  glfwSetMouseButtonCallback(window_, &onMouseButton);
  glfwSetScrollCallback(window_, &onScroll);
  glfwSetCursorPosCallback(window_, &onCursor);
  glfwSetWindowFocusCallback(window_, &onFocus);
  if (glfwRawMouseMotionSupported()) glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
}

GlfwInputBackend::~GlfwInputBackend() {
  glfwSetKeyCallback(window_, nullptr);
  glfwSetCharCallback(window_, nullptr);
  glfwSetMouseButtonCallback(window_, nullptr);
  glfwSetScrollCallback(window_, nullptr);
  glfwSetCursorPosCallback(window_, nullptr);
  glfwSetWindowFocusCallback(window_, nullptr);
  glfwSetWindowUserPointer(window_, nullptr);
}

void GlfwInputBackend::onKey(GLFWwindow* w, int key, int, int action, int) {
  const Key k = fromGlfwKey(key);
  if (k == Key::Unknown) return;
  const auto type = action == GLFW_PRESS ? InputEvent::Type::KeyDown
                    : action == GLFW_RELEASE ? InputEvent::Type::KeyUp : InputEvent::Type::KeyRepeat;
  from(w)->push(InputEvent::key(type, k, glfwGetTime()));
}

void GlfwInputBackend::onChar(GLFWwindow* w, unsigned int cp) { from(w)->push(InputEvent::text(char32_t(cp), glfwGetTime())); }

void GlfwInputBackend::onMouseButton(GLFWwindow* w, int button, int action, int) {
  if (const auto b = fromGlfwButton(button))
    from(w)->push(InputEvent::mouse(action == GLFW_PRESS ? InputEvent::Type::MouseDown : InputEvent::Type::MouseUp, *b, glfwGetTime()));
}

void GlfwInputBackend::onScroll(GLFWwindow* w, double, double dy) { from(w)->push(InputEvent::wheel(float(dy), glfwGetTime())); }

void GlfwInputBackend::onCursor(GLFWwindow* w, double x, double y) {
  GlfwInputBackend* self = from(w);
  const double t = glfwGetTime();
  if (self->haveCursor_) self->push(InputEvent::motion(float(x - self->lastX_), float(y - self->lastY_), t));
  self->lastX_ = x;
  self->lastY_ = y;
  self->haveCursor_ = true;
  int ww, wh, fw, fh;  // HiDPI: window coordinates -> framebuffer pixels
  glfwGetWindowSize(w, &ww, &wh);
  glfwGetFramebufferSize(w, &fw, &fh);
  self->push(InputEvent::cursor(float(ww > 0 ? x * fw / ww : x), float(wh > 0 ? y * fh / wh : y), t));
}

void GlfwInputBackend::onFocus(GLFWwindow* w, int focused) {
  GlfwInputBackend* self = from(w);
  self->haveCursor_ = false;  // no motion jump from where the cursor went while away
  self->push(focused ? InputEvent::focusGained(glfwGetTime()) : InputEvent::focusLost(glfwGetTime()));
}

bool GlfwInputBackend::addGamepadMappings(const char* text) { return glfwUpdateGamepadMappings(text) == GLFW_TRUE; }

// Gamepads are polled (GLFW has no gamepad callbacks); state changes become events.
void GlfwInputBackend::pollGamepads(double now) {
  using T = InputEvent::Type;
  for (int slot = 0; slot < kMaxGamepads; ++slot) {
    PadState& p = pads_[size_t(slot)];
    GLFWgamepadstate st;
    const bool present = glfwJoystickIsGamepad(GLFW_JOYSTICK_1 + slot) && glfwGetGamepadState(GLFW_JOYSTICK_1 + slot, &st);
    if (present != p.present) {
      if (present) logInfo("gamepad %d connected: %s", slot, glfwGetGamepadName(GLFW_JOYSTICK_1 + slot));
      else logInfo("gamepad %d disconnected", slot);
      push(InputEvent::pad(present ? T::PadConnected : T::PadDisconnected, slot, now));
      if (!present) p = {};
      p.present = present;
    }
    if (!present) continue;
    for (int b = 0; b < int(GamepadButton::Count); ++b)
      if (st.buttons[b] != p.buttons[size_t(b)]) {
        p.buttons[size_t(b)] = st.buttons[b];
        push(InputEvent::padButton(st.buttons[b] ? T::PadButtonDown : T::PadButtonUp, slot, GamepadButton(b), now));
      }
    for (int a = 0; a < int(GamepadAxis::Count); ++a) {
      float v = st.axes[a];
      if (a >= GLFW_GAMEPAD_AXIS_LEFT_TRIGGER) v = (v + 1.f) * 0.5f;  // GLFW triggers are -1..1; ours 0..1
      if (std::abs(v - p.axes[size_t(a)]) > 1e-4f) {
        p.axes[size_t(a)] = v;
        push(InputEvent::padAxis(slot, GamepadAxis(a), v, now));
      }
    }
  }
}

void GlfwInputBackend::poll(std::vector<InputEvent>& out) {
  glfwPollEvents();  // runs the callbacks above (and the window's close / resize handling)
  pollGamepads(glfwGetTime());
  out.insert(out.end(), pending_.begin(), pending_.end());
  pending_.clear();
}

}  // namespace litholattice
