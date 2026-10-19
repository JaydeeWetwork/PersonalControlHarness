// Interchange self-test (2026-10-14): plan determinism, the home-reserve
// rule (scenario-sized worlds stay untouched — the hash-safety property),
// building on a real generated Wafer, build determinism across two fresh
// worlds, and population mix counts. See interchange.h for the design.
#include <cmath>
#include <cstdio>

#include "lithokraft/content/blocks.h"
#include "lithokraft/game/dimensions.h"
#include "lithokraft/game/interchange.h"
#include "litholattice/core/threading.h"
#include "litholattice/world/dimension.h"

namespace lithokraft {
namespace {

int gFails = 0;
void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("  [FAIL] interchange: %s\n", what);
    ++gFails;
  }
}

// Cheap full-map fingerprint over the loaded columns (get() everywhere):
// proves "untouched" for small worlds and "identical" for built ones.
uint64_t fingerprint(const litholattice::ChunkMap& w, int extent) {
  uint64_t h = 1469598103934665603ULL;
  for (int z = 0; z < extent; ++z)
    for (int x = 0; x < extent; ++x) {
      const int sy = w.surfaceY(x, z);
      for (int y = 0; y <= sy + 2 && y < litholattice::kWorldHeight; ++y) {
        h ^= uint64_t(w.get(x, y, z)) + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
      }
    }
  return h;
}

bool samePlan(const InterchangePlan& a, const InterchangePlan& b) {
  if (a.present != b.present || a.centreX != b.centreX || a.centreZ != b.centreZ ||
      a.rotation != b.rotation || a.mix != b.mix ||
      a.countReceiptor != b.countReceiptor || a.countDeceitor != b.countDeceitor ||
      a.countNetor != b.countNetor || a.buildings.size() != b.buildings.size() ||
      a.population.size() != b.population.size())
    return false;
  for (size_t i = 0; i < a.buildings.size(); ++i)
    if (a.buildings[i].cx != b.buildings[i].cx || a.buildings[i].cz != b.buildings[i].cz ||
        a.buildings[i].rot != b.buildings[i].rot)
      return false;
  for (size_t i = 0; i < a.population.size(); ++i)
    if (a.population[i].kind != b.population[i].kind || a.population[i].x != b.population[i].x ||
        a.population[i].z != b.population[i].z)
      return false;
  return true;
}

}  // namespace

int runInterchangeSelfTest() {
  gFails = 0;
  using T = InterchangeTuning;

  // 1) Plan determinism + mix counts over a spread of regions.
  int present = 0;
  for (int rz = 0; rz < 4; ++rz)
    for (int rx = 0; rx < 4; ++rx) {
      const InterchangePlan a = planInterchange(0, rx, rz);
      const InterchangePlan b = planInterchange(0, rx, rz);
      check(samePlan(a, b), "planInterchange is deterministic per (seed, region)");
      if (!a.present) continue;
      ++present;
      const int wantRc = a.mix == InterchangeMix::ReceiptorHeavy ? T::kRcHeavyRc : T::kDcHeavyRc;
      const int wantDc = a.mix == InterchangeMix::ReceiptorHeavy ? T::kRcHeavyDc : T::kDcHeavyDc;
      const int wantNt = a.mix == InterchangeMix::ReceiptorHeavy ? T::kRcHeavyNetor : T::kDcHeavyNetor;
      int rc = 0, dc = 0, nt = 0;
      for (const PopSlot& s : a.population) {
        if (s.kind == PopKind::Receiptor) ++rc;
        else if (s.kind == PopKind::Deceitor) ++dc;
        else ++nt;
      }
      check(rc == wantRc && dc == wantDc && nt == wantNt, "population counts match the mix tuning");
      check(int(a.population.size()) == wantRc + wantDc + wantNt, "population size = mix total");
      check(a.buildings.size() == 6, "ring has six buildings");
    }
  check(present >= 4, "a 4x4 region spread holds several interchanges");

  // 2) Reserve rule: NO plan's footprint touches the scenario rect
  // [0,128)^2 — the exact property that keeps the scenario hash still.
  check(planInterchangesIn(0, 0, 0, 128, 128).empty(),
        "no interchange footprint intersects the 128x128 scenario/home rect");
  check(!planInterchangesIn(0, -200, -200, 200, 200).empty() ||
            !planInterchangesIn(0, 0, 0, 600, 600).empty(),
        "rect queries over larger areas find interchanges");

  // 3) Small worlds stay untouched: waferSetup already ran
  // placeInterchanges during load(8); running it again must build 0 and
  // change nothing (and likewise at size 6, the app selftest's world).
  {
    JobPool pool(2);
    DimensionRegistry reg = createDimensions();
    Dimension* wafer = reg.find("wafer");
    check(wafer != nullptr, "wafer dimension exists");
    wafer->load(pool, 8);
    const uint64_t before = fingerprint(wafer->chunks, 128);
    const int built = placeInterchanges(wafer->chunks, 0, 8);
    check(built == 0, "size-8 world (the scenario's) grows no interchange");
    check(fingerprint(wafer->chunks, 128) == before, "size-8 world is byte-identical afterwards");
    wafer->unload();
    wafer->load(pool, 6);
    check(placeInterchanges(wafer->chunks, 0, 6) == 0, "size-6 world grows no interchange");
    wafer->unload();
  }

  // 4) A real 24-chunk Wafer (384x384) grows at least one interchange
  // via the production setup path, with the expected blocks in place.
  {
    JobPool pool(2);
    DimensionRegistry reg = createDimensions();
    Dimension* wafer = reg.find("wafer");
    wafer->load(pool, 24);
    // Plans fully inside the world, same filter placeInterchanges uses.
    InterchangePlan site;
    bool found = false;
    for (const InterchangePlan& p : planInterchangesIn(0, 0, 0, 384, 384)) {
      if (p.centreX - T::kPadRadius - 4 >= 0 && p.centreZ - T::kPadRadius - 4 >= 0 &&
          p.centreX + T::kPadRadius + 4 < 384 && p.centreZ + T::kPadRadius + 4 < 384) {
        site = p;
        found = true;
        break;
      }
    }
    check(found, "24-chunk wafer holds at least one buildable interchange site");
    if (found) {
      litholattice::ChunkMap& w = wafer->chunks;
      // padY measured off the plaza paving (nothing stands on (5, 0)).
      const int padY = w.surfaceY(site.centreX + 5, site.centreZ);
      check(padY > 0, "plaza paving found at surface");
      check(w.get(site.centreX + 5, padY, site.centreZ) == kScribeSilicon,
            "plaza top is scribe silicon");
      check(w.get(site.centreX, padY, site.centreZ) == kDenseSilicon,
            "centre feature base is dense silicon");
      check(w.get(site.centreX, padY + 1, site.centreZ) == kTraceTrunk &&
                w.get(site.centreX, padY + 4, site.centreZ) == kLamp,
            "centre lamp pillar: trunk + lamp");
      int scribe = 0, glass = 0, lamps = 0;
      for (int dz = -T::kFootprintRadius; dz <= T::kFootprintRadius; ++dz)
        for (int dx = -T::kFootprintRadius; dx <= T::kFootprintRadius; ++dx)
          for (int y = std::max(0, padY - 2); y <= padY + 6; ++y) {
            const BlockId b = w.get(site.centreX + dx, y, site.centreZ + dz);
            if (b == kScribeSilicon) ++scribe;
            else if (b == kGlass || b == kFrostedGlass) ++glass;
            else if (b == kLamp) ++lamps;
          }
      check(scribe >= 60, "plaza disc is paved in scribe silicon");
      check(glass >= 8, "buildings have glass windows");
      check(lamps >= 5, "plaza + corner lamps are placed");

      // Build determinism: a second fresh world grows the identical site.
      DimensionRegistry reg2 = createDimensions();
      Dimension* wafer2 = reg2.find("wafer");
      wafer2->load(pool, 24);
      uint64_t h1 = 1469598103934665603ULL, h2 = h1;
      auto mixInto = [](uint64_t h, uint64_t v) {
        return h ^ (v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2));
      };
      for (int dz = -T::kFootprintRadius; dz <= T::kFootprintRadius; ++dz)
        for (int dx = -T::kFootprintRadius; dx <= T::kFootprintRadius; ++dx)
          for (int y = std::max(0, padY - 2); y <= padY + 6; ++y) {
            h1 = mixInto(h1, uint64_t(w.get(site.centreX + dx, y, site.centreZ + dz)));
            h2 = mixInto(h2, uint64_t(wafer2->chunks.get(site.centreX + dx, y, site.centreZ + dz)));
          }
      check(h1 == h2, "two fresh wafers grow byte-identical interchanges");
      wafer2->unload();
    }
    wafer->unload();
  }

  if (gFails == 0) std::printf("  interchange: OK\n");
  return gFails;
}

}  // namespace lithokraft
