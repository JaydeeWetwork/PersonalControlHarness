// Headless checks of LithoKraft content that sits on engine interfaces: sound-effect recipes on
// the engine synth, the block appearance through the engine mesher, the registry layout.
#include <cmath>
#include <cstdio>
#include <vector>
#include "lithokraft/content/appearance.h"
#include "lithokraft/content/blocks.h"
#include "lithokraft/content/content_selftest.h"
#include "lithokraft/content/sfx.h"
#include "litholattice/render/mesher.h"

namespace lithokraft {
namespace {

double peak(const std::vector<float>& x, size_t from, size_t to) {
  double p = 0;
  for (size_t i = from; i < to && i < x.size(); ++i) p = std::max(p, double(std::fabs(x[i])));
  return p;
}

}  // namespace

int runContentSelfTest() {
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
  };
  std::printf("Content self-test (LithoKraft on the engine SDK)\n");
  {
    bool ok = true;
    for (Sfx s : {Sfx::Place, Sfx::Break, Sfx::Click, Sfx::PulsorFire}) {
      AudioEngine eng(48000.f);
      eng.post(sfxCommand(s, 1.f, 30));
      std::vector<float> x(24000);
      eng.render(x.data(), x.size(), 1);
      ok = ok && peak(x, 0, 2400) > 0.05 && peak(x, 14400, 24000) == 0.0;
    }
    check(ok, "SFX place / break / click / pulsar fire: audible at once, silent within 300 ms");
  }
  {
    const BlockRegistry& reg = blockRegistry();
    // Registry is append-only: every named id must resolve inside it, and ids must be dense.
    const bool idsDense = kSilicaTilledSoil < reg.size() && reg.idOf(kSilicaTilledSoil) == "lithokraft:tilled_silicon";
    check(idsDense && reg.def(kAir).flags == kTransparent && reg.def(kStone).tex == "stone" &&
              roleOf(kRedTrace) == SimRole::Wire && reg.def(kRedTrace).shape == Shape::Custom &&
              reg.def(kRedTorch).shape == Shape::Cross && reg.has(kStone, kPowerable | kCutsWire),
          "block registry: game rows after engine Air, roles / shapes / game flags round-trip");
  }
  {
    // JD materials blocks (2026-10-14): appended after the formula
    // shrubs, dense and in order, string ids round-trip, registry ends
    // at the last appended block.
    const BlockRegistry& reg = blockRegistry();
    const bool appended = kCopperBlock == kFormulaShrub314 + 1 &&
                          kAluminumBlock == kCopperBlock + 1 &&
                          kGravityBlock == kAluminumBlock + 1 &&
                          kSubstrate == kGravityBlock + 1 &&
                          kInsulatnor == kSubstrate + 1 &&
                          kInsulator == kInsulatnor + 1 &&
                          kInsulatxor == kInsulator + 1 &&
                          reg.size() == size_t(kInsulatxor) + 1 &&
                          reg.idOf(kCopperBlock) == "lithokraft:copper_block" &&
                          reg.idOf(kAluminumBlock) == "lithokraft:aluminum_block" &&
                          reg.idOf(kGravityBlock) == "lithokraft:gravity_block" &&
                          reg.idOf(kSubstrate) == "lithokraft:substrate" &&
                          reg.idOf(kInsulatnor) == "lithokraft:insulatnor" &&
                          reg.idOf(kInsulator) == "lithokraft:insulator" &&
                          reg.idOf(kInsulatxor) == "lithokraft:insulatxor";
    check(appended, "materials blocks: 7 JD blocks appended dense after the shrubs (ids 67-73), string ids round-trip");
    // Substrate (JD): the Wafer's bedrock layer -- semi-transparent
    // (transparent, NOT opaque) and breakable (solid, density far under
    // the 255 unbreakable class).
    const BlockDef& sub = reg.def(kSubstrate);
    check((sub.flags & kSolid) && (sub.flags & kTransparent) && !(sub.flags & kOpaque) &&
              sub.shape == Shape::Cube && sub.density < 255,
          "substrate: semi-transparent (kTransparent, not kOpaque), solid and breakable");
    // Insulators (JD): the ceramic pattern -- solid, opaque, cuts wire,
    // never powerable (a powered insulator would not be insulating).
    bool ins = true;
    for (BlockId b : {kInsulatnor, kInsulator, kInsulatxor}) {
      const BlockDef& d = reg.def(b);
      ins = ins && (d.flags & kSolid) && (d.flags & kOpaque) && (d.flags & kCutsWire) &&
            !(d.flags & kPowerable) && d.shape == Shape::Cube && roleOf(d) == SimRole::None;
    }
    check(ins, "insulators: Insulatnor / Insulator / Insulatxor solid + opaque + cuts wire, never powerable");
    // Gravity Block: JD-named, function open -- a neutral full cube with
    // no behavior and no sim role (no mechanics invented).
    const BlockDef& grav = reg.def(kGravityBlock);
    check((grav.flags & kSolid) && grav.shape == Shape::Cube && grav.behavior.empty() &&
              roleOf(grav) == SimRole::None,
          "gravity block: neutral solid placeholder, no behavior, no sim role (function JD-open)");
  }
  {
    // F-M4: registerBlock validates the namespace format — malformed ids are
    // refused (kAir, nothing registered); a well-formed namespaced id works.
    auto mkDef = [](std::string_view id) {
      return BlockDef{"Widget", kSolid, Shape::Cube, "stone", "", 0, 0, 0, 0.8f, false, 0, id};
    };
    const size_t before = blockRegistry().size();
    const bool refused = registerBlock(mkDef("")) == kAir &&
                         registerBlock(mkDef("no_colon")) == kAir &&
                         registerBlock(mkDef("Bad NS:widget")) == kAir &&
                         registerBlock(mkDef("testmod:")) == kAir &&
                         blockRegistry().size() == before &&
                         !blockRegistry().findById("testmod:widget");
    // Positive case WITHOUT polluting the shared registry: re-registering a
    // well-formed existing id passes validation and hits first-wins (returns
    // the existing id, appends nothing). A foreign-namespace APPEND cannot
    // be exercised in this process — the save selftest pins that every id in
    // the game registry is a unique 'lithokraft:' name.
    const BlockId good = registerBlock(mkDef("lithokraft:stone"));
    check(refused && good == kStone && blockRegistry().size() == before,
          "registerBlock: malformed namespace ids refused, well-formed id registers (F-M4)");
  }
  {
    // A red trace between two red traces on stone: the appearance meshes one straight floor strip.
    MeshInput in{};
    for (auto& id : in.id) id = kAir;
    auto at = [&](int x, int y, int z) -> BlockId& { return in.id[meshPadIndex(x + 1, y + 1, z + 1)]; };
    for (int x = 4; x <= 6; ++x) { at(x, 0, 5) = kStone; at(x, 1, 5) = kRedTrace; }
    in.st[meshPadIndex(6, 2, 6)] = makeState(40);
    const BlockMaterials mats(blockRegistry().size());
    const ChunkMesh plain = meshSubChunk(in, MeshContext{blockRegistry(), mats, nullptr});
    const ChunkMesh game = meshSubChunk(in, MeshContext{blockRegistry(), mats, &blockAppearance()});
    const size_t stoneQuads = plain.verts.size() / 4;  // engine alone: custom wires are not drawn
    bool tinted = false;
    for (const PackedVertex& v : game.verts) tinted = tinted || (v.mode == kShadeStripTint0 && v.value == 40);
    check(game.verts.size() / 4 == stoneQuads + 3 && tinted,
          "appearance: wire strips mesh through IBlockAppearance (3 strips, red tint, strength 40)");
  }
  {
    // Wafer ground pulse wiring through the engine mesher: silicon faces carry
    // the kShadeGroundPulse tag, trace-tree tips (40/41) carry kShadeEmissive.
    MeshInput in{};
    for (auto& id : in.id) id = kAir;
    auto at = [&](int x, int y, int z) -> BlockId& { return in.id[meshPadIndex(x + 1, y + 1, z + 1)]; };
    at(4, 0, 4) = kWaferSilicon;
    at(8, 0, 8) = kTraceTipRed;
    at(10, 0, 8) = kTraceTipBlue;
    const BlockMaterials mats(blockRegistry().size());
    const ChunkMesh m = meshSubChunk(in, MeshContext{blockRegistry(), mats, &blockAppearance()});
    bool siliconPulse = false, tipGlow = false;
    for (const PackedVertex& v : m.verts) {
      if (v.mode == kShadeGroundPulse) siliconPulse = true;
      if (v.mode == kShadeEmissive) tipGlow = true;
    }
    check(siliconPulse, "appearance: wafer silicon faces tag kShadeGroundPulse (ground pulse)");
    check(tipGlow, "appearance: trace tips (40/41) tag kShadeEmissive (always-on glow)");
  }
  std::printf(fails ? "CONTENT SELFTEST FAILED (%d)\n" : "CONTENT SELFTEST OK\n", fails);
  return fails;
}

}  // namespace lithokraft
