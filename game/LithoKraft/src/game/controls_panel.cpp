#include "controls_panel.h"
#include <algorithm>
#include <cstdio>
#include <initializer_list>

namespace lithokraft {
namespace {

struct Row {
  const char* label;
  std::initializer_list<Action> actions;
};

const Row kRows[] = {
    {"look (click: grab)", {Action::Look}},
    {"move", {Action::MoveForward, Action::MoveLeft, Action::MoveBack, Action::MoveRight}},
    {"jump (2x: flight)", {Action::Jump}},
    {"sneak / down (fly)", {Action::Sneak}},
    {"crawl", {Action::Crawl}},
    {"sprint", {Action::Sprint}},
    {"break", {Action::Break}},
    {"place / use", {Action::Place, Action::Use}},
    {"pick block", {Action::PickBlock}},
    {"hotbar slot", {Action::Hotbar1, Action::Hotbar2, Action::Hotbar3, Action::Hotbar4, Action::Hotbar5,
                     Action::Hotbar6, Action::Hotbar7, Action::Hotbar8, Action::Hotbar9}},
    {"hotbar next / prev", {Action::HotbarNext, Action::HotbarPrev}},
    {"inventory", {Action::Inventory}},
    {"clear slot", {Action::Drop}},
    {"hide HUD", {Action::ToggleHUD}},
    {"debug stats", {Action::ToggleDebug}},
    {"switch dimension", {Action::SwitchDim}},
    {"TPS 32/64/128/256", {Action::CycleTPS}},
    {"EUV clock domain", {Action::ToggleEUV}},
    {"Save world", {Action::SaveWorld}},
    {"reload textures + shaders", {Action::ReloadResources}},
    {"script editor (debug)", {Action::ScriptEditor}},
    {"pause / release", {Action::Pause}},
};

const char* padButtonLabel(GamepadButton b) {
  switch (b) {
    case GamepadButton::LeftBumper: return "LB";
    case GamepadButton::RightBumper: return "RB";
    case GamepadButton::LeftThumb: return "L3";
    case GamepadButton::RightThumb: return "R3";
    case GamepadButton::DpadUp: return "Up";
    case GamepadButton::DpadRight: return "Right";
    case GamepadButton::DpadDown: return "Down";
    case GamepadButton::DpadLeft: return "Left";
    default: return nullptr;
  }
}

const char* padAxisLabel(GamepadAxis a) {
  switch (a) {
    case GamepadAxis::LeftX: case GamepadAxis::LeftY: return "LS";
    case GamepadAxis::RightX: case GamepadAxis::RightY: return "RS";
    case GamepadAxis::LeftTrigger: return "LT";
    default: return "RT";
  }
}

/// "a/b/c", or "first-last" for long runs such as 1-9.
std::string join(const std::vector<std::string>& names) {
  if (names.empty()) return "-";
  if (names.size() > 4) return names.front() + "-" + names.back();
  std::string s;
  for (const auto& n : names) s += (s.empty() ? "" : "/") + n;
  return s;
}

}  // namespace

std::string bindingLabel(const Binding& b) {
  using S = Binding::Source;
  switch (b.source) {
    case S::Key: {
      const Key k = Key(b.code);
      if (k >= Key::Num0 && k <= Key::Num9) return std::to_string(int(k) - int(Key::Num0));
      std::string n(inputName(k));
      if (n.starts_with("Left") && n.size() > 4 && n != "LeftBracket") n = "L" + n.substr(4);
      if (n.starts_with("Right") && n.size() > 5 && n != "RightBracket") n = "R" + n.substr(5);
      return n;
    }
    case S::MouseButton:
      switch (MouseButton(b.code)) {
        case MouseButton::Left: return "LMB";
        case MouseButton::Right: return "RMB";
        case MouseButton::Middle: return "MMB";
        case MouseButton::Back: return "MB4";
        default: return "MB5";
      }
    case S::WheelUp: return "WheelUp";
    case S::WheelDown: return "WheelDn";
    case S::MouseMotion: return "Mouse";
    case S::PadButton: {
      const GamepadButton pb = GamepadButton(b.code);
      const char* l = padButtonLabel(pb);
      return l ? l : std::string(inputName(pb));
    }
    case S::PadAxisPositive: case S::PadAxisNegative: return padAxisLabel(GamepadAxis(b.code));
    case S::PadStick: return Stick(b.code) == Stick::Left ? "LS" : "RS";
  }
  return "?";
}

std::vector<std::string> controlsPanelLines(const ActionMap& map) {
  std::vector<std::string> lines;
  char buf[96];
  std::snprintf(buf, sizeof buf, "%-16s %-9s CONTROLS (%s hide)", "KEYS / MOUSE", "PAD", controlsToggleLabel(map).c_str());
  lines.emplace_back(buf);
  for (const Row& row : kRows) {
    std::vector<std::string> keys, pad;
    for (const Action a : row.actions)
      for (const Binding& b : map.bindings(a)) {
        auto& list = b.isGamepad() ? pad : keys;
        if (std::string l = bindingLabel(b); std::find(list.begin(), list.end(), l) == list.end()) list.push_back(std::move(l));
      }
    std::snprintf(buf, sizeof buf, "%-16s %-9s %s", join(keys).c_str(), join(pad).c_str(), row.label);
    lines.emplace_back(buf);
  }
  lines.emplace_back("RMB on keyboard block: type (Esc ends)");
  return lines;
}

std::string controlsToggleLabel(const ActionMap& map) {
  for (const Binding& b : map.bindings(Action::ToggleControls))
    if (!b.isGamepad()) return bindingLabel(b);
  return "-";
}

}  // namespace lithokraft
