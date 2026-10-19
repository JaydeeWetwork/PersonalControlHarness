// Wiring-pass selftests (B6 etch flow, B7 signing flow, player.dat v5
// stack data). Follows the game_ui_selftest convention: one PASS/FAIL
// line per check, return the failure count. See wiring_selftest.h.
#include "lithokraft/game/wiring_selftest.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>

#include "lithokraft/content/blocks.h"
#include "lithokraft/content/certificates.h"
#include "lithokraft/content/component.h"  // BehaviorCatalog
#include "lithokraft/content/items.h"
#include "lithokraft/game/dimensions.h"
#include "lithokraft/game/etching.h"
#include "lithokraft/game/inventory.h"
#include "lithokraft/game/signing.h"
#include "lithokraft/game/world_save.h"
#include "litholattice/procgen/shrubs.h"
#include "litholattice/storage/storage.h"

namespace lithokraft {
namespace {

int g_fails = 0;
void check(bool ok, const std::string& name) {
  std::printf("[%s] wiring: %s\n", ok ? "PASS" : "FAIL", name.c_str());
  if (!ok) ++g_fails;
}

// First flat slot whose stack matches pred, or -1.
template <typename Pred>
int findSlot(const Inventory& inv, Pred pred) {
  const auto slots = inv.allSlots();
  for (size_t i = 0; i < slots.size(); ++i)
    if (pred(*slots[i])) return int(i);
  return -1;
}

}  // namespace

int runWiringSelfTest() {
  std::string err;

  // ---------- B6: EtchOR block + role + behaviour seam -------------------
  {
    const BlockRegistry& reg = blockRegistry();
    check(reg.def(kEtchOR).id == "lithokraft:etchor", "B6: EtchOR block registered as lithokraft:etchor");
    check(uint32_t(kEtchOR) == uint32_t(kCertificateTable) + 1,
          "B6: EtchOR id appends directly after the certificate table (append-only, no renumber)");
    check(roleOf(kEtchOR) == SimRole::EtchOR, "B6: EtchOR carries SimRole::EtchOR");
    check(BehaviorCatalog::get().byName("etchor") != nullptr, "B6: 'etchor' behaviour registered in the catalog");
    check(BehaviorCatalog::get().describe(kEtchOR, 0) == "EtchOR", "B6: EtchOR readout resolves through its behaviour");
    check(etchInputItemId() == uint32_t(kWaferSilicon), "B6: etch input is the Wafer Silicon blank");
    check(etchedChipItemId() == uint32_t(kChipOR), "B6: etch output is the ChipOR item (block-backed id)");
  }

  // ---------- B6: the etch flow ------------------------------------------
  {
    Inventory inv;
    inv.addItem(etchInputItemId(), 2);
    check(etchChip(inv, "Alpha", &err), "B6: etch with a wafer + name succeeds");
    check(inv.countOf(etchInputItemId()) == 1, "B6: etch consumed exactly one wafer");
    const int a = findSlot(inv, [](const ItemStack& s) { return s.customName == "Alpha"; });
    check(a >= 0 && inv.slotAt(a).itemId == etchedChipItemId() && inv.slotAt(a).count == 1,
          "B6: output is one ChipOR stack bearing the name-field value");
    check(a >= 0 && stackDisplayName(inv.slotAt(a)) == "Alpha",
          "B6: the name field overwrites the chip's display name");
    check(etchChip(inv, "Alpha", &err), "B6: second identical etch succeeds");
    check(a >= 0 && inv.slotAt(a).count == 2, "B6: identical named chips stack together (identity stacking)");
    check(!etchChip(inv, "Beta", &err) && inv.countOf(etchedChipItemId()) == 2,
          "B6: etch with no wafer left fails and changes nothing");

    Inventory inv2;
    inv2.addItem(etchedChipItemId(), 3);  // plain chips
    inv2.addItem(etchInputItemId(), 1);
    check(etchChip(inv2, "Gamma", &err), "B6: etch beside plain chips succeeds");
    const int g = findSlot(inv2, [](const ItemStack& s) { return s.customName == "Gamma"; });
    const int p = findSlot(inv2, [](const ItemStack& s) {
      return s.itemId == etchedChipItemId() && !s.hasData();
    });
    check(g >= 0 && inv2.slotAt(g).count == 1 && p >= 0 && inv2.slotAt(p).count == 3,
          "B6: named output never merges into the plain chip stack");
    inv2.addItem(etchedChipItemId(), 1);
    check(inv2.slotAt(p).count == 4 && inv2.slotAt(g).count == 1,
          "B6: plain chip adds never merge into a named stack");

    Inventory inv3;
    inv3.addItem(etchInputItemId(), 1);
    check(etchChip(inv3, "", &err), "B6: empty name field etches a plain chip");
    check(findSlot(inv3, [](const ItemStack& s) {
            return s.itemId == etchedChipItemId() && !s.hasData();
          }) >= 0,
          "B6: empty-name output carries no stack data");

    Inventory inv4;
    inv4.addItem(etchInputItemId(), 1);
    check(!etchChip(inv4, std::string(kEtchNameMaxBytes + 1, 'x'), &err) &&
              inv4.countOf(etchInputItemId()) == 1,
          "B6: over-cap name refused (not truncated), wafer untouched");
    check(!etchChip(inv4, "bad\nname", &err) && inv4.countOf(etchInputItemId()) == 1,
          "B6: control characters in the name field refused");
  }

  // ---------- B7: the static curve (PROPOSED values, pinned from the doc) --
  {
    check(staticLevelCost(1) == 8 && staticLevelCost(4) == 8 && staticLevelCost(5) == 16 &&
              staticLevelCost(32) == 1024,
          "B7: curve bands are powers of two doubling every 4 levels, base 8");
    check(staticTotalForLevel(0) == 0 && staticTotalForLevel(16) == 480 && staticTotalForLevel(32) == 8160,
          "B7: curve totals match the proposal (level 16 = 480, level 32 = 8160)");
    check(staticLevelForTotal(0) == 0 && staticLevelForTotal(479) == 15 && staticLevelForTotal(480) == 16,
          "B7: level derives from total static");
    check(staticSpendLevels(480, 3) == 288, "B7: spending 3 levels from level 16 lands on the level-13 floor");
    check(staticSpendLevels(100, 99) == -1, "B7: overspend is refused");
    const CertificateDef* clarity = findCertificate("certificate_of_clarity");
    check(clarity && std::string(clarity->name) == "Certificate of Clarity",
          "B7: Certificate of Clarity is registered (DECIDED design)");
    check(findCertificate("certificate_of_knowledge") != nullptr,
          "B7: Certificate of Knowledge is registered (DECIDED 2026-10-13)");
    check(findCertificate("certificate_of_nonsense") == nullptr, "B7: unknown certificate id resolves to nothing");
  }

  // ---------- B7: the signing flow ----------------------------------------
  {
    Inventory inv;
    inv.addItem(etchedChipItemId(), 3);  // flat slot 0
    int32_t total = int32_t(staticTotalForLevel(10));  // exactly level 10 (160)
    check(signStack(inv, 0, "certificate_of_efficiency", total, &err), "B7: signing a stack succeeds");
    check(total == 96, "B7: signing spent 2 levels (level 10 -> 8 floor, enchanting-style)");
    check(inv.slotAt(0).count == 2 && !inv.slotAt(0).hasData(), "B7: the stack splits -- 2 unsigned remain");
    const int s = findSlot(inv, [](const ItemStack& st) { return !st.certificates.empty(); });
    check(s > 0 && inv.slotAt(s).count == 1 &&
              inv.slotAt(s).certificates == std::vector<std::string>{"certificate_of_efficiency"},
          "B7: the signed item is its own stack carrying the certificate id");
    check(s > 0 && !signStack(inv, s, "certificate_of_efficiency", total, &err) && total == 96,
          "B7: duplicate certificate on one item refused, static untouched");
    check(s > 0 && signStack(inv, 0, "certificate_of_knowledge", total, &err) && total == 24,
          "B7: second signing (5 levels from level 8) lands on the level-3 floor");
    int32_t poor = 10;  // level 1
    check(!signStack(inv, 0, "certificate_of_clarity", poor, &err) && poor == 10,
          "B7: insufficient level refused, static untouched");
    check(!signStack(inv, 0, "certificate_of_nonsense", total, &err), "B7: unknown certificate refused");
    check(!signStack(inv, 40, "certificate_of_efficiency", total, &err), "B7: empty slot refused");
  }

  // ---------- player.dat v5: stack data persists; v4 still loads ----------
  {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() /
        ("lithokraft_wiring_selftest_" + std::to_string(std::time(nullptr)));
    fs::remove_all(dir);
    fs::create_directories(dir);
    WorldSave save(dir);
    PlayerMeta m;
    m.dimension = "wafer";
    ItemStack named;
    named.itemId = etchedChipItemId();
    named.count = 1;
    named.customName = "Alpha";
    named.certificates = {"certificate_of_clarity"};
    m.inventory.slotAt(0) = named;
    ItemStack plain;
    plain.itemId = uint32_t(kStone);
    plain.count = 7;
    m.inventory.slotAt(5) = plain;
    check(save.writePlayer(m, &err), "B6/B7: player.dat v5 writes");
    const auto back = save.readPlayer(&err);
    check(back && back->fileVersion == PlayerMeta::kVersion, "B6/B7: written file reads back at the current version");
    check(back && back->inventory.slotAt(0).customName == "Alpha" &&
              back->inventory.slotAt(0).certificates == std::vector<std::string>{"certificate_of_clarity"},
          "B6/B7: name field + certificate list survive the save round trip");
    check(back && back->inventory.slotAt(5).itemId == uint32_t(kStone) &&
              back->inventory.slotAt(5).count == 7 && !back->inventory.slotAt(5).hasData(),
          "B6/B7: plain stacks round-trip with no data attached");

    // A hand-built v4 player.dat (the pre-v5 layout, no stack-data
    // block) must still load, with plain stacks -- world permanence.
    storage::ByteWriter body;
    body.u16(4);
    auto f32 = [&body](float v) { uint32_t u; std::memcpy(&u, &v, 4); body.u32(u); };
    f32(0.f); f32(0.f); f32(0.f); f32(0.f); f32(0.f);
    const std::string dimName = "wafer";
    body.bytes(storage::Bytes(dimName.begin(), dimName.end()));
    body.u8(0);                       // hot
    body.u8(0);                       // hotbar count
    body.u16(0);                      // stats
    body.u16(0);                      // vitals
    body.u16(1);                      // one inventory slot
    const std::string chipSid = "lithokraft:chipor";
    body.bytes(storage::Bytes(chipSid.begin(), chipSid.end()));
    body.u16(2);
    storage::ByteWriter file;
    file.u32(0x4D504C4C);  // "LLPM" (kPlayerMagic in world_save.cpp)
    file.u32(storage::crc32(body.out.data(), body.out.size()));
    file.bytes(body.out);
    std::string werr;
    check(storage::writeFileAtomic(dir / "player.dat", file.out, &werr),
          "B6/B7: hand-built v4 player.dat writes");
    const auto back4 = save.readPlayer(&err);
    check(back4 && back4->fileVersion == 4 && back4->hasInventory &&
              back4->inventory.slotAt(0).itemId == etchedChipItemId() &&
              back4->inventory.slotAt(0).count == 2 && !back4->inventory.slotAt(0).hasData(),
          "B6/B7: a v4 save still loads after the v5 bump (stacks plain, nothing lost)");
    fs::remove_all(dir);
  }

  // ---- B4: procgen plant blocks + live worldgen binding ----
  {
    namespace pg = litholattice::procgen;
    const BlockId plantIds[7] = {kProcgenGrassTuft, kProcgenFern, kProcgenDryScrub,
                                 kProcgenFormulaFlower, kProcgenFormulaStar,
                                 kProcgenFormulaLeaf, kProcgenFormulaBlob};
    bool reg = true;
    for (int i = 0; i < 7; ++i) {
      const BlockDef& d = blockDef(plantIds[i]);
      reg = reg && plantIds[i] == kEtchOR + 1 + i && d.shape == litholattice::Shape::Cross &&
            std::string(d.tex) == pg::plantTextureNameAny(i);
    }
    check(reg, "B4: seven procgen plant blocks registered (Cross, texture-index "
            "order, ids 58-64 after EtchOR)");
    // Formula selection per biome_vegetation.h's rows (super* params copied
    // from the table): DicingPlains (4, .6/.6/.6, 1.8) -> leaf index 5;
    // ResonatorSpires (3, 1.5/1.5/1.5, 1) -> blob index 6. (The vegetation
    // selftest pins shrubParamsForRow itself; this pins the texture choice
    // the groundcover binding relies on.)
    pg::ShrubParams dicingP;
    dicingP.superM = 4.0; dicingP.superN1 = 0.6; dicingP.superN2 = 0.6;
    dicingP.superN3 = 0.6; dicingP.superA = 1.8; dicingP.superB = 1.0;
    pg::ShrubParams spiresP;
    spiresP.superM = 3.0; spiresP.superN1 = 1.5; spiresP.superN2 = 1.5;
    spiresP.superN3 = 1.5; spiresP.superA = 1.0; spiresP.superB = 1.0;
    check(pg::formulaTextureIndexFor(dicingP) == 5 &&
           pg::formulaTextureIndexFor(spiresP) == 6,
       "B4: biome formula rows resolve to leaf (5) / blob (6) plant textures");
    // Placement proof on the shipped wafer.json through the real dimension
    // registry: leaf appears in DicingPlains columns, blob in
    // ResonatorSpires columns, silica_grass no longer appears as worldgen
    // groundcover, and regeneration is cell-identical.
    auto dims = createDimensions();
    const Dimension* wafer = dims.find("wafer");
    check(wafer && wafer->generator,
       "B4: wafer generator loads (groundcover materials resolve fail-closed)");
    if (wafer && wafer->generator) {
      const litholattice::StableId sidLeaf = blockRegistry().stableIdOf(kProcgenFormulaLeaf);
      const litholattice::StableId sidBlob = blockRegistry().stableIdOf(kProcgenFormulaBlob);
      const litholattice::StableId sidSilica = blockRegistry().stableIdOf(kSilicaGrass);
      long leafInDicing = 0, blobInSpires = 0, silicaAnywhere = 0;
      bool sawDicing = false, sawSpires = false;
      for (int cx = -6; cx < 6 && !(leafInDicing > 0 && blobInSpires > 0); ++cx) {
        for (int cz = -6; cz < 6 && !(leafInDicing > 0 && blobInSpires > 0); ++cz) {
          pg::ColumnMaps maps;
          auto col = wafer->generator->generate(cx, cz, &maps);
          if (!col || maps.biome.size() != 256) continue;
          for (int lz = 0; lz < 16; ++lz) {
            for (int lx = 0; lx < 16; ++lx) {
              const uint8_t biome = maps.biome[lz * 16 + lx];
              if (biome == 1) sawDicing = true;
              if (biome == 3) sawSpires = true;
              for (size_t sy = 0; sy < col->subs.size(); ++sy) {
                const auto& sub = col->subs[sy];
                if (!sub) continue;
                for (int ly = 0; ly < 16; ++ly) {
                  const litholattice::StableId b = sub->blocks.at(
                      size_t(litholattice::SubChunk::index(lx, ly, lz)));
                  if (b == sidLeaf && biome == 1) ++leafInDicing;
                  if (b == sidBlob && biome == 3) ++blobInSpires;
                  if (b == sidSilica) ++silicaAnywhere;
                }
              }
            }
          }
        }
      }
      check(sawDicing && sawSpires,
         "B4: scan window covers DicingPlains and ResonatorSpires");
      check(leafInDicing > 0, "B4: formula leaf shrubs generate in DicingPlains");
      check(blobInSpires > 0, "B4: formula blob shrubs generate in ResonatorSpires");
      check(silicaAnywhere == 0,
         "B4: worldgen no longer places silica_grass as groundcover");
      pg::ColumnMaps m1, m2;
      auto c1 = wafer->generator->generate(2, 2, &m1);
      auto c2 = wafer->generator->generate(2, 2, &m2);
      bool same = c1 && c2 && c1->subs.size() == c2->subs.size();
      for (size_t s = 0; same && s < c1->subs.size(); ++s) {
        if (!c1->subs[s] || !c2->subs[s]) { same = !c1->subs[s] && !c2->subs[s]; continue; }
        for (int i = 0; i < litholattice::kSubChunkVolume; ++i)
          if (c1->subs[s]->blocks.at(i) != c2->subs[s]->blocks.at(i)) { same = false; break; }
      }
      check(same, "B4: regenerated chunk is cell-identical (deterministic)");
    }
  }

  return g_fails;
}

}  // namespace lithokraft
