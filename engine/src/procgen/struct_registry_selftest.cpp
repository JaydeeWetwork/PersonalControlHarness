// Self-test for the StructureRegistry, placement-solver upgrades, and the
// structure block capture/place helpers. Determinism is the invariant under
// test: same seed, same world, bit-identical.
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "litholattice/procgen/struct_block.h"
#include "litholattice/procgen/struct_registry.h"
#include "litholattice/procgen/stages.h"
#include "litholattice/procgen/generator_loader.h"
#include "litholattice/procgen/seed.h"
#include "litholattice/crypto/ed25519.h"
#include "litholattice/crypto/signing.h"

namespace litholattice::procgen {
namespace {

struct Check {
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }
  int fails_ = 0;
};

std::vector<BlueprintCell> testCells() {
  // A 2x2x2 cube of material 7.
  std::vector<BlueprintCell> cells;
  for (int y = 0; y < 2; ++y)
    for (int z = 0; z < 2; ++z)
      for (int x = 0; x < 2; ++x) cells.push_back({x, y, z, MaterialId{7}, 0.f});
  return cells;
}

StructureEntry testEntry(const std::string& name) {
  StructureEntry e;
  e.name = name;
  StructureVariant v;
  v.cells = testCells();
  v.weight = 1.f;
  e.variants.push_back(std::move(v));
  return e;
}

void registryChecks(Check& check) {
  auto& reg = StructureRegistry::instance();
  reg.resetForTests();

  check(reg.registerEntry(testEntry("jd:hut")), "registry: register ok");
  check(!reg.registerEntry(testEntry("jd:hut")), "registry: duplicate rejected (first wins)");
  StructureEntry empty;
  empty.name = "jd:empty";
  check(!reg.registerEntry(empty), "registry: no variants rejected");
  StructureEntry nameless = testEntry("");
  check(!reg.registerEntry(nameless), "registry: empty name rejected");
  StructureEntry badWeight = testEntry("jd:badweight");
  badWeight.variants.front().weight = 0.f;
  check(!reg.registerEntry(badWeight), "registry: non-positive weight rejected");

  const StructureEntry* found = reg.find("jd:hut");
  check(found && found->variants.size() == 1 && found->variants.front().cells.size() == 8,
        "registry: find returns the entry");
  check(reg.find("jd:nope") == nullptr, "registry: unknown name -> nullptr");
  check(reg.size() == 1, "registry: size counts accepted entries");

  reg.seal();
  check(reg.isSealed(), "registry: sealed");
  check(!reg.registerEntry(testEntry("jd:late")), "registry: sealed refuses registration");
  check(reg.find("jd:hut") != nullptr, "registry: sealed still reads");
  reg.resetForTests();
  check(!reg.isSealed() && reg.size() == 0, "registry: resetForTests clears");
}

void separationChecks(Check& check) {
  // Separation: with spacing > 0, no two candidate bases are closer than spacing.
  StructureSettings s;
  s.seedName = "septest";
  s.cellSize = 16;
  s.density = 1.0f;
  s.spacing = 24.f;  // larger than the cell: neighbours must yield
  s.biomes.density.fill(1.f);
  StructureFeature feat(std::move(s), testCells());
  feat.setBiomeProbe([](int32_t, int32_t) -> uint8_t { return 0; });

  const uint64_t dimSeed = 0x12345678ull;
  const auto bases = feat.basesInRect(dimSeed, -128, -128, 128, 128);
  check(!bases.empty(), "placement: separation test produced bases");
  bool ok = true;
  for (size_t i = 0; i < bases.size() && ok; ++i)
    for (size_t j = i + 1; j < bases.size() && ok; ++j) {
      const float dx = float(bases[i][0] - bases[j][0]);
      const float dz = float(bases[i][1] - bases[j][1]);
      if (dx * dx + dz * dz < 24.f * 24.f - 1e-3f) ok = false;
    }
  check(ok, "placement: separation keeps bases >= spacing apart (" +
                std::to_string(bases.size()) + " bases)");
  check(feat.basesInRect(dimSeed, -128, -128, 128, 128) == bases,
        "placement: separation deterministic across runs");
}

void variantChecks(Check& check) {
  // Variant pool: construction is deterministic; the pool is observable.
  auto makeFeat = [] {
    StructureSettings s;
    s.seedName = "varianttest";
    s.cellSize = 16;
    s.density = 1.0f;
    s.biomes.density.fill(1.f);
    std::vector<StructureFeature::Variant> variants;
    for (int i = 0; i < 3; ++i) {
      StructureFeature::Variant v;
      v.cells = testCells();
      v.cells.front().material = MaterialId{10 + i};  // tag each variant
      v.weight = float(i + 1);                        // weights 1, 2, 3
      variants.push_back(std::move(v));
    }
    return StructureFeature(std::move(s), std::move(variants));
  };
  StructureFeature feat = makeFeat();
  StructureFeature feat2 = makeFeat();
  feat.setBiomeProbe([](int32_t, int32_t) -> uint8_t { return 0; });
  feat2.setBiomeProbe([](int32_t, int32_t) -> uint8_t { return 0; });
  check(feat.variants().size() == 3, "variants: pool holds 3 variants");

  const uint64_t dimSeed = 0xabcdefull;
  const auto b1 = feat.basesInRect(dimSeed, 0, 0, 256, 256);
  const auto b2 = feat2.basesInRect(dimSeed, 0, 0, 256, 256);
  check(b1 == b2 && !b1.empty(),
        "variants: identical construction -> identical bases (deterministic)");
}

void structBlockChecks(Check& check) {
  // Capture round-trip: build a small voxel map, capture, place elsewhere, compare.
  // World: 8x8x8 of material 5 with a 2x2x2 cube of material 7 at (2,2,2).
  MaterialId world[8][8][8] = {};
  for (int y = 0; y < 8; ++y)
    for (int z = 0; z < 8; ++z)
      for (int x = 0; x < 8; ++x) world[y][z][x] = MaterialId{5};
  for (int y = 2; y < 4; ++y)
    for (int z = 2; z < 4; ++z)
      for (int x = 2; x < 4; ++x) world[y][z][x] = MaterialId{7};
  // A structure block at (0,0,0) must be skipped by capture.
  const MaterialId kStructMat = MaterialId{99};
  world[0][0][0] = kStructMat;

  BlockReader reader = [&](int32_t x, int32_t y, int32_t z) -> MaterialId {
    if (x < 0 || y < 0 || z < 0 || x >= 8 || y >= 8 || z >= 8) return kAirMaterial;
    return world[y][z][x];
  };
  // Capture the 2x2x2 cube region only.
  auto cells = captureRegion(reader, 2, 2, 2, 3, 3, 3, kStructMat);
  check(cells.size() == 8, "structure block: capture finds 8 cells");
  bool sorted = true, allSeven = true;
  for (size_t i = 1; i < cells.size(); ++i) {
    const auto& a = cells[i - 1];
    const auto& b = cells[i];
    if (!(a.dy < b.dy || (a.dy == b.dy && (a.dz < b.dz || (a.dz == b.dz && a.dx <= b.dx)))))
      sorted = false;
  }
  for (const auto& c : cells) {
    if (c.material != MaterialId{7}) allSeven = false;
    if (c.dx < 0 || c.dx > 1 || c.dy < 0 || c.dy > 1 || c.dz < 0 || c.dz > 1) allSeven = false;
  }
  check(sorted && allSeven, "structure block: capture sorted, relative, correct material");

  // Capture skips structure blocks.
  auto cells2 = captureRegion(reader, 0, 0, 0, 0, 0, 0, kStructMat);
  check(cells2.empty(), "structure block: capture skips structure blocks");

  // Place the captured cells into a fresh world; compare cell-by-cell.
  MaterialId out[8][8][8] = {};
  BlockWriter writer = [&](int32_t x, int32_t y, int32_t z, MaterialId m) -> bool {
    if (x < 0 || y < 0 || z < 0 || x >= 8 || y >= 8 || z >= 8) return false;
    out[y][z][x] = m;
    return true;
  };
  const size_t placed = placeCells(writer, cells, 4, 4, 4, 0);
  check(placed == 8, "structure block: place writes 8 cells");
  bool identical = true;
  for (int y = 0; y < 2 && identical; ++y)
    for (int z = 0; z < 2 && identical; ++z)
      for (int x = 0; x < 2 && identical; ++x)
        if (out[4 + y][4 + z][4 + x] != MaterialId{7}) identical = false;
  check(identical, "structure block: capture -> place round-trip identical");

  // SAVE/LOAD via the registry.
  auto& reg = StructureRegistry::instance();
  reg.resetForTests();
  check(saveStructureAs(reader, "jd:testcube", 2, 2, 2, 3, 3, 3, kStructMat),
        "structure block: SAVE registers the capture");
  check(!saveStructureAs(reader, "jd:testcube", 2, 2, 2, 3, 3, 3, kStructMat),
        "structure block: SAVE duplicate name rejected");
  MaterialId out2[8][8][8] = {};
  BlockWriter writer2 = [&](int32_t x, int32_t y, int32_t z, MaterialId m) -> bool {
    if (x < 0 || y < 0 || z < 0 || x >= 8 || y >= 8 || z >= 8) return false;
    out2[y][z][x] = m;
    return true;
  };
  check(loadStructureAt("jd:testcube", writer2, 0, 0, 0, 0) == 8,
        "structure block: LOAD places 8 cells");
  check(loadStructureAt("jd:missing", writer2, 0, 0, 0, 0) == 0,
        "structure block: LOAD unknown name places nothing");
  bool identical2 = true;
  for (int y = 0; y < 2 && identical2; ++y)
    for (int z = 0; z < 2 && identical2; ++z)
      for (int x = 0; x < 2 && identical2; ++x)
        if (out2[y][z][x] != MaterialId{7}) identical2 = false;
  check(identical2, "structure block: SAVE -> LOAD round-trip identical");
  reg.resetForTests();
}

void loaderNameChecks(Check& check) {
  // The "structure": "<name>" JSON field resolves through the registry.
  auto& reg = StructureRegistry::instance();
  reg.resetForTests();
  check(reg.registerEntry(testEntry("jd:watchtower")), "loader: test structure registered");

  const auto mat = [](std::string_view name) -> std::optional<MaterialId> {
    if (name == "rock") return MaterialId{1};
    if (name == "turf") return MaterialId{2};
    return std::nullopt;
  };
  const std::string base = R"({"graph": {"nodes": {"h": {"type": "constant"}}, "outputs": {"height": "h"}},)"
                           R"( "terrain": {"output": "height", "material": "rock"},)"
                           R"( "biomes": [{"name": "hot"}],)";
  {
    std::string err;
    const auto gen = loadGenerator(
        base + R"( "features": [{"name": "t", "type": "structure", "structure": "jd:watchtower",)"
              R"( "blueprint": [{"d": [0,0,0], "m": "turf"}]}]})",
        dimensionSeed(7, "t"), mat, err);
    check(gen == nullptr, "loader: 'structure' + 'blueprint' rejected (mutually exclusive)");
  }
  {
    std::string err;
    const auto gen = loadGenerator(
        base + R"( "features": [{"name": "t", "type": "structure", "structure": "jd:nope"}]})",
        dimensionSeed(7, "t"), mat, err);
    check(gen == nullptr, "loader: unknown structure name rejected");
  }
  {
    std::string err;
    const auto gen = loadGenerator(
        base + R"( "features": [{"name": "t", "type": "structure", "structure": "jd:watchtower",)"
              R"( "spacing": 32, "anchor": "floating", "hover_height": 10}]})",
        dimensionSeed(7, "t"), mat, err);
    check(gen != nullptr, "loader: 'structure' name + rule fields load" + (err.empty() ? "" : " (" + err + ")"));
  }
  {
    std::string err;
    const auto gen = loadGenerator(
        base + R"( "features": [{"name": "t", "type": "structure", "anchor": "nope",)"
              R"( "blueprint": [{"d": [0,0,0], "m": "turf"}]}]})",
        dimensionSeed(7, "t"), mat, err);
    check(gen == nullptr, "loader: bad anchor rejected");
  }
  reg.resetForTests();
}

void identityChecks(Check& check) {
  auto& reg = StructureRegistry::instance();
  reg.resetForTests();

  // Deterministic test keypairs (fixed seeds — no RNG anywhere in tests).
  crypto::ed25519::Seed seed1{};
  crypto::ed25519::Seed seed2{};
  for (size_t i = 0; i < 32; ++i) {
    seed1[i] = uint8_t(0xA0 + i);
    seed2[i] = uint8_t(0x50 + i);
  }
  crypto::ed25519::PublicKey pub1, pub2;
  crypto::ed25519::SecretKey sec1, sec2;
  crypto::ed25519::keypair_from_seed(seed1, pub1, sec1);
  crypto::ed25519::keypair_from_seed(seed2, pub2, sec2);
  const std::string pub1Hex = crypto::toHex(pub1.data(), pub1.size());

  auto signFp = [&](const std::string& fpHex, const crypto::ed25519::SecretKey& sec,
                    const crypto::ed25519::PublicKey& pub) {
    std::array<uint8_t, 32> fpBytes{};
    check(crypto::fromHex(fpHex, fpBytes.data(), fpBytes.size()), "identity: fp hex parses");
    StructureEntry::Signature sig;
    sig.publicKeyHex = crypto::toHex(pub.data(), pub.size());
    const auto edSig = crypto::ed25519::sign(fpBytes.data(), fpBytes.size(), sec);
    sig.signatureHex = crypto::toHex(edSig.data(), edSig.size());
    return sig;
  };

  // --- fingerprint determinism ---
  StructureVariant v;
  v.cells = testCells();
  v.weight = 1.f;
  const std::string fp1 = StructureRegistry::computeFingerprint({v});
  const std::string fp2 = StructureRegistry::computeFingerprint({v});
  check(fp1 == fp2 && fp1.size() == 64, "identity: fingerprint deterministic, 64 hex chars");

  // Reordered input cells -> same fingerprint.
  StructureVariant vr;
  vr.weight = 1.f;
  auto rc = testCells();
  std::reverse(rc.begin(), rc.end());
  vr.cells = rc;
  check(StructureRegistry::computeFingerprint({vr}) == fp1, "identity: reordered cells, same hash");

  // Different blueprint -> different fingerprint.
  StructureVariant vd;
  vd.weight = 1.f;
  vd.cells = testCells();
  vd.cells.push_back({9, 9, 9, MaterialId{3}, 0.f});
  check(StructureRegistry::computeFingerprint({vd}) != fp1, "identity: different blueprint, different hash");

  // --- name validation ---
  check(StructureRegistry::isValidName("jd:watchtower"), "identity: good name ok");
  check(StructureRegistry::isValidName("a:b_c9"), "identity: good name ok 2");
  for (const char* bad : {"nocolon", "UPPER:name", "jd:", ":name", "a:b:c", "jd:watch tower",
                          "", "jd:watch-tower", "jd:name!"}) {
    check(!StructureRegistry::isValidName(bad),
          std::string("identity: bad name rejected: '") + bad + "'");
  }
  {
    std::string err;
    StructureEntry e = testEntry("BadName");
    check(!reg.registerEntry(std::move(e), nullptr, &err) && !err.empty(),
          "identity: malformed name refused with reason");
  }

  // --- registration stores fingerprint; findByFingerprint; list; search ---
  {
    std::string err;
    check(reg.registerEntry(testEntry("jd:watchtower"), nullptr, &err), "identity: register ok");
    check(reg.registerEntry(testEntry("jd:hut"), nullptr, &err), "identity: register ok 2");
    check(reg.registerEntry(testEntry("mod:village"), nullptr, &err), "identity: register ok 3");
  }
  const StructureEntry* w = reg.find("jd:watchtower");
  check(w && w->fingerprintHex == fp1, "identity: stored fingerprint matches computed");
  check(reg.findByFingerprint(fp1) == w, "identity: findByFingerprint round-trip");
  check(reg.findByFingerprint(std::string(64, '0')) == nullptr, "identity: unknown fp -> null");
  const auto names = reg.list();
  check(names.size() == 3 && names[0] == "jd:watchtower" && names[2] == "mod:village",
        "identity: list() in registration order");
  check(reg.findByNamespace("jd").size() == 2, "identity: findByNamespace jd -> 2");
  check(reg.findByNamespace("mod").size() == 1, "identity: findByNamespace mod -> 1");
  check(reg.findByNamespace("nope").empty(), "identity: findByNamespace unknown -> empty");
  check(reg.findByPrefix("jd:watch").size() == 1, "identity: findByPrefix jd:watch -> 1");
  check(reg.findByPrefix("jd:").size() == 2, "identity: findByPrefix jd: -> 2");
  check(reg.findByPrefix("").size() == 3, "identity: empty prefix matches all");

  // --- signing ---
  reg.setTrustedPublicKeyHex(pub1Hex);  // game boots this with JD's baked-in key
  {
    // Signed by the trusted key -> Trusted.
    StructureEntry e = testEntry("jd:signed");
    const std::string fp = StructureRegistry::computeFingerprint(e.variants);
    auto sig = signFp(fp, sec1, pub1);
    std::string err;
    check(reg.registerEntry(std::move(e), &sig, &err), "identity: signed registration ok");
    check(reg.verifyEntry("jd:signed") == StructureTrust::Trusted, "identity: trusted signature");
  }
  {
    // Tampered: signature over a DIFFERENT blueprint -> registration refused.
    StructureEntry e = testEntry("jd:tampered");
    const std::string otherFp = StructureRegistry::computeFingerprint({vd});
    auto sig = signFp(otherFp, sec1, pub1);
    std::string err;
    check(!reg.registerEntry(std::move(e), &sig, &err) && !err.empty(),
          "identity: tampered entry refused (fail closed)");
    check(reg.find("jd:tampered") == nullptr, "identity: refused entry not stored");
  }
  {
    // Valid signature, unknown key -> registers, reports SignedByUnknownKey.
    StructureEntry e = testEntry("jd:selfsigned");
    const std::string fp = StructureRegistry::computeFingerprint(e.variants);
    auto sig = signFp(fp, sec2, pub2);
    std::string err;
    check(reg.registerEntry(std::move(e), &sig, &err), "identity: unknown-key sig registers");
    check(reg.verifyEntry("jd:selfsigned") == StructureTrust::SignedByUnknownKey,
          "identity: unknown key reported");
  }
  {
    // Malformed signature hex -> refused.
    StructureEntry e = testEntry("jd:badsig");
    StructureEntry::Signature sig{"zzzz", "00"};
    std::string err;
    check(!reg.registerEntry(std::move(e), &sig, &err), "identity: malformed sig refused");
  }
  check(reg.verifyEntry("jd:watchtower") == StructureTrust::Unsigned, "identity: unsigned entry");
  check(reg.verifyEntry("jd:missing") == StructureTrust::Unsigned, "identity: unknown name unsigned");

  reg.resetForTests();
}

}  // namespace

int runStructRegistrySelfTest() {
  std::printf("StructRegistry self-test (registry, placement solver, structure block)\n");
  Check check;
  registryChecks(check);
  separationChecks(check);
  variantChecks(check);
  structBlockChecks(check);
  loaderNameChecks(check);
  identityChecks(check);
  const int fails = check.fails();
  std::printf(fails ? "STRUCTREGISTRY SELFTEST FAILED (%d)\n" : "STRUCTREGISTRY SELFTEST OK\n", fails);
  return fails;
}

}  // namespace litholattice::procgen
