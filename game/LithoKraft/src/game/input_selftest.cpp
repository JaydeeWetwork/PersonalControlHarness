// Injects fake events through ReplayInputBackend and checks what LithoKraft sees: its actions,
// default bindings and contexts on the engine input SDK.
#include "ui_input_bridge.h"
#include "lithokraft/game/input_selftest.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#ifdef _WIN32
#include <process.h>  // _getpid: per-run unique temp dirs (G-M7, P2b-2)
#define getpid _getpid
#else
#include <unistd.h>  // getpid: per-run unique temp dirs (G-M7, P2b-2)
#endif
#include "litholattice/input/input_system.h"
#include "lithokraft/game/actions.h"

namespace lithokraft {
namespace {

using T = InputEvent::Type;

/// Drives an InputSystem one frame at a time from scripted events.
struct Rig {
  InputSystem input{defaultActionMap(), inputContexts()};
  ReplayInputBackend backend;
  double t = 0.0;

  /// Runs one frame containing `events` (their timestamps are set to this frame).
  void frame(std::initializer_list<InputEvent> events = {}) {
    t += 1.0 / 60.0;
    for (InputEvent e : events) { e.time = t; backend.push(e); }
    backend.advanceTo(t);
    input.update(backend);
  }
  static InputEvent down(Key k) { return InputEvent::key(T::KeyDown, k, 0); }
  static InputEvent up(Key k) { return InputEvent::key(T::KeyUp, k, 0); }
};

bool near(float a, float b, float tol = 1e-4f) { return std::abs(a - b) <= tol; }

}  // namespace

int runGameInputSelfTest() {
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
  };
  std::printf("Game input self-test (LithoKraft actions, injected events)\n");

  {
    const ActionMap m = defaultActionMap();
    check(m.conflicts().empty() && m.bindings(Action::MoveForward).size() == 2 &&
              m.bindings(Action::Break)[1] == Binding::padAxis(GamepadAxis::RightTrigger, true),
          "defaults: MC keyboard/mouse + gamepad, several bindings per action, no conflicts (Place/Use share RMB)");
  }
  {
    Rig r;
    r.frame({Rig::down(Key::W)});
    const bool p1 = r.input.pressed(Action::MoveForward) && r.input.held(Action::MoveForward);
    r.frame();
    const bool h2 = !r.input.pressed(Action::MoveForward) && r.input.held(Action::MoveForward) && r.input.value(Action::MoveForward) == 1.f;
    r.frame({Rig::up(Key::W)});
    const bool r3 = r.input.released(Action::MoveForward) && !r.input.held(Action::MoveForward);
    r.frame({InputEvent::mouse(T::MouseDown, MouseButton::Left, 0), InputEvent::mouse(T::MouseUp, MouseButton::Left, 0)});
    const bool tap = r.input.pressed(Action::Break) && r.input.released(Action::Break) && !r.input.held(Action::Break);
    check(p1 && h2 && r3 && tap, "button actions: pressed / held / released; a click within one frame still presses");
  }
  {
    Rig r;
    r.frame({InputEvent::pad(T::PadConnected, 0, 0), InputEvent::padAxis(0, GamepadAxis::LeftY, -1.f, 0),
             InputEvent::padAxis(0, GamepadAxis::RightTrigger, 0.9f, 0)});
    const bool stick = r.input.held(Action::MoveForward) && near(r.input.value(Action::MoveForward), 1.f) && r.input.pressed(Action::Break);
    r.frame({InputEvent::padAxis(0, GamepadAxis::LeftY, -0.6f, 0), InputEvent::padButton(T::PadButtonDown, 0, GamepadButton::RightBumper, 0)});
    const float expect = std::pow((0.6f - 0.2f) / 0.8f, 2.f);  // deadzone 0.2, curve 2
    const bool analog = near(r.input.value(Action::MoveForward), expect) && r.input.pressed(Action::HotbarNext);
    r.frame({InputEvent::pad(T::PadDisconnected, 0, 0)});
    const bool gone = !r.input.held(Action::MoveForward) && !r.input.held(Action::HotbarNext) && r.input.devices().connectedPads() == 0;
    check(stick && analog && gone, "gamepad: stick + trigger drive actions with analog values; disconnect releases everything");
  }
  {
    const AnalogSettings s;
    const AxisValue small = applyRadialDeadzone({0.12f, 0.12f}, s.stickDeadzone, 1.f);  // |v| = 0.17 < 0.2
    const AxisValue diag = applyRadialDeadzone({0.6f, 0.6f}, s.stickDeadzone, 1.f);
    const AxisValue full = applyRadialDeadzone({1.f, 0.f}, s.stickDeadzone, s.stickCurve);
    const float m = (std::hypot(0.6f, 0.6f) - 0.2f) / 0.8f;
    check(small.x == 0.f && small.y == 0.f && near(diag.x, diag.y) && near(std::hypot(diag.x, diag.y), m) && near(full.x, 1.f) &&
              applyDeadzone(0.05f, 0.1f, 1.f) == 0.f && near(applyDeadzone(-0.55f, 0.1f, 1.f), -0.5f),
          "deadzones: radial on sticks (direction kept, rescaled), linear on triggers / axes");
  }
  {
    Rig r;
    r.frame({InputEvent::motion(100.f, -40.f, 0)});
    const AxisValue mouse = r.input.axis(Action::Look, 1.f / 60.f);
    r.frame({InputEvent::pad(T::PadConnected, 1, 0), InputEvent::padAxis(1, GamepadAxis::RightX, 1.f, 0)});
    const AxisValue stick = r.input.axis(Action::Look, 0.5f);
    check(near(mouse.x, 0.25f) && near(mouse.y, -0.1f) && near(stick.x, 1.5f) && near(stick.y, 0.f),
          "look axis: mouse pixels x sensitivity; right stick x look speed x dt");
  }
  {
    Rig r;
    r.frame({Rig::down(Key::E)});
    const bool opened = r.input.pressed(Action::Inventory);
    r.input.push(InputContext::Inventory);
    const bool deferred = !r.input.active(InputContext::Inventory);
    r.frame({Rig::down(Key::W)});
    const bool noToggle = !r.input.pressed(Action::Inventory) && r.input.active(InputContext::Inventory);
    const bool noLeak = !r.input.held(Action::MoveForward) && !r.input.pressed(Action::Hotbar1);
    r.frame({Rig::down(Key::F3), InputEvent::mouse(T::MouseDown, MouseButton::Left, 0)});
    // The click is for the inventory UI (raw events), not a Break action.
    const bool debugAndClick = r.input.pressed(Action::ToggleDebug) && !r.input.pressed(Action::Break) &&
                               r.input.frameEvents().size() == 2;
    r.input.pop(InputContext::Inventory);
    r.frame();
    const bool resumes = r.input.held(Action::MoveForward) && !r.input.pressed(Action::MoveForward);
    check(opened && deferred && noToggle && noLeak && debugAndClick && resumes,
          "contexts: inventory hides movement/hotbar/Break, keeps debug + raw UI events; pushes apply next frame (E does not re-toggle)");
  }
  {
    Rig r;
    r.input.push(InputContext::KeyboardTyping);
    r.frame();
    r.frame({Rig::down(Key::H), InputEvent::text(U'h', 0), Rig::down(Key::Num1), InputEvent::text(U'1', 0),
             Rig::down(Key::E), InputEvent::text(U'e', 0), Rig::down(Key::Enter),
             InputEvent::key(T::KeyRepeat, Key::Enter, 0), Rig::down(Key::Backspace), Rig::down(Key::F7)});
    const TextInput& tx = r.input.text();
    const bool text = tx.chars == U"h1e" && tx.enter == 2 && tx.backspace == 1;
    {
      Rig t;
      t.input.push(InputContext::KeyboardTyping);
      t.frame();
      t.frame({Rig::down(Key::LeftAlt), Rig::down(Key::Enter)});
      check(t.input.text().enter == 0, "typing: Alt+Enter (display toggle) is not a text Enter");
    }
    {
      Rig u;
      u.frame({Rig::down(Key::LeftAlt), Rig::down(Key::Enter)});
      std::vector<ui::UiEvent> evs;
      translateUiEvents(u.input.frameEvents(), u.input.devices(), evs);
      bool uiEnter = false;
      for (const ui::UiEvent& e : evs) uiEnter = uiEnter || (e.type == ui::UiEvent::Type::KeyDown && e.key == ui::Key::Enter);
      u.frame({InputEvent::key(T::KeyUp, Key::LeftAlt, 0), InputEvent::key(T::KeyUp, Key::Enter, 0)});
      u.frame({Rig::down(Key::Enter)});
      evs.clear();
      translateUiEvents(u.input.frameEvents(), u.input.devices(), evs);
      bool plainEnter = false;
      for (const ui::UiEvent& e : evs) plainEnter = plainEnter || (e.type == ui::UiEvent::Type::KeyDown && e.key == ui::Key::Enter);
      check(!uiEnter && plainEnter, "UI bridge: Alt+Enter never reaches widgets as Enter; plain Enter still does");
    }
    const bool blocked = !r.input.pressed(Action::ToggleControls) && !r.input.pressed(Action::Hotbar1) &&
                         !r.input.pressed(Action::Inventory) && !r.input.held(Action::MoveForward);
    const bool debug = r.input.pressed(Action::CycleTPS);
    r.input.pop(InputContext::KeyboardTyping);
    r.frame({InputEvent::text(U'x', 0)});
    check(text && blocked && debug && r.input.text().empty(),
          "typing: text channel gets chars + Enter/Backspace (with repeat); letters never reach actions; no text outside typing");
  }
  {
    Rig r;
    r.frame({Rig::down(Key::W), Rig::down(Key::LeftShift)});
    r.frame({InputEvent::focusLost(0)});
    check(r.input.released(Action::MoveForward) && !r.input.held(Action::Sneak), "focus loss releases held keys (no stuck movement)");
  }
  {
    ActionMap m = defaultActionMap();
    m.setBindings(Action::Jump, {Binding::key(Key::J), Binding::pad(GamepadButton::Y)});
    m.analog().invertY = true;
    m.analog().stickDeadzone = 0.15f;
    std::string err;
    std::vector<std::string> warnings;
    const auto back = ActionMap::fromJson(defaultActionMap(), m.toJson(), &err);
    const auto partial = ActionMap::fromJson(defaultActionMap(), R"({"bindings":{"Drop":["key:G","key:NoSuchKey"],"Fly":["key:F"]}})", &err, &warnings);
    const bool bad = !ActionMap::fromJson(defaultActionMap(), "{\"bindings\": [", &err);
    check(back && *back == m && partial && partial->bindings(Action::Drop) == std::vector{Binding::key(Key::G)} &&
              partial->bindings(Action::Jump) == defaultActionMap().bindings(Action::Jump) && warnings.size() == 2 && bad,
          "JSON: round trip; missing actions keep defaults; unknown entries warned; syntax errors rejected");
  }
  {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p = fs::temp_directory_path(ec) / ("litholattice-selftest-input-" + std::to_string(::getpid())) / "input.json";
    fs::remove(p, ec);
    const BindingsFile created = loadOrCreateBindings(p, defaultActionMap());
    const BindingsFile loaded = loadOrCreateBindings(p, defaultActionMap());
    check(created.status == BindingsFile::Status::Created && loaded.status == BindingsFile::Status::Loaded &&
              loaded.map == defaultActionMap(),
          "config file: created from defaults on first run, then loaded back unchanged");
    fs::remove_all(p.parent_path(), ec);
  }
  {
    Rig live;
    live.input.startRecording();
    const std::initializer_list<InputEvent> frames[] = {
        {Rig::down(Key::W), InputEvent::motion(5.f, 2.f, 0)}, {InputEvent::wheel(-1.f, 0)}, {Rig::up(Key::W)}};
    std::vector<bool> seen;
    for (const auto& f : frames) {
      live.frame(f);
      seen.push_back(live.input.held(Action::MoveForward));
      seen.push_back(live.input.pressed(Action::HotbarNext));
    }
    const std::vector<InputEvent> rec = live.input.stopRecording();
    InputSystem replayed{defaultActionMap(), inputContexts()};
    ReplayInputBackend backend(rec);
    std::vector<bool> again;
    for (int i = 1; i <= 3; ++i) {
      backend.advanceTo(i / 60.0 + 1e-9);
      replayed.update(backend);
      again.push_back(replayed.held(Action::MoveForward));
      again.push_back(replayed.pressed(Action::HotbarNext));
    }
    check(rec.size() == 4 && seen == again && backend.finished(), "replay: a recorded event stream reproduces the same actions");
  }
  {
    bool names = true;  // every key name round-trips (the letter X once came out as the macro name)
    for (int k = 0; k < int(Key::Count); ++k) names = names && keyFromName(inputName(Key(k))) == Key(k);
    check(names && inputName(Key::X) == "X", "names: every key name round-trips (incl. X)");
  }
  std::printf(fails ? "INPUT SELFTEST FAILED (%d)\n" : "INPUT SELFTEST OK\n", fails);
  return fails;
}

}  // namespace lithokraft
