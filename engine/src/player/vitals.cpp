#include "litholattice/player/vitals.h"

namespace litholattice {

StandardVitals registerStandardVitals(IVitals& vitals) {
  StandardVitals v;
  // First-wins registration makes this safe to call more than once (and on
  // a registry where a game pre-registered its own "health" — theirs wins).
  v.health = vitals.registerVital(VitalDef{0, kHealthVitalName, 0, 64, 64});
  v.oxygen = vitals.registerVital(VitalDef{0, kOxygenVitalName, 0, 64, 64});
  v.stamina = vitals.registerVital(VitalDef{0, kStaminaVitalName, 0, 64, 64});
  return v;
}

}  // namespace litholattice
