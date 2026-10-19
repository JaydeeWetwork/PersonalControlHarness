// McpPlayerController / McpPlayerView: game-side adapters for the MCP
// player-control seam (litholattice::mcp::IPlayerController / IPlayerView).
//
// The controller translates a game-agnostic ControlFrame into synthetic
// InputEvents using the CURRENT ActionMap (user remaps respected) and
// stages them into the InputSystem, where they merge with the backend's
// events at the next update() — the exact point keyboard input enters.
// The model therefore plays by the same rules as a keyboard player: the
// same physics in Player::update, the same reach/interact checks in
// Game::handleWorldActions, the same sim-thread marshaling for world edits.
//
// MULTI-PILOT (locked 2026-10-11): shadow input state is per pilot id.
// v1 wires exactly one pilot ("player" = the local player); adding pilot #2
// is registering another id here — no MCP module changes.
//
// Threading: submitFrame()/releaseAll()/pilotIds() run on the MCP server
// thread. All shared state is mutex-guarded; staging into the InputSystem
// is thread-safe by engine contract.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "litholattice/input/input_system.h"
#include "litholattice/mcp/server.h"
#include "lithokraft/game/actions.h"

namespace lithokraft {

class McpPlayerController : public litholattice::mcp::IPlayerController {
 public:
  /// `pilotIds`: ids to wire (v1: {"player"}). `controllable`: true when the
  /// local player exists to drive (Playing state, not paused, no inventory
  /// open). Called on the MCP thread.
  McpPlayerController(litholattice::InputSystem& input,
                      std::vector<std::string> pilotIds,
                      std::function<bool()> controllable);
  std::vector<std::string> pilotIds() const override;
  bool submitFrame(std::string_view pilot, const ControlFrame& f) override;
  void releaseAll(std::string_view pilot) override;
  /// Diagnostic: synthetic components dropped because the action has no
  /// Key binding in the current map.
  uint64_t droppedUnbound() const;

 private:
  struct PilotState {
    bool heldFwd = false, heldBack = false;
    bool heldLeft = false, heldRight = false;
    bool heldJump = false, heldSneak = false, heldSprint = false;
  };
  // Find the first Key binding for a game action, or Key::Count when unbound.
  litholattice::Key keyFor(Action a) const;
  void setHeld(Action a, bool held, bool& shadow,
               std::vector<litholattice::InputEvent>& out);
  void pressOnce(Action a, std::vector<litholattice::InputEvent>& out);

  litholattice::InputSystem& input_;
  std::vector<std::string> pilotIds_;
  std::function<bool()> controllable_;
  mutable std::mutex mutex_;
  std::map<std::string, PilotState, std::less<>> pilots_;
  uint64_t droppedUnbound_ = 0;
};

/// Game-side IPlayerView adapter: fills the snapshot from the live game.
/// Constructed with accessors so the game owns the data.
class McpPlayerView : public litholattice::mcp::IPlayerView {
 public:
  struct GameState {
    double x = 0, y = 0, z = 0;
    double vx = 0, vy = 0, vz = 0;  ///< true velocity (blocks/s), via GamePlayerAdapter
    float yaw = 0, pitch = 0;
    uint8_t dimension = 1;
    uint8_t health = 64;  ///< effective health stat (IPlayer integration)
    /// Named stats (name, effective value) from the engine stat registry,
    /// via GamePlayerAdapter::namedStats().
    std::vector<std::pair<std::string, double>> stats;
    std::vector<uint16_t> hotbar;
    int selectedSlot = 0;
  };
  explicit McpPlayerView(std::function<GameState()> snapshot);
  Snapshot localPlayer() override;

 private:
  std::function<GameState()> snapshot_;
};

/// Provisional string->uint8 mapping for the MCP snapshot dimension field
/// (1 = Wafer). Goes away when the engine gets canonical numeric ids.
uint8_t mcpDimensionNumber(const std::string& id);

}  // namespace lithokraft
