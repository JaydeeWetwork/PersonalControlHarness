// Device-independent UI input. The game translates its input events into these (ui does not
// depend on the input module), so tests can drive the UI headlessly.
#pragma once
#include <cstdint>
#include "litholattice/ui/geometry.h"

namespace litholattice::ui {

/// Keys the UI reacts to; everything else arrives as text.
enum class Key : uint8_t {
  None, Tab, Enter, Escape, Backspace, Delete, Left, Right, Up, Down, Home, End, PageUp, PageDown, Space,
  A, C, V, X, Z, Y, S,  // shortcuts (with Ctrl): select all, copy, paste, cut, undo, redo, save
  F10,
};

/// Modifier bits held during an event.
enum Modifier : uint8_t { kModNone = 0, kModShift = 1, kModCtrl = 2, kModAlt = 4 };

enum class PointerButton : uint8_t { Left, Right, Middle };

struct UiEvent {
  enum class Type : uint8_t {
    PointerMove,   ///< pos
    PointerDown,   ///< pos, button
    PointerUp,     ///< pos, button
    Wheel,         ///< pos, wheel (+ = up)
    KeyDown,       ///< key, mods (also sent for key repeat)
    Text,          ///< codepoint
  };
  Type type = Type::PointerMove;
  Vec2 pos;
  PointerButton button = PointerButton::Left;
  float wheel = 0;
  Key key = Key::None;
  uint8_t mods = kModNone;
  char32_t codepoint = 0;

  static UiEvent move(Vec2 p) { UiEvent e; e.type = Type::PointerMove; e.pos = p; return e; }
  static UiEvent down(Vec2 p, PointerButton b = PointerButton::Left) { UiEvent e; e.type = Type::PointerDown; e.pos = p; e.button = b; return e; }
  static UiEvent up(Vec2 p, PointerButton b = PointerButton::Left) { UiEvent e; e.type = Type::PointerUp; e.pos = p; e.button = b; return e; }
  static UiEvent scroll(Vec2 p, float steps) { UiEvent e; e.type = Type::Wheel; e.pos = p; e.wheel = steps; return e; }
  static UiEvent keyDown(Key k, uint8_t m = kModNone) { UiEvent e; e.type = Type::KeyDown; e.key = k; e.mods = m; return e; }
  static UiEvent text(char32_t c) { UiEvent e; e.type = Type::Text; e.codepoint = c; return e; }

  bool shift() const { return (mods & kModShift) != 0; }
  bool ctrl() const { return (mods & kModCtrl) != 0; }
};

}  // namespace litholattice::ui
