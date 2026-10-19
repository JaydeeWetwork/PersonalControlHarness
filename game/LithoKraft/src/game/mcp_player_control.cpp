// McpPlayerController / McpPlayerView: game-side MCP player-control adapters.
#include "lithokraft/game/mcp_player_control.h"

#include <algorithm>
#include <cmath>

namespace lithokraft {
namespace {

using litholattice::Binding;
using litholattice::InputEvent;
using litholattice::Key;

uint8_t dimensionNumber(const std::string& id) {
  if (id == "wafer") return 1;
  if (id == "underwafer") return 2;
  if (id == "organic") return 3;
  if (id == "cosmic") return 4;
  if (id == "void") return 5;
  if (id == "ether") return 6;
  if (id == "undefined") return 7;
  return 0;
}

}  // namespace

McpPlayerController::McpPlayerController(litholattice::InputSystem& input,
                                         std::vector<std::string> pilotIds,
                                         std::function<bool()> controllable)
    : input_(input), pilotIds_(std::move(pilotIds)), controllable_(std::move(controllable)) {
  for (const auto& id : pilotIds_) pilots_.emplace(id, PilotState{});
}

std::vector<std::string> McpPlayerController::pilotIds() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return pilotIds_;
}

Key McpPlayerController::keyFor(Action a) const {
  for (const Binding& b : input_.map().bindings(a)) {
    if (b.source == Binding::Source::Key) return Key(b.code);
  }
  return Key::Count;
}

void McpPlayerController::setHeld(Action a, bool held, bool& shadow,
                                  std::vector<InputEvent>& out) {
  if (held == shadow) return;
  const Key k = keyFor(a);
  if (k == Key::Count) {
    ++droppedUnbound_;
    shadow = held;  // don't spam the counter every frame
    return;
  }
  out.push_back(InputEvent::key(held ? InputEvent::Type::KeyDown : InputEvent::Type::KeyUp,
                               k, 0.0));
  shadow = held;
}

void McpPlayerController::pressOnce(Action a, std::vector<InputEvent>& out) {
  const Key k = keyFor(a);
  if (k == Key::Count) {
    ++droppedUnbound_;
    return;
  }
  // Down+up in one batch: pressed() is true for exactly this frame
  // (InputSystem::resolveButton: edgeDown feeds s.pressed).
  out.push_back(InputEvent::key(InputEvent::Type::KeyDown, k, 0.0));
  out.push_back(InputEvent::key(InputEvent::Type::KeyUp, k, 0.0));
}

bool McpPlayerController::submitFrame(std::string_view pilot, const ControlFrame& f) {
  if (controllable_ && !controllable_()) return false;
  std::vector<InputEvent> ev;
  ev.reserve(16);
  {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = pilots_.find(pilot);
    if (it == pilots_.end()) return false;  // unknown pilot (tool layer rejects first)
    PilotState& st = it->second;
    // Held movement: diff against this pilot's shadow state -> KeyDown/KeyUp.
    setHeld(Action::MoveForward, f.moveForward > 0.05f, st.heldFwd, ev);
    setHeld(Action::MoveBack, f.moveForward < -0.05f, st.heldBack, ev);
    setHeld(Action::MoveRight, f.moveStrafe > 0.05f, st.heldRight, ev);
    setHeld(Action::MoveLeft, f.moveStrafe < -0.05f, st.heldLeft, ev);
    setHeld(Action::Jump, f.jump, st.heldJump, ev);
    setHeld(Action::Sneak, f.sneak, st.heldSneak, ev);
    setHeld(Action::Sprint, f.sprint, st.heldSprint, ev);
    // Look: relative mouse motion, same path as the mouse.
    if (f.lookDx != 0.f || f.lookDy != 0.f)
      ev.push_back(InputEvent::motion(f.lookDx, f.lookDy, 0.0));
    // Edge actions: exactly one frame.
    if (f.usePressed) pressOnce(Action::Use, ev);
    if (f.attackPressed) pressOnce(Action::Break, ev);
    if (f.placePressed) pressOnce(Action::Place, ev);
    if (f.dropPressed) pressOnce(Action::Drop, ev);
    if (f.hotbarSlot >= 0 && f.hotbarSlot < 8)
      pressOnce(hotbarAction(f.hotbarSlot), ev);
  }
  if (!ev.empty()) input_.stageSyntheticEvents(std::move(ev));
  return true;
}

void McpPlayerController::releaseAll(std::string_view pilot) {
  std::vector<InputEvent> ev;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = pilots_.find(pilot);
    if (it == pilots_.end()) return;
    PilotState& st = it->second;
    setHeld(Action::MoveForward, false, st.heldFwd, ev);
    setHeld(Action::MoveBack, false, st.heldBack, ev);
    setHeld(Action::MoveRight, false, st.heldRight, ev);
    setHeld(Action::MoveLeft, false, st.heldLeft, ev);
    setHeld(Action::Jump, false, st.heldJump, ev);
    setHeld(Action::Sneak, false, st.heldSneak, ev);
    setHeld(Action::Sprint, false, st.heldSprint, ev);
  }
  if (!ev.empty()) input_.stageSyntheticEvents(std::move(ev));
}

uint64_t McpPlayerController::droppedUnbound() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return droppedUnbound_;
}

McpPlayerView::McpPlayerView(std::function<GameState()> snapshot)
    : snapshot_(std::move(snapshot)) {}

McpPlayerView::Snapshot McpPlayerView::localPlayer() {
  GameState g = snapshot_();
  Snapshot s;
  s.x = g.x; s.y = g.y; s.z = g.z;
  s.vx = g.vx; s.vy = g.vy; s.vz = g.vz;  // derived from applied movement by GamePlayerAdapter
  s.yaw = g.yaw; s.pitch = g.pitch;
  s.dimension = g.dimension;
  s.health = g.health;  // the real health stat, via GamePlayerAdapter
  s.stats = g.stats;
  s.hotbar = std::move(g.hotbar);
  s.selectedSlot = g.selectedSlot;
  return s;
}

uint8_t mcpDimensionNumber(const std::string& id) { return dimensionNumber(id); }

}  // namespace lithokraft
