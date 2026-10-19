// Standard stats: the small set of stats every game gets from the engine,
// registered on IStat (NOT IVitals — these are tracked numbers, not
// condition pools; XP is the canonical case). Games register anything
// beyond this themselves (LithoKraft's Kharge is the first).
#pragma once

#include <cstdint>

#include "litholattice/player/stat.h"

namespace litholattice {

/// Stat name of the standard experience stat (stable: saves and netcode
/// key on the registered id handed out for this name).
inline constexpr const char* kXpStatName = "xp";

struct StandardStats {
  uint32_t xp = 0;
};

/// Register the standard stats on a stat registry:
///   xp 0..INT32_MAX, default 0 — accumulated experience. A stat, not a
///       vital: it is never drained by survival systems, has no resting
///       pool semantics, and only moves when progression grants or spends
///       it. Levels (when a game wants them) derive from xp; they are not
///       stored alongside it here.
/// Idempotent: re-registering returns the existing id (IStat is
/// first-wins; a game that pre-registered its own "xp" keeps its def).
StandardStats registerStandardStats(IStat& stats);

}  // namespace litholattice
