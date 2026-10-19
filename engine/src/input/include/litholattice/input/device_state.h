// Raw device state built from events: what is held now, what changed this frame, motion and text.
// Knows nothing about actions or contexts.
#pragma once
#include <array>
#include <bitset>
#include <string>
#include "litholattice/input/input_event.h"

namespace litholattice {

/// Text typed this frame (from the OS text channel) plus editing keys (including key repeat).
struct TextInput {
  std::u32string chars;
  int enter = 0, backspace = 0;
  bool empty() const { return chars.empty() && enter == 0 && backspace == 0; }
};

class DeviceState {
 public:
  struct Pad {
    bool connected = false;
    std::bitset<size_t(GamepadButton::Count)> down, pressed, released;
    std::array<float, size_t(GamepadAxis::Count)> axes{};
  };

  /// Clears per-frame data (edges, motion, wheel, text). Held state persists.
  void beginFrame();
  /// Applies one event.
  void apply(const InputEvent& e);

  bool keyDown(Key k) const { return keys_.down[size_t(k)]; }
  bool keyPressed(Key k) const { return keys_.pressed[size_t(k)]; }
  bool keyReleased(Key k) const { return keys_.released[size_t(k)]; }
  bool mouseDown(MouseButton b) const { return mouse_.down[size_t(b)]; }
  bool mousePressed(MouseButton b) const { return mouse_.pressed[size_t(b)]; }
  bool mouseReleased(MouseButton b) const { return mouse_.released[size_t(b)]; }
  float mouseDx() const { return mouseDx_; }
  float mouseDy() const { return mouseDy_; }
  float wheel() const { return wheel_; }
  float cursorX() const { return cursorX_; }
  float cursorY() const { return cursorY_; }
  const Pad& pad(int i) const { return pads_[size_t(i)]; }
  int connectedPads() const;
  const TextInput& text() const { return text_; }

 private:
  template <size_t N>
  struct Buttons {
    std::bitset<N> down, pressed, released;
    void set(size_t i, bool on);
    void releaseAll() { released |= down; down.reset(); }
  };
  void releaseAll();

  Buttons<size_t(Key::Count)> keys_;
  Buttons<size_t(MouseButton::Count)> mouse_;
  std::array<Pad, kMaxGamepads> pads_{};
  float mouseDx_ = 0, mouseDy_ = 0, wheel_ = 0, cursorX_ = 0, cursorY_ = 0;
  TextInput text_;
};

}  // namespace litholattice
