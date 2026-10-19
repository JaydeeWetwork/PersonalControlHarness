// Player-operated components: switch, button, keyboard.
#include <format>
#include <tuple>
#include "components.h"
#include "lithokraft/content/signal.h"

namespace lithokraft::components {
namespace {

/// Right-click toggles a steady 64 on every side.
class Switch final : public ComponentBehavior {
 public:
  ComponentStep tick(const ComponentInputs&, CellState st, ComponentMemory&) const override {
    const int out = stOn(st) ? kMaxSignal : 0;
    return {out, 0, withAttach(makeState(out, 0, 0, stOn(st)), stAttach(st))};
  }
  // Free-standing (legacy) switches emit on every side; attached ones power through their support.
  int emitToward(int, CellState st, const ComponentMemory& m) const override { return stAttach(st) ? 0 : m.out; }
  std::optional<CellState> use(CellState st, ComponentMemory&) const override {
    return withAttach(makeState(stValue(st), 0, 0, !stOn(st)), stAttach(st));
  }
  std::string describe(const BlockDef& d, CellState s) const override {
    return std::format("{} | {} (right-click)", d.name, stOn(s) ? "ON" : "off");
  }
};

/// Right-click gives a kButtonTicks-long 64 pulse on every side.
class Button final : public ComponentBehavior {
 public:
  ComponentStep tick(const ComponentInputs&, CellState st, ComponentMemory& m) const override {
    const int out = m.timer > 0 ? kMaxSignal : 0;
    if (m.timer) --m.timer;
    return {out, 0, withAttach(makeState(out, 0, 0, m.timer > 0), stAttach(st))};
  }
  // Like the switch: free-standing buttons emit on every side, attached ones through their support.
  int emitToward(int, CellState st, const ComponentMemory& m) const override { return stAttach(st) ? 0 : m.out; }
  std::optional<CellState> use(CellState st, ComponentMemory& m) const override {
    m.timer = kButtonTicks;
    return withAttach(makeState(stValue(st), 0, 0, true), stAttach(st));
  }
  std::string describe(const BlockDef& d, CellState s) const override {
    return std::format("{} | {} (right-click)", d.name, stOn(s) ? "pressed" : "up");
  }
};

/// Typing terminal. Right-click toggles capture (state "on"); the game queues one pulse per key:
/// SIXBIT code + 1 out the front, control codes (Enter 1, Backspace 2) out the right side.
class Keyboard final : public ComponentBehavior {
 public:
  ComponentStep tick(const ComponentInputs&, CellState st, ComponentMemory& m) const override {
    int out = 0, out2 = 0;
    if (!m.keys.empty()) {
      std::tie(out, out2) = m.keys.front();
      m.keys.pop_front();
    }
    // The value keeps the last character for the look-at readout.
    return {out, out2, makeState(out ? out : stValue(st), stFacing(st), 0, stOn(st))};
  }
  int emitToward(int dir, CellState st, const ComponentMemory& m) const override {
    const int f = stFacing(st);
    return dir == f ? m.out : (dir == dirRight(f) ? m.out2 : 0);
  }
  std::optional<CellState> use(CellState st, ComponentMemory& m) const override {
    m.keys.clear();
    return makeState(stValue(st), stFacing(st), 0, !stOn(st));
  }
  std::string describe(const BlockDef& d, CellState s) const override {
    return std::format("{} | {} | facing {}", d.name, stOn(s) ? "TYPING" : "right-click to type", facingName(stFacing(s)));
  }
};

}  // namespace

void registerInputs(const Registrar& add) {
  add("switch", std::make_unique<Switch>());
  add("button", std::make_unique<Button>());
  add("keyboard", std::make_unique<Keyboard>());
}

}  // namespace lithokraft::components
