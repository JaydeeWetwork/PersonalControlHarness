// SettingsCvars: bridges GameSettings (the JSON-backed settings file) to live
// lithoscript cvars. Each cvar get/sets the ACTUAL engine state, not a copy.
//
// Naming: the dotted subsystem hierarchy (sim., client., net., render.,
// audio., input., hud., phys., game., debug., mod.<id>.) is the LOCKED
// convention (JD 2026-10-11). These registrations live in ONE table below so
// any future rename is mechanical. This is the first-party population of the
// general console language; the language itself stays complete and general in
// the console module (language/population split, JD-locked).
//
// Sync design: cvar -> setting flows through GameSettings::onSettingsChanged
// (the apply path), so a console set behaves exactly like a settings-file
// load. Setting -> cvar flows through syncFrom(), called by Game after
// onSettingsChanged runs (e.g. after the settings UI edits the file).
// A syncing_ guard prevents the cvar->setting->cvar echo from recursing.
#pragma once

#include <functional>
#include <string>

#include "lithokraft/game/settings.h"

namespace litholattice::console {
class ConsoleRegistry;
}  // namespace litholattice::console

namespace lithokraft {

/// MCP pilot-senses cvars (mcp.vision_width, mcp.vision_height,
/// mcp.audio_tier, camera.chunk_radius). MCP-host knobs, not player
/// preferences, so they are not part of GameSettings; registered alongside
/// the settings cvars and archived to config.lth.
void registerMcpSensesCvars(litholattice::console::ConsoleRegistry& r);

/// AiWorld host cvar (ai.enabled). A host knob like the MCP senses knobs
/// above — not a player preference, so not part of GameSettings. Bool,
/// default 0 (OFF): the production AiWorld is constructed only when the
/// player/host turns it on (brief B6 wiring, 2026-10-13). Archived.
void registerAiCvars(litholattice::console::ConsoleRegistry& r);

class SettingsCvars {
 public:
  /// `apply` must behave like a settings-file load: apply `s` to the engine,
  /// then call GameSettings::onSettingsChanged (Game::onSettingsChanged).
  /// `read` returns the current settings for cvar default registration.
  SettingsCvars(std::function<void(const GameSettings&)> apply,
                std::function<GameSettings()> read);

  /// Registers all cvars on `r` with their current values as defaults.
  /// Archived cvars persist to config.lth via saveArchived().
  void registerOn(litholattice::console::ConsoleRegistry& r);

  /// Pushes current settings values into the cvars (call after UI edits).
  void syncFrom(const GameSettings& s);

 private:  std::function<void(const GameSettings&)> apply_;
  std::function<GameSettings()> read_;
  bool syncing_ = false;
  litholattice::console::ConsoleRegistry* registry_ = nullptr;
};

}  // namespace lithokraft
