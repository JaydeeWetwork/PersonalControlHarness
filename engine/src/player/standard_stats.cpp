#include "litholattice/player/standard_stats.h"

#include <cstdint>

namespace litholattice {

StandardStats registerStandardStats(IStat& stats) {
  StandardStats s;
  // First-wins registration makes this safe to call more than once (and on
  // a registry where a game pre-registered its own "xp" — theirs wins).
  // Max is the int32 ceiling: XP accumulates for the life of a world and
  // must never clamp against the 64-scale used by pools and vitals.
  s.xp = stats.registerStat(StatDef{0, kXpStatName, 0, INT32_MAX, 0});
  return s;
}

}  // namespace litholattice
