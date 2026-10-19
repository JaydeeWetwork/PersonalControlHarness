// Standard vitals: the small set of vitals every game gets from the engine.
// All on the engine's 64 scale (matching PlayerState::health). These are
// the engine's seed registrations on IVitals — games and mods register
// anything beyond this themselves, through the same IVitals interface
// (hunger, a dimension's exposure pool, ...). LithoKraft's Kharge, by
// contrast, is a stat, not a vital: it lives on IStat.
#pragma once

#include <cstdint>

#include "litholattice/player/vital.h"

namespace litholattice {

/// Vital names of the standard vitals (stable: saves and netcode key on the
/// registered ids handed out for these names).
inline constexpr const char* kHealthVitalName = "health";
inline constexpr const char* kOxygenVitalName = "oxygen";
inline constexpr const char* kStaminaVitalName = "stamina";

struct StandardVitals {
  uint32_t health = 0;
  uint32_t oxygen = 0;
  uint32_t stamina = 0;
};

/// Register the standard vitals on a vital registry:
///   health  0..64, default 64 — 0 means dead (IPlayer::alive derives from it)
///   oxygen  0..64, default 64 — the vital the game drains while the player
///           is under liquids; at 0 the game starts draining health
///   stamina 0..64, default 64 — exertion pool for sprinting and the like
/// Idempotent: re-registering returns the existing ids (IVitals is
/// first-wins; a game that pre-registered its own "health" keeps its def).
StandardVitals registerStandardVitals(IVitals& vitals);

}  // namespace litholattice
