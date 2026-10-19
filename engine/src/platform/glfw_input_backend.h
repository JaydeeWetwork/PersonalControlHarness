// Private to litholattice_platform: GLFW implementation of IInputBackend.
#pragma once
#include <array>
#include <vector>
#include "litholattice/input/input_backend.h"

struct GLFWwindow;

namespace litholattice {

/// Keyboard, mouse (buttons, wheel, motion, cursor), text (char callback), focus, and gamepads
/// through glfwGetGamepadState (SDL_GameControllerDB mappings built into GLFW, plus an optional
/// gamecontrollerdb.txt). Callbacks append to a pending list; poll() pumps GLFW and hands it over.
class GlfwInputBackend final : public IInputBackend {
 public:
  explicit GlfwInputBackend(GLFWwindow* window);
  ~GlfwInputBackend() override;
  GlfwInputBackend(const GlfwInputBackend&) = delete;
  GlfwInputBackend& operator=(const GlfwInputBackend&) = delete;

  std::string_view name() const override { return "glfw"; }
  void poll(std::vector<InputEvent>& out) override;

  /// The next cursor sample only sets the baseline (no jump when the cursor is captured / released).
  void resetMouseBaseline() { haveCursor_ = false; }
  /// Adds SDL-format gamepad mappings (e.g. config/gamecontrollerdb.txt). False if rejected.
  bool addGamepadMappings(const char* text);

 private:
  struct PadState {
    bool present = false;
    std::array<unsigned char, 15> buttons{};
    std::array<float, 6> axes{};
  };
  void pollGamepads(double now);
  void push(const InputEvent& e) { pending_.push_back(e); }

  static void onKey(GLFWwindow* w, int key, int scancode, int action, int mods);
  static void onChar(GLFWwindow* w, unsigned int codepoint);
  static void onMouseButton(GLFWwindow* w, int button, int action, int mods);
  static void onScroll(GLFWwindow* w, double dx, double dy);
  static void onCursor(GLFWwindow* w, double x, double y);
  static void onFocus(GLFWwindow* w, int focused);
  static GlfwInputBackend* from(GLFWwindow* w);

  GLFWwindow* window_;
  std::vector<InputEvent> pending_;
  std::array<PadState, kMaxGamepads> pads_{};
  double lastX_ = 0, lastY_ = 0;
  bool haveCursor_ = false;
};

}  // namespace litholattice
