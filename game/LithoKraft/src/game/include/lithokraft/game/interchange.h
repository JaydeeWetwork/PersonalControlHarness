// Interchanges: basic Wafer settlements (JD directive, 2026-10-14).
//
// JD: "give us some villages or interchanges, make them basic, maybe kind
// of copy some of Minecraft's stuff... I know that we don't have anything
// to trade but it'll give the Wafer a little bit more life — you can even
// find an interchange and just look at it." Visual life first, trade
// later; JD designs further FROM SEEING them, and will hand-build
// showcase interchanges himself. This generator is the basic procedural
// baseline, not the final art.
//
// SHAPE (Minecraft villages as loose inspiration, LithoKraft's own):
// a levelled die-pad on the Wafer surface, a scribed plaza with a lamp
// feature at the centre, slab-free paved paths along the four arms, and
// a ring of simple buildings built ONLY from already-registered blocks:
// houses (two material variants), trade stalls, a workshop hut with a
// dead memory-bank, and path lamp posts.
//
// PLACEMENT: a pure function of (world seed, site region). Regions are
// 128x128 blocks; each region rolls presence / centre jitter / rotation
// / population mix off a detMix64 stream, so the same world always grows
// the same interchanges. The Wafer generator is seed-pinned to 0 (see
// dimensions.cpp makeGenerator), and waferSetup passes that same pin:
// the whole Wafer stays a pure function of world size.
//
// RESERVE: no interchange footprint may intersect the home reserve
// around the world origin — the demo circuit, the spawn garden and the
// player's first steps live there, and so do the determinism scenario
// (world size 8) and every small test world. Interchanges therefore
// NEVER appear inside a 128x128 world, by rule, not by luck: the demo
// scenario hash cannot move. In the default game world (16 chunks =
// 256 blocks) the regions east/south/south-east of the reserve can and
// do hold sites.
//
// POPULATION (manifest, not mobs — yet): Receiptors (RC) have no mob
// implementation yet, Netors none, and Deceitor natural spawning awaits
// structure tagging (see deceitor.h). So each plan also computes the
// intended population as DATA — slots with kind / role / feet position /
// facing per building, counts set by the site's mix (RC-heavy with a
// few DCs, or DC-heavy with a few RCs, plus Netors tidying — JD's
// both-ways mixture rule, 2026-10-14). The plan is re-derivable from
// the seed at any time, so the manifest needs no save format: placed
// blocks persist like any other block, and the population layer (AiHost
// world + DeceitorDirector::spawn for DC slots, a future Receiptor
// director for RC/Netor slots) consumes planInterchange() when it
// lands. interchangeManifestJson() is the written form of that data.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "litholattice/world/chunk_map.h"

namespace lithokraft {

// ---------------------------------------------------------------- tuning
// Every value here is a JD-TUNABLE placeholder (2026-10-14), not a lock.
struct InterchangeTuning {
  static constexpr int kRegionSize = 128;      ///< site lattice cell (blocks)
  static constexpr int kPresenceOutOf8 = 6;    ///< regions with a site: 6/8 rolls
  static constexpr int kPadRadius = 24;        ///< levelled die-pad disc radius
  static constexpr int kFootprintRadius = 28;  ///< pad + margin: reserve tests use this
  static constexpr int kPlazaRadius = 7;       ///< scribed plaza disc radius
  // Home reserve: footprints must stay clear of [kReserveMin, kReserveMax)^2.
  static constexpr int kReserveMin = -32;
  static constexpr int kReserveMax = 152;
  // Population mixes (JD mixture rule: both ways, never pure-guaranteed).
  static constexpr int kRcHeavyRc = 6, kRcHeavyDc = 2, kRcHeavyNetor = 2;
  static constexpr int kDcHeavyRc = 2, kDcHeavyDc = 6, kDcHeavyNetor = 1;
};

enum class PopKind : uint8_t { Receiptor, Deceitor, Netor };
enum class InterchangeMix : uint8_t { ReceiptorHeavy, DeceitorHeavy };

/// One intended inhabitant: feet cell (y resolved against the built
/// world by buildInterchange; in a bare plan y is pad-relative feet,
/// i.e. padY + 1, with padY unknown — see BuiltInterchange).
struct PopSlot {
  PopKind kind = PopKind::Receiptor;
  int x = 0, y = 0, z = 0;
  float yaw = 0.f;        ///< facing (radians, engine Spawn convention)
  const char* role = "";  ///< "resident" / "trader" / "keeper" / "lurker" / "tidier"
  int building = -1;      ///< index into InterchangePlan::buildings (-1 = plaza)
};

struct BuildingPlan {
  const char* kind = "";  ///< "house_a" / "house_b" / "stall" / "workshop"
  int cx = 0, cz = 0;     ///< world centre of the footprint
  int rot = 0;            ///< 0..3: local -Z front rotated by rot * 90 degrees
  int halfW = 0, halfD = 0;  ///< footprint half extents (blocks)
};

/// The full deterministic plan for one region. Pure function of
/// (worldSeed, regionX, regionZ) — terrain is NOT consulted, so a plan
/// can be computed before/without any world (manifests, tests, the
/// future population layer). `padY` is only known after building.
struct InterchangePlan {
  bool present = false;
  int regionX = 0, regionZ = 0;
  int centreX = 0, centreZ = 0;
  int rotation = 0;  ///< site rotation 0..3 applied to the building ring
  InterchangeMix mix = InterchangeMix::ReceiptorHeavy;
  std::vector<BuildingPlan> buildings;
  std::vector<PopSlot> population;  ///< y is pad-relative feet (padY + 1)
  int countReceiptor = 0, countDeceitor = 0, countNetor = 0;
};

/// Plan one region. present=false when the region rolls no site or its
/// footprint would intersect the home reserve.
InterchangePlan planInterchange(uint64_t worldSeed, int regionX, int regionZ);

/// All present plans whose footprint intersects block rect
/// [minX, maxX) x [minZ, maxZ), in region order (deterministic).
std::vector<InterchangePlan> planInterchangesIn(uint64_t worldSeed, int minX, int minZ, int maxX,
                                                int maxZ);

/// The result of building: the plan plus terrain-resolved data.
struct BuiltInterchange {
  InterchangePlan plan;
  int padY = -1;                       ///< levelled pad top height (world y)
  std::vector<PopSlot> population;     ///< plan slots with world-y feet
  int blocksPlaced = 0;
};

/// Stamp one interchange into `w` (blocks only where columns already
/// exist — ChunkMap::set clips). Computes padY from the centre column,
/// levels the pad, paves plaza + paths, builds the ring. Returns
/// blocks placed (0 when the centre column is ungenerated).
int buildInterchange(litholattice::ChunkMap& w, const InterchangePlan& plan,
                     BuiltInterchange* out = nullptr);

/// waferSetup hook: build every planned interchange whose footprint is
/// fully inside the loaded world [0, sizeChunks*16)^2. Returns the
/// number of interchanges built. With sizeChunks <= 8 the reserve rule
/// guarantees this is 0 (scenario/test worlds stay untouched).
int placeInterchanges(litholattice::ChunkMap& w, uint64_t worldSeed, int sizeChunks,
                      std::vector<BuiltInterchange>* built = nullptr);

/// Written population/structure manifest for one built interchange
/// (hand-rolled JSON, stable field order — the same text the selftest
/// pins for determinism, and the capture tool writes next to its PNGs).
std::string interchangeManifestJson(const BuiltInterchange& built);

/// Self-test (app selftest section): plan determinism, reserve/scenario
/// safety, build on a real generated wafer, population mix counts.
/// Returns failures.
int runInterchangeSelfTest();

}  // namespace lithokraft
