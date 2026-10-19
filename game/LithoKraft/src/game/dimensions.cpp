#include "lithokraft/game/dimensions.h"
#include <algorithm>
#include <memory>
#include <string>
#include "builtin_worldgen.h"
#include "lithokraft/content/appearance.h"
#include "lithokraft/content/blocks.h"
#include "litholattice/core/log.h"
#include "lithokraft/game/interchange.h"
#include "lithokraft/game/structures.h"
#include "lithokraft/game/wafer_biomes.h"
#include "litho_stamp_models.h"

namespace lithokraft {

namespace {

Spawn waferSetup(ChunkMap& w, int size) {
  const int o = std::min(40, size * 16 / 2 - 8);
  const Spawn sp = buildDemoCircuit(w, o, o);
  // Spawn garden (2026-10-13, JD directive): the two Litho Formula shrubs
  // grown from seeds 311 and 314 (JD's door number and his neighbour
  // Chris's -- the Ether's suggester at 314) flank the spawn on its clear
  // row: 311 three cells west, 314 three cells east. Stamped here, with
  // the demo circuit, so every fresh Wafer grows the same pair at the
  // same cells: the Wafer generator is seed-pinned and the spawn is a
  // pure function of world size, so placement is fully deterministic,
  // and once the world saves they persist (and evict/reload) like any
  // other placed block. Fail closed: a shrub is planted only on solid
  // ground under an air cell -- an occupied cell is left exactly as found.
  const int sx = int(sp.x), sz = int(sp.z);
  auto plant = [&w](int x, int z, BlockId b) {
    const int gy = w.surfaceY(x, z);
    if (gy < 0 || gy + 1 >= kWorldHeight) return;      // no column / no headroom
    if (w.get(x, gy, z) == kAir) return;               // nothing solid to stand on
    if (w.get(x, gy + 1, z) != kAir) return;           // occupied: never overwrite
    w.set(x, gy + 1, z, b);
  };
  plant(sx - 3, sz, kFormulaShrub311);
  plant(sx + 3, sz, kFormulaShrub314);
  // Interchanges (2026-10-14, JD directive): basic Wafer settlements on
  // the deterministic site lattice (see interchange.h). The seed follows
  // the Wafer terrain pin (0, see makeGenerator), so a Wafer is a pure
  // function of world size end to end. The home-reserve rule inside
  // placeInterchanges keeps size <= 8 worlds (the determinism scenario,
  // test worlds) completely untouched.
  placeInterchanges(w, uint64_t{0}, size);
  return sp;
}

Spawn organicSetup(ChunkMap& w, int size) {
  const int c = size * 16 / 2;
  const int h = std::max(0, w.surfaceY(c, c));
  // Feet on the ground (P2b-2, review G-M1): h is the top block, so the
  // standable surface is h+1 -- the old +12 dropped the player 11 blocks.
  return {float(c), float(h) + 1.f, float(c), 0.785f, -0.35f};
}

// B5 (2026-10-13): crust-level spawns for the two newly registered
// dimensions -- same feet-on-the-ground pattern as organicSetup. Cosmic's
// crust tops out inside the solid band (y < 256); the Empty above it is
// air today (no medium field exists in worldgen JSON -- see cosmic.json).
Spawn cosmicSetup(ChunkMap& w, int size) {
  const int c = size * 16 / 2;
  const int h = std::max(0, w.surfaceY(c, c));
  return {float(c), float(h) + 1.f, float(c), 0.785f, -0.35f};
}

Spawn underwaferSetup(ChunkMap& w, int size) {
  const int c = size * 16 / 2;
  const int h = std::max(0, w.surfaceY(c, c));
  return {float(c), float(h) + 1.f, float(c), 0.785f, -0.35f};
}

// Pack JSON when present and valid, else the built-in generator (which must always load).
// The Wafer additionally gets its code surface rules (dead-silicon remap, die-grid scribe
// lines, crystal accents): JSON material names resolve by texture key and cannot name the
// new biome blocks (40-48), so those rules place BlockIds directly (see wafer_biomes.h).
std::shared_ptr<const ColumnGenerator> makeGenerator(const WorldgenSettings& settings, const std::string& id) {
  std::string err;
  const auto extraSurface = [&]() {
    std::vector<std::unique_ptr<litholattice::procgen::ISurfaceRule>> rules;
    if (id == "wafer") rules = waferSurfaceRules();
    return rules;
  };
  // README design: "the wafer is seed-independent" -- pin the wafer generator to fixed world
  // seed 0 on BOTH the pack and built-in paths; other dimensions (organic) use settings.seed.
  const uint64_t seed = (id == "wafer") ? uint64_t{0} : settings.seed;
  if (const auto it = settings.generators.find(id); it != settings.generators.end()) {
    if (auto g = ColumnGenerator::create(it->second, seed, id, blockRegistry(), err, extraSurface(),
                                     model::makeLithoStampModelResolver()))
      return g;
    logError("worldgen: %s generator from packs is invalid (%s); using the built-in", id.c_str(), err.c_str());
  }
  const char* builtin = builtinWorldgenSource(id);
  auto g =
      builtin ? ColumnGenerator::create(builtin, seed, id, blockRegistry(), err, extraSurface(),
                                     model::makeLithoStampModelResolver()) : nullptr;
  if (!g) logError("worldgen: built-in %s generator failed: %s", id.c_str(), err.c_str());
  return g;
}

}  // namespace

const char* builtinWorldgenSource(std::string_view dimensionId) {
  for (const BuiltinWorldgenFile& f : kBuiltinWorldgenFiles)
    if (std::string_view(f.name) == std::string(dimensionId) + ".json") return f.source;
  return nullptr;
}

DimensionRegistry createDimensions(const WorldgenSettings& settings) {
  // Registered: wafer, organic (+ overworld alias), cosmic, underwafer
  // (B5 2026-10-13). Cosmic and Underwafer are registered with honest
  // CRUST-LEVEL content only (see their assets/worldgen JSONs): each has
  // a real generator, terrain and a spawn, but the design content that
  // makes them themselves does NOT exist yet and is not faked:
  //  - "cosmic": the Empty as a named medium (worldgen JSON has no
  //    medium field -- air stands in above the crust), Recons / Lattice
  //    Bridge satellites, starship traversal, free descent into Organic,
  //    Hypervisor gating behind the Underwafer/Sear.
  //  - "underwafer": the Sear, hypervisor ore, a ceiling structure, and
  //    the Wafer-bottom portal link.
  // Still deliberately NOT registered:
  //  - "void": its locked extent is INFINITE and no packPos key can
  //    represent that (see dimensions.h + the B5 representability
  //    analysis); infohazard mechanics and any content are also missing.
  //    `map void` honestly answers "unknown dimension" until JD decides
  //    a finite bound or a virtualised coordinate scheme.
  DimensionRegistry r;
  auto wafer = std::make_unique<Dimension>();
  wafer->id = "wafer";
  wafer->name = "Wafer";
  wafer->biomes = {"trace_forest", "dicing_plains", "capacitor_groves", "resonator_spires", "dead_zones"};
  wafer->generator = makeGenerator(settings, wafer->id); wafer->setup = waferSetup; wafer->belowWorldFill = kStone;
  wafer->chunks.setRegistry(&blockRegistry());  // Infinite Registry: ChunkMap translates via this
  r.add(std::move(wafer));
  auto ow = std::make_unique<Dimension>();
  ow->id = "organic"; ow->name = "Organic"; ow->biomes = {"plains", "hills"};  // locked one-name convention (was "Organic World")
  ow->generator = makeGenerator(settings, ow->id); ow->setup = organicSetup; ow->belowWorldFill = kStone;
  ow->chunks.setRegistry(&blockRegistry());
  r.add(std::move(ow));
  r.addAlias("overworld", "organic");  // the old name still works
  auto uw = std::make_unique<Dimension>();
  uw->id = "underwafer"; uw->name = "Underwafer"; uw->biomes = {"deep_stone"};
  uw->generator = makeGenerator(settings, uw->id); uw->setup = underwaferSetup; uw->belowWorldFill = kStone;
  uw->chunks.setRegistry(&blockRegistry());
  r.add(std::move(uw));
  auto cos = std::make_unique<Dimension>();
  cos->id = "cosmic"; cos->name = "Cosmic"; cos->biomes = {"empty_crust"};
  cos->generator = makeGenerator(settings, cos->id); cos->setup = cosmicSetup; cos->belowWorldFill = kStone;
  cos->chunks.setRegistry(&blockRegistry());
  r.add(std::move(cos));
  return r;
}

}  // namespace lithokraft
