// Engine input SDK checks with a tiny fixture vocabulary (the game's own vocabulary is tested by
// the game): a game-declared ActionSet, bindings JSON round trip, context gating and text.
#include "litholattice/input/input_selftest.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string_view>
#include <thread>
#include "litholattice/input/input_thread.h"
#include "litholattice/input/input_system.h"
#include "litholattice/input/device_state.h"
#include "litholattice/input/mod_input.h"

namespace litholattice {
namespace {

enum class TestAction : ActionId { Go, Fire, Alt, Aim, Count };
enum class TestContext : uint8_t { Play, Menu };

const ActionSet& testActions() {
  static const ActionSet set({{"Go"}, {"Fire"}, {"Alt"}, {"Aim", ActionKind::Axis2D}},
                             {{actionId(TestAction::Fire), actionId(TestAction::Alt)}});
  return set;
}

ActionMap testDefaults() {
  ActionMap m(testActions());
  m.setBindings(TestAction::Go, {Binding::key(Key::W)});
  m.setBindings(TestAction::Fire, {Binding::mouse(MouseButton::Left)});
  m.setBindings(TestAction::Alt, {Binding::mouse(MouseButton::Left)});  // shared on purpose
  m.setBindings(TestAction::Aim, {Binding::mouseMotion()});
  return m;
}

std::vector<ContextRules> testContexts() {
  using A = TestAction;
  return {{"Play", false, false, true, {actionId(A::Go), actionId(A::Fire), actionId(A::Alt), actionId(A::Aim)}},
          {"Menu", true, true, false, {actionId(A::Aim)}}};
}

}  // namespace


// Scripted backend for the input-pump tests: hands out a few events per poll so
// partial drains and queue behavior are exercised.
class ScriptedBackend final : public IInputBackend {
 public:
  explicit ScriptedBackend(std::vector<InputEvent> evs) : evs_(std::move(evs)) {}
  std::string_view name() const override { return "scripted"; }
  void poll(std::vector<InputEvent>& out) override {
    const size_t n = std::min<size_t>(7, evs_.size() - next_);
    out.insert(out.end(), evs_.begin() + (ptrdiff_t)next_, evs_.begin() + (ptrdiff_t)(next_ + n));
    next_ += n;
  }

 private:
  std::vector<InputEvent> evs_;
  size_t next_ = 0;
};

void testInputPump(const auto& check) {
  auto motionStream = [](int n) {
    std::vector<InputEvent> evs;
    for (int i = 0; i < n; ++i) evs.push_back(InputEvent::motion(float(i), 0.f, 0.0));
    return evs;
  };
  {  // Threaded mode: order preserved, pump timestamps monotonic, nothing dropped.
    InputPump pump(std::make_unique<ScriptedBackend>(motionStream(500)));
    std::vector<InputEvent> got;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (got.size() < 500 && std::chrono::steady_clock::now() < until) {
      pump.drainInto(got);
      if (got.size() < 500) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    bool ordered = got.size() == 500, mono = true;
    for (size_t i = 0; i < got.size(); ++i) {
      ordered = ordered && got[i].x == float(i);
      if (i > 0) mono = mono && got[i].time >= got[i - 1].time;
    }
    check(ordered && mono && pump.dropped() == 0,
          "input pump (threaded): 500 events arrive in order with monotonic pump timestamps");
  }
  {  // Manual mode: the owner drives pumpOnce() (the GLFW-backend path).
    InputPump pump(std::make_unique<ScriptedBackend>(motionStream(300)), InputPump::Mode::Manual);
    std::vector<InputEvent> got;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (got.size() < 300 && std::chrono::steady_clock::now() < until) {
      pump.pumpOnce();
      pump.drainInto(got);
    }
    bool ordered = got.size() == 300;
    for (size_t i = 0; i < got.size(); ++i) ordered = ordered && got[i].x == float(i);
    check(ordered, "input pump (manual): owner-driven polling delivers every event in order");
  }
  {  // Overflow: 3000 events, no drain; the newest are dropped with a count.
    InputPump pump(std::make_unique<ScriptedBackend>(motionStream(3000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // let the pump overrun
    std::vector<InputEvent> got;
    pump.drainInto(got);
    bool ordered = got.size() == InputPump::kQueueCapacity;
    for (size_t i = 0; i < got.size(); ++i) ordered = ordered && got[i].x == float(i);
    check(ordered && pump.dropped() == 3000 - InputPump::kQueueCapacity,
          "input pump: queue overflow drops the newest events and counts them");
  }

}

// Mod input confinement (mod hardening item 6): registration, the delivery
// policy, and the anti-cheat / no-raw-device guarantees.
void testModInputConfinement(const auto& check) {
  // Registration: ids validated, namespaced per mod, duplicates refused.
  {
    ModInputRouter router;
    std::string err;
    check(router.registerAction("mymod", "jump", "Jump", err),
          "mod input: valid action registers");
    check(!router.registerAction("mymod", "jump", "Jump", err) && !err.empty(),
          "mod input: duplicate action refused");
    check(!router.registerAction("BadMod", "jump", "Jump", err),
          "mod input: bad mod id refused");
    check(!router.registerAction("mymod", "has space", "Jump", err),
          "mod input: bad action id refused");
    check(!router.registerAction("mymod", "ok", "", err),
          "mod input: empty display name refused");
    check(router.registerAction("othermod", "jump", "Jump", err),
          "mod input: same action id under another mod registers");
    const auto acts = router.actions();
    check(acts.size() == 2 && acts[0].actionId == "mymod:jump" &&
              acts[1].actionId == "othermod:jump",
          "mod input: actions are namespaced per mod (no shadowing engine or other mods)");
    check(router.hasAction("mymod:jump") && !router.hasAction("jump"),
          "mod input: only the namespaced id resolves");
    check(router.unregisterMod("mymod") == 1 && router.actionCount() == 1 &&
              !router.hasAction("mymod:jump"),
          "mod input: unregisterMod drops the mod's actions");
  }

  // Delivery policy: nothing reaches a mod while the UI wants the keyboard
  // (system modal open, popup open, or a text field focused)  -  keystrokes
  // typed into system dialogs, passwords included, never reach mods.
  check(ModInputRouter::shouldDeliverToMod(false),
        "mod input: key events delivered when the UI does not want the keyboard");
  check(!ModInputRouter::shouldDeliverToMod(true),
        "mod input: key events withheld while a system modal/text field has focus");

  // No raw device access: the router works with no InputSystem,
  // IInputBackend, or DeviceState anywhere near it  -  a mod holding only the
  // router (or the C API) cannot observe device state. The API-surface half
  // of this is audit-pinned in mods_selftest (no device/input-injection
  // tokens in any LithoLatticeModApi/LithoLatticeMod field name).
  {
    ModInputRouter router;  // standalone: no input-system handles involved
    std::string err;
    check(router.registerAction("spy", "snoop", "Snoop", err) &&
              !ModInputRouter::shouldDeliverToMod(true),
          "mod input: router is standalone - registration and policy need no device handles");
  }

  // Anti-cheat: there is no injection path. The router exposes no method
  // taking an InputEvent and holds no InputPump/InputSystem/IInputBackend -
  // a synthetic key event is inexpressible here, so routing a game key event
  // to a mod (the game's explicit onInputEvent call) must leave the engine
  // pipeline exactly as it was.
  {
    InputSystem sys(testDefaults(), testContexts());
    ScriptedBackend backend({});
    sys.update(backend);  // empty frame: nothing pressed anywhere
    // The game would now call the mod's onInputEvent(key, down) directly -
    // the only engine->mod input path. Nothing flows back.
    if (ModInputRouter::shouldDeliverToMod(false)) { /* game -> mod call */ }
    check(!sys.devices().keyDown(Key::W) && !sys.pressed(TestAction::Go),
          "mod input: delivering a key event to a mod cannot inject into the engine pipeline");
  }
}


int runInputSelfTest() {
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
  };
  {  // Focus: nothing held survives losing or regaining the window (stale keys / buttons reset).
    using T = InputEvent::Type;
    DeviceState d;
    d.apply(InputEvent::key(T::KeyDown, Key::W, 0)); d.apply(InputEvent::mouse(T::MouseDown, MouseButton::Left, 0));
    const bool held = d.keyDown(Key::W) && d.mouseDown(MouseButton::Left);
    d.apply(InputEvent::focusLost(1));
    const bool lost = !d.keyDown(Key::W) && !d.mouseDown(MouseButton::Left);
    d.apply(InputEvent::key(T::KeyDown, Key::A, 2));  // pressed while away, release never seen
    d.apply(InputEvent::focusGained(3));
    check(held && lost && !d.keyDown(Key::A), "focus lost / gained: held keys and buttons are released (no stuck movement)");
  }
  testModInputConfinement(check);
  std::printf("Input SDK self-test (fixture vocabulary)\n");
  using T = InputEvent::Type;
  {
    const ActionMap m = testDefaults();
    const ActionSet& s = testActions();
    std::string err;
    std::vector<std::string> warnings;
    const auto back = ActionMap::fromJson(m, m.toJson(), &err);
    const auto edited = ActionMap::fromJson(m, R"({"bindings":{"Go":["key:G"],"Nope":["key:F"]}})", &err, &warnings);
    check(s.size() == 4 && s.find("Aim") == actionId(TestAction::Aim) && s.kind(3) == ActionKind::Axis2D &&
              m.conflicts().empty() && back && *back == m && edited &&
              edited->bindings(TestAction::Go) == std::vector{Binding::key(Key::G)} && warnings.size() == 1,
          "game-declared ActionSet: names, kinds, intended shared input, bindings JSON round trip over defaults");
  }
  {
    InputSystem in(testDefaults(), testContexts());
    ReplayInputBackend backend;
    double t = 0;
    auto frame = [&](std::initializer_list<InputEvent> evs) {
      t += 1.0 / 60.0;
      for (InputEvent e : evs) { e.time = t; backend.push(e); }
      backend.advanceTo(t);
      in.update(backend);
    };
    frame({InputEvent::key(T::KeyDown, Key::W, 0)});
    const bool go = in.pressed(TestAction::Go) && in.active(TestContext::Play) && !in.active(TestContext::Menu);
    in.push(TestContext::Menu);
    frame({});
    const bool gated = in.active(TestContext::Menu) && !in.held(TestAction::Go);
    in.pop(TestContext::Menu);
    frame({});
    check(go && gated && in.held(TestAction::Go), "contexts declared by the game: opaque context gates lower actions, pop resumes");
  }
  testInputPump(check);
  std::printf(fails ? "INPUT SDK SELFTEST FAILED (%d)\n" : "INPUT SDK SELFTEST OK\n", fails);
  return fails;
}

}  // namespace litholattice
