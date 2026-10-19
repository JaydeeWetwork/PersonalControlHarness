// Wafer ground traces: compilation-firewall translation unit.
// The whole system is header-only (like render/ground_pulse.h); this TU exists
// so the header is proven self-contained - it must compile standalone with no
// other engine headers included first. LF line endings.
#include "litholattice/render/ground_traces.h"

// The trace system reuses the pulse's biome contract: same ids, same count.
static_assert(litholattice::kGroundPulseBiomeCount == 6,
              "ground traces: biome count must match the pulse's contract");
// Every tuning row must stay in shader-safe ranges (tint <= 1 keeps the
// emissive bounded by amplitude; density/speed/amplitude/dirMix >= 0).
static_assert([] {
  for (uint8_t i = 0; i < litholattice::kGroundPulseBiomeCount; ++i) {
    const litholattice::GroundTraceParams p =
        litholattice::groundTraceParams(litholattice::GroundPulseBiome(i));
    if (p.density < 0.f || p.speed < 0.f || p.amplitude < 0.f) return false;
    if (p.dirMix < 0.f || p.dirMix > 1.f) return false;
    if (p.tintR < 0.f || p.tintR > 1.f || p.tintG < 0.f || p.tintG > 1.f ||
        p.tintB < 0.f || p.tintB > 1.f)
      return false;
  }
  return true;
}(), "ground traces: tuning table out of shader-safe ranges");
