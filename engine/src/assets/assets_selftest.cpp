// Builds small packs in a temp folder (tiny PNGs written with Image::savePng) and checks the pack stack, registry, models and shader loading.
#include "litholattice/assets/assets_selftest.h"
#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include "litholattice/assets/image.h"
#include "litholattice/assets/resource_loader.h"
#include "litholattice/assets/shader_library.h"
#include "litholattice/assets/mod_manifest.h"
#include "litholattice/assets/mod_signature.h"
#include "litholattice/core/test_env.h"
#include "litholattice/crypto/signing.h"

namespace litholattice {
namespace {
namespace fs = std::filesystem;

void writeFile(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << text;
}

/// Small RGBA PNG filled with `rgba` (0xRRGGBBAA); row y adds y to blue so the frames of a
/// strip differ.
void writePng(const fs::path& p, int w, int h, uint32_t rgba) {
  std::vector<uint8_t> px;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      px.insert(px.end(), {uint8_t(rgba >> 24), uint8_t(rgba >> 16), uint8_t(((rgba >> 8) & 255) + uint32_t(y)), uint8_t(rgba & 255)});
  fs::create_directories(p.parent_path());
  Image(w, h, std::move(px)).savePng(p, nullptr);
}

uint8_t red(const TextureRegistry& r, std::string_view name) { return r.texture(r.id(name)).image.pixel(0, 0)[0]; }

}  // namespace

int runAssetsSelfTest() {
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
  };
  std::printf("Assets self-test (throwaway packs)\n");
  std::error_code ec;
  const fs::path root = fs::temp_directory_path(ec) /
      ("litholattice-selftest-assets-" + std::to_string(test_env::processId()));  // R-TST-4: per process; engine_assets and lithokraft_selftest both run this
  fs::remove_all(root, ec);
  const fs::path base = root / "base", mod = root / "mod", player = root / "player", broken = root / "broken";

  writeFile(base / "pack.json", R"({"name": "base"})");
  writeFile(base / "resources.json", R"({
    "version": 1, "tile_size": 4,
    "textures": {"stone": "textures/stone.png", "dirt": "textures/dirt.png", "gone": "textures/gone.png",
                 "wide": "textures/wide.png", "evil": "../outside.png", "a": "textures/a.png", "b": "textures/b.png",
                 "c": "textures/c.png", "d": "textures/d.png"},
    "animations": {"blink": {"file": "textures/blink.png", "loop": false}},
    "palettes": {"ramp": "textures/ramp.png"},
    "models": {
      "full": {"faces": {"top": "a", "side": "b", "all": "c"}, "active": {"top": {"texture": "d", "uv": [0, 0, 8, 16]}}},
      "plain": {"faces": {"all": "c"}},
      "lamp": {"faces": {"top": "blink"}, "palette": "ramp", "masks": {"rim": "a"}},
      "wire": {"wire": {"straight": "a", "dot": "b"}},
      "bad": {"faces": {"top": "nope", "diagonal": "a"}, "palette": "nope"}
    }})");
  writePng(base / "textures/stone.png", 4, 4, 0x100000FFu);
  writePng(base / "textures/dirt.png", 4, 4, 0x200000FFu);
  writePng(base / "textures/wide.png", 8, 4, 0x300000FFu);
  for (const char* n : {"a", "b", "c", "d"}) writePng(base / "textures" / (std::string(n) + ".png"), 4, 4, 0x400000FFu);
  writePng(base / "textures/blink.png", 4, 12, 0x500000FFu);
  writeFile(base / "textures/blink.png.json", R"({"frametime_ms": 40, "frames": [2, 0], "loop": true})");
  writePng(base / "textures/ramp.png", 5, 1, 0x600000FFu);
  writeFile(base / "shaders/chunk.vert", "// base chunk vert\n");
  writeFile(base / "shaders/chunk.frag", "// base chunk frag\n");
  writeFile(base / "shaders/hud.vert", "// base hud vert only\n");
  writePng(mod / "textures/stone.png", 4, 4, 0x700000FFu);  // file override, no JSON at all
  writeFile(mod / "shaders/chunk.frag", "// mod chunk frag\n  #include \"common.glsl\"\n");
  writeFile(base / "shaders/common.glsl", "// common");
  writeFile(player / "resources.json", R"({"textures": {"dirt": "textures/my_dirt.png"}})");
  writePng(player / "textures/my_dirt.png", 4, 4, 0x800000FFu);
  writeFile(broken / "resources.json", R"({"textures": {"x": "y.png",})");

  PackStack stack;
  std::string err;
  for (const fs::path& p : {base, mod, player})
    if (auto pack = ResourcePack::open(p, &err)) stack.push(std::move(*pack));
  const ResourceSet rs = loadResources(stack);
  const TextureRegistry& reg = rs.textures;
  auto warned = [&](std::string_view needle) {
    for (const auto& w : rs.warnings) if (w.find(needle) != std::string::npos) return true;
    return false;
  };

  check(stack.packs().size() == 3 && red(reg, "stone") == 0x70 && reg.texture(reg.id("stone")).origin == "mod" &&
            red(reg, "dirt") == 0x80 && reg.texture(reg.id("dirt")).origin == "player" && red(reg, "a") == 0x40,
        "pack stack: a file resolves to the highest pack that has it; a higher resources.json replaces a definition");
  const Image& miss = reg.texture(TextureId{}).image;
  check(reg.id("gone").missing() && reg.id("never-defined").missing() && reg.id("wide").missing() && reg.id("evil").missing() &&
            miss.pixel(0, 0)[0] == 0 && miss.pixel(3, 0)[0] == 255 && miss.pixel(3, 0)[2] == 255 &&
            warned("gone") && warned("expected 4x4") && warned("unsafe path") && reg.missingNames().size() == 3,
        "missing fallback: absent file, wrong size, unsafe path and unknown names all map to the magenta checker (id 0)");
  {
    std::string perr;
    const bool rejected = !ResourcePack::open(broken, &perr) && perr.find("resources.json") != std::string::npos;
    PackStack s2;
    s2.push(*ResourcePack::open(base, &perr));
    const fs::path odd = root / "odd";
    writeFile(odd / "resources.json", R"({"version": 9, "textures": ["x"], "colours": {}})");
    s2.push(*ResourcePack::open(odd, &perr));
    const ResourceSet r2 = loadResources(s2);
    int hits = 0;
    for (const auto& w : r2.warnings)
      hits += w.find("version 9") != std::string::npos || w.find("'textures' must be an object") != std::string::npos ||
              w.find("unknown key 'colours'") != std::string::npos;
    check(rejected && hits == 3 && red(r2.textures, "stone") == 0x10,
          "manifest errors: a syntax error rejects the pack (file + position); bad sections / version are warnings");
  }
  {
    const auto pal = reg.findPalette("ramp");
    const auto anim = reg.findAnimation("blink");
    const AnimatedTexture* a = anim ? &reg.animation(*anim) : nullptr;
    check(pal && reg.palette(*pal).colors.size() == 5 && reg.palette(*pal).colors[0][0] == 0x60 && reg.paletteWidth() == 5 &&
              a && a->frameCount == 3 && a->timing.frameMs == 40 && !a->timing.loop && a->timing.sequence == std::vector{2, 0} &&
              reg.texture(TextureId{uint16_t(a->firstFrame.value + 2)}).name == "blink#2" &&
              reg.texture(TextureId{uint16_t(a->firstFrame.value + 2)}).image.pixel(0, 0)[2] == 8,
          "palettes (row 0) and animation strips (frames as consecutive layers, sidecar timing, inline override)");
  }
  {
    const BlockModel* full = rs.models.find("full");
    const BlockModel* plain = rs.models.find("plain");
    const BlockModel* lamp = rs.models.find("lamp");
    const BlockModel* wire = rs.models.find("wire");
    const BlockModel* bad = rs.models.find("bad");
    const TextureId A = reg.id("a"), B = reg.id("b"), C = reg.id("c"), D = reg.id("d");
    const bool faces = full && full->face(FaceSlot::Top, false).texture == A && full->face(FaceSlot::Front, false).texture == B &&
                       full->face(FaceSlot::Bottom, false).texture == B && full->face(FaceSlot::Top, true).texture == D &&
                       full->face(FaceSlot::Top, true).uv == UvRect{0, 0, 8, 16} && full->face(FaceSlot::Left, true).texture == B &&
                       plain && plain->face(FaceSlot::Front, true).texture == C;
    const bool extras = lamp && lamp->face(FaceSlot::Top, false).animation.valid() && lamp->palette.valid() &&
                        lamp->mask("rim") == A && lamp->mask("ring").missing() && wire && wire->isWire() &&
                        wire->wirePiece(WirePiece::Dot).texture == B && wire->wirePiece(WirePiece::Cross).texture == A;
    const bool broken2 = bad && bad->face(FaceSlot::Top, false).texture.missing() && !bad->palette.valid() &&
                         warned("unknown face 'diagonal'") && warned("unknown texture 'nope'");
    check(faces && extras && broken2,
          "block models: face -> side -> all -> missing; active falls back to base; UV rects; animated faces; wire pieces; masks");
  }
  {
    std::vector<std::string> w;
    constexpr std::string_view progs[] = {"chunk", "hud", "line"};
    const ShaderLibrary lib = ShaderLibrary::load(stack, progs, &w);
    const ShaderSource* chunk = lib.find("chunk");
    check(chunk && chunk->vertex == "// base chunk vert\n" && chunk->fragment == "// mod chunk frag\n// common\n" &&
              chunk->origin == "base + mod" && !lib.find("hud") && !lib.find("line") && w.size() == 1,
          "shaders: loaded from pack files per stage with overrides and includes; a half program is skipped (built-in used)");
    const ShaderIncludeResolver res = [](std::string_view n) -> std::optional<std::string> {
      if (n == "self.glsl") return "#include \"self.glsl\"";
      if (n == "a.glsl") return "A";
      return std::nullopt;
    };
    std::string e1, e2;
    const auto ok = expandShaderIncludes("x\n#include \"a.glsl\"\ny", res, nullptr);
    const bool cycle = !expandShaderIncludes("#include \"self.glsl\"", res, &e1) && e1.find("cycle") != std::string::npos;
    const bool missing = !expandShaderIncludes("#include \"nope.glsl\"", res, &e2) && e2.find("not found") != std::string::npos;
    check(ok && *ok == "x\nA\ny" && cycle && missing, "shader includes: expanded in place; cycles and missing files are errors");
  }
  {
    PackStack s3;
    s3.push(*ResourcePack::open(base, &err));
    const ResourceSet before = loadResources(s3);
    writePng(base / "textures/a.png", 4, 4, 0x410000FFu);
    const RegistryDiff d = diffRegistries(before.textures, loadResources(s3).textures);
    check(d.changed == std::vector<std::string>{"a"} && d.added.empty() && d.removed.empty(),
          "hot reload diff: reports exactly the changed texture");
  }
  {
    // Litho SVG ----------------------------------------------------------
    const fs::path fixture = fs::path(__FILE__).parent_path() / "testdata" / "cistercian_1.svg";
    std::string serr;
    auto nearViolet = [](const uint8_t* px) {
      const int dr = int(px[0]) - 0xA9, dg = int(px[1]) - 0x70, db = int(px[2]) - 0xFF;
      return px[3] > 200 && dr * dr + dg * dg + db * db < 14 * 14 * 3;
    };
    auto img64 = Image::loadSvg(fixture, 64, 64, &serr);
    int opaque = 0;
    bool violetFound = false;
    if (img64)
      for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
          const uint8_t* px = img64->pixel(x, y);
          if (px[3] > 0) ++opaque;
          if (nearViolet(px)) violetFound = true;
        }
    check(img64 && img64->width() == 64 && img64->height() == 64 && opaque > 0 && violetFound &&
              img64->pixel(0, 0)[3] == 0,
          "litho svg: cistercian_1.svg decodes at 64x64, violet stroke pixels present, corners transparent");
    const std::string badBytes("<svg><g><oops");
    const std::string notSvg("this is not an svg at all");
    auto asBytes = [](const std::string& t) {
      return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(t.data()), t.size());
    };
    const auto badImg = Image::decodeSvg(asBytes(badBytes), 64, 64, &serr);
    const auto notImg = Image::decodeSvg(asBytes(notSvg), 64, 64, &serr);
    check(!badImg && !notImg && !serr.empty(), "litho svg: malformed input -> nullopt + err");
    auto img32 = Image::loadSvg(fixture, 32, 32, &serr);
    check(img32 && img32->width() == 32 && img64 && img64->hash() != img32->hash(),
          "litho svg: 32 and 64 rasterizations both succeed with different buffers");
    auto intr = Image::load(fixture, &serr);
    check(intr && intr->width() == 100 && intr->height() == 140,
          "litho svg: Image::load routes .svg by extension at intrinsic size (100x140)");
    const fs::path svgpack = root / "svgpack";
    writeFile(svgpack / "pack.json", R"({"name": "svgpack"})");
    fs::create_directories(svgpack / "textures", ec);
    fs::copy_file(fixture, svgpack / "textures" / "sigil.svg", ec);
    writeFile(svgpack / "resources.json",
              R"({"version": 1, "tile_size": 32, "textures": {"sigil": "textures/sigil.svg"}})");
    PackStack ssvg;
    auto svgpackOpen = ResourcePack::open(svgpack, &serr);
    check(svgpackOpen.has_value(), "litho svg: svg test pack opens");
    if (svgpackOpen) ssvg.push(std::move(*svgpackOpen));
    clearSharedSvgRasterCache();
    const ResourceSet rsvg = loadResources(ssvg);
    const TextureId sid = rsvg.textures.id("sigil");
    int sigilOpaque = 0;
    if (!sid.missing()) {
      const Image& im = rsvg.textures.texture(sid).image;
      for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x)
          if (im.pixel(x, y)[3] > 0) ++sigilOpaque;
    }
    const ResourceSet rsvg2 = loadResources(ssvg);  // cached: no re-rasterize
    const TextureId sid2 = rsvg2.textures.id("sigil");
    check(!sid.missing() && rsvg.textures.texture(sid).image.width() == 32 && sigilOpaque > 0 &&
              !sid2.missing() && rsvg2.textures.texture(sid2).image == rsvg.textures.texture(sid).image,
          "litho svg: .svg texture loads through loadImage at pack tile size into the registry; reload identical (cache)");
  }
  {
    // Litho certificate (provenance) --------------------------------------
    const fs::path fixture = fs::path(__FILE__).parent_path() / "testdata" / "cistercian_1.svg";
    std::string perr;
    std::ifstream fin(fixture, std::ios::binary);
    const std::vector<uint8_t> fbytes((std::istreambuf_iterator<char>(fin)),
                                      std::istreambuf_iterator<char>());
    const uint32_t fcrc = lithoCrc32(fbytes);
    char hexbuf[11];
    std::snprintf(hexbuf, sizeof hexbuf, "0x%08x", fcrc);
    const fs::path provpack = root / "provpack";
    writeFile(provpack / "pack.json", R"({"name": "provpack"})");
    fs::create_directories(provpack / "textures", ec);
    fs::copy_file(fixture, provpack / "textures" / "cert.svg", ec);
    std::vector<uint8_t> tbytes = fbytes;  // tampered: one flipped byte (direct-API check only)
    tbytes[tbytes.size() / 2] ^= 0x01;
    Json man = Json::object();
    man.add("version", Json::number(1));
    man.add("tile_size", Json::number(32));
    Json& tex = man.add("textures", Json::object());
    Json& good = tex.add("good", Json::object());
    good.add("file", Json::string("textures/cert.svg"));
    Json& goodProv = good.add("provenance", Json::object());
    goodProv.add("origin", Json::string("litho-generated"));
    goodProv.add("crc32", Json::string(hexbuf));
    tex.add("plain", Json::string("textures/cert.svg"));
    Json& bad = tex.add("badcrc", Json::object());
    bad.add("file", Json::string("textures/cert.svg"));
    Json& badProv = bad.add("provenance", Json::object());
    badProv.add("origin", Json::string("litho-generated"));
    badProv.add("crc32", Json::string("0xdeadbeef"));
    writeFile(provpack / "resources.json", man.dump());
    PackStack sp;
    auto provOpen = ResourcePack::open(provpack, &perr);
    check(provOpen.has_value(), "provenance: test pack opens");
    if (provOpen) sp.push(std::move(*provOpen));
    auto parseDef = [&](const std::string& text) {
      Json j; return Json::parse(text, j, &perr) ? std::optional<Json>(std::move(j)) : std::nullopt;
    };
    auto goodDef = parseDef(std::string(R"({"file": "textures/cert.svg", "provenance": {"origin": "litho-generated", "crc32": ")") + hexbuf + "\"}}");
    auto plainDef = parseDef(R"({"file": "textures/cert.svg"})");
    auto badDef = parseDef(R"({"file": "textures/cert.svg", "provenance": {"origin": "litho-generated", "crc32": "0xdeadbeef"}})");
    check(goodDef && plainDef && badDef, "provenance: def json parses");
    if (goodDef && plainDef && badDef) {
      check(verifyTextureProvenance(sp, "textures/cert.svg", *goodDef, perr) == Provenance::Verified,
            "provenance: correct marker -> Verified");
      check(verifyTextureProvenance(sp, "textures/cert.svg", *plainDef, perr) == Provenance::Absent,
            "provenance: no marker -> Absent (ordinary third-party)");
      check(verifyTextureProvenance(sp, "textures/cert.svg", *badDef, perr) == Provenance::Mismatch,
            "provenance: wrong crc32 -> Mismatch");
      check(lithoCrc32(tbytes) != fcrc, "provenance: tampered bytes hash differently (sanity)");
    }
    clearSharedSvgRasterCache();
    const ResourceSet rprov = loadResources(sp);
    const TextureId gid = rprov.textures.id("good");
    int goodOpaque = 0;
    if (!gid.missing()) {
      const Image& im = rprov.textures.texture(gid).image;
      for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x)
          if (im.pixel(x, y)[3] > 0) ++goodOpaque;
    }
    bool badWarned = false;
    for (const auto& w : rprov.warnings)
      if (w.find("badcrc") != std::string::npos && w.find("provenance") != std::string::npos &&
          w.find("third-party") != std::string::npos)
        badWarned = true;
    const bool badLoaded = !rprov.textures.id("badcrc").missing();
    check(!gid.missing() && rprov.textures.texture(gid).image.width() == 32 && goodOpaque > 0,
          "provenance: certified .svg loads at pack tile size with content");
    check(badWarned && badLoaded,
          "provenance: mismatch warns 'treated as third-party' but the texture still loads");
  }
  // Mod signature verification (Worker B).
  {
    const fs::path sigRoot = root / "sigtest";
    // Test 1: unsigned mod (no signature.json) -> Unsigned.
    {
      const fs::path modDir = sigRoot / "unsigned_mod";
      writeFile(modDir / "mod.json", R"({"id":"test_unsigned","version":"1.0.0"})");
      writeFile(modDir / "data.txt", "hello");
      std::string err;
      auto st = assets::verifyModSignature(modDir, &err);
      check(st == SignatureStatus::Unsigned, "sig: no signature.json -> Unsigned");
    }
    // Test 2: verified mod (ephemeral keypair, generated per run — no private key
    // material in the tree) -> Verified via verifyModSignatureAgainstKey.
    {
      const fs::path modDir = sigRoot / "verified_mod";
      writeFile(modDir / "mod.json", R"({"id":"test_verified","version":"1.0.0"})");
      writeFile(modDir / "data.txt", "hello");
      std::array<uint8_t, 32> ephSeed{}, ephPub{};
      bool ok = crypto::generateKeypair(ephSeed, ephPub);
      std::string signErr;
      if (ok) ok = crypto::signMod(modDir, ephSeed.data(), &signErr);
      check(ok, "sig: test mod signed (ephemeral key)");
      if (ok) {
        std::string err;
        auto st = assets::verifyModSignatureAgainstKey(modDir, ephPub, &err);
        check(st == SignatureStatus::Verified, "sig: valid signature -> Verified");
      }
      writeFile(modDir / "data.txt", "tampered!");
      {
        std::string err;
        auto st = assets::verifyModSignatureAgainstKey(modDir, ephPub, &err);
        check(st == SignatureStatus::Invalid, "sig: tampered mod -> Invalid (refused)");
      }
    }
    // Test 3: wrong key -> Invalid.
    {
      const fs::path modDir = sigRoot / "wrongkey_mod";
      writeFile(modDir / "mod.json", R"({"id":"test_wrongkey","version":"1.0.0"})");
      writeFile(modDir / "data.txt", "hello");
      std::array<uint8_t, 32> otherSeed, otherPub;
      bool ok = crypto::generateKeypair(otherSeed, otherPub);
      std::string signErr;
      if (ok) ok = crypto::signMod(modDir, otherSeed.data(), &signErr);
      check(ok, "sig: other-key mod signed");
      if (ok) {
        std::string err;
        auto st = assets::verifyModSignature(modDir, &err);
        check(st == SignatureStatus::Invalid, "sig: untrusted key -> Invalid (refused)");
      }
    }
    // Test 4: malformed signature.json -> Invalid.
    {
      const fs::path modDir = sigRoot / "badjson_mod";
      writeFile(modDir / "mod.json", R"({"id":"test_badjson","version":"1.0.0"})");
      writeFile(modDir / "signature.json", "{not valid json");
      std::string err;
      auto st = assets::verifyModSignature(modDir, &err);
      check(st == SignatureStatus::Invalid, "sig: malformed signature.json -> Invalid");
    }
    // Test 5: symlink in a signed mod -> Invalid at verify time (P3-3,
    // Appendix E M4). The canonical hash excludes symlinked content the
    // loader would read, so a signed tree containing any link is refused;
    // the same tree verifies without the link (checked below), and
    // signing itself is unaffected.
    if (!test_env::symlinksAvailable()) {
      test_env::skip("sig: signed mod containing a symlink (E M4)", test_env::symlinkSkipReason());
    } else {
      const fs::path modDir = sigRoot / "link_mod";
      writeFile(modDir / "mod.json", R"({"id":"test_link","version":"1.0.0"})");
      writeFile(modDir / "real.txt", "real content");
      std::error_code lec;
      fs::create_symlink("real.txt", modDir / "link.txt", lec);
      check(!lec, "sig: symlink fixture created");
      if (!lec) {
        std::array<uint8_t, 32> seed{}, pub{};
        bool ok = crypto::generateKeypair(seed, pub);
        std::string signErr;
        if (ok) ok = crypto::signMod(modDir, seed.data(), &signErr);
        check(ok, "sig: link mod signed (signing itself is unaffected)");
        if (ok) {
          std::string err;
          auto st = assets::verifyModSignatureAgainstKey(modDir, pub, &err);
          check(st == SignatureStatus::Invalid && err.find("symlink") != std::string::npos,
                "sig: signed mod containing a symlink -> Invalid at verify time (E M4)");
        }
        fs::remove(modDir / "link.txt", lec);
        std::string err;
        auto st = assets::verifyModSignatureAgainstKey(modDir, pub, &err);
        check(st == SignatureStatus::Verified, "sig: same mod verifies once the symlink is gone");
      }
    }
  }
  // resolveMods depth cap (P3-3, Appendix E L5): a 100-deep dependency
  // chain must fail with a recorded error, not recurse unboundedly.
  {
    std::vector<ModManifest> chain;
    for (int i = 0; i < 100; ++i) {
      ModManifest m;
      m.id = "chain_" + std::to_string(i);
      m.version = SemVer{1, 0, 0};
      if (i + 1 < 100) m.deps.push_back(ModManifest::Dep{"chain_" + std::to_string(i + 1), "*"});
      chain.push_back(std::move(m));
    }
    const ModSet set = resolveMods(std::move(chain), {});
    bool depthErr = false;
    for (const std::string& e : set.errors)
      if (e.find("deeper than") != std::string::npos) depthErr = true;
    check(depthErr && set.ordered.size() < 100,
          "mods: dependency chain past depth 64 -> error recorded, walk terminates (E L5)");
    std::vector<ModManifest> shallow;
    for (int i = 0; i < 3; ++i) {
      ModManifest m;
      m.id = "shallow_" + std::to_string(i);
      m.version = SemVer{1, 0, 0};
      if (i + 1 < 3) m.deps.push_back(ModManifest::Dep{"shallow_" + std::to_string(i + 1), "*"});
      shallow.push_back(std::move(m));
    }
    const ModSet ok = resolveMods(std::move(shallow), {});
    check(ok.errors.empty() && ok.ordered.size() == 3 && ok.ordered.front().id == "shallow_2",
          "mods: shallow chain still resolves dependency-first");
  }
  // SvgRasterCache bound + canonical key (P3-3, Appendix E L7).
  {
    SvgRasterCache cache;
    const fs::path svgFile = root / "cachetest" / "a.svg";
    writeFile(svgFile, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\">"
                       "<rect width=\"8\" height=\"8\" fill=\"#ff0000\"/></svg>");
    const Image img = Image::checker(8);
    for (int i = 0; i < 200; ++i) cache.store(svgFile.string(), 1000 + i, img);
    check(cache.size() <= SvgRasterCache::kMaxEntries, "svg cache: bounded at kMaxEntries under 200 stores");
    cache.clear();
    cache.store(svgFile.string(), 16, img);
    const auto viaDot = cache.fetch((root / "cachetest" / "." / "a.svg").string(), 16);
    check(viaDot.has_value(), "svg cache: same file via a different spelling hits the canonical key");
  }
  // Provenance on an animation entry (P3-3, Appendix E L8: was textures-only).
  {
    const fs::path packDir = root / "provpack2";
    writePng(packDir / "textures" / "anim.png", 16, 32, 0xFF2040FF);
    std::vector<uint8_t> bytes;
    {
      std::ifstream in(packDir / "textures" / "anim.png", std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    char crcBuf[16];
    std::snprintf(crcBuf, sizeof crcBuf, "0x%08x", lithoCrc32(bytes));
    writeFile(packDir / "resources.json",
              std::string("{\"tile_size\":16,\"animations\":{\"spin\":{\"file\":\"textures/anim.png\","
                          "\"provenance\":{\"origin\":\"litho-generated\",\"crc32\":\"") +
                  crcBuf + "\"}}}}");
    PackStack stack;
    std::string perr;
    auto pack = ResourcePack::open(packDir, &perr);
    check(pack.has_value(), "provenance: animation pack opens");
    if (pack) {
      stack.push(std::move(*pack));
      const ResourceSet rs = loadResources(stack);
      bool mismatchWarned = false;
      for (const std::string& w : rs.warnings)
        if (w.find("provenance") != std::string::npos) mismatchWarned = true;
      check(!mismatchWarned && rs.textures.findAnimation("spin").has_value(),
            "provenance: valid marker on an animation verifies silently (E L8)");
    }
  }
  // Hostile mod.json dependency ranges (Appendix E M1, P1b): the crafted
  // ranges below used to throw std::out_of_range out of ModManifest::parse
  // (substr on an empty/short string) and abort boot. Now: a clean
  // manifest error from parse(), and loadModDirectory skips the bad mod
  // while the good mod next to it still loads. (If parse threw, this
  // selftest would crash instead of reporting — itself a failure signal.)
  {
    std::string err;
    const auto emptyRange = ModManifest::parse(
        R"({"id":"hostile_empty","version":"1.0.0","deps":[{"id":"x","version":""}]})", &err);
    check(!emptyRange && !err.empty(),
          "mods: dep range \"\" -> manifest error, no exception (App. E M1 repro 1)");
    const auto gtRange = ModManifest::parse(
        R"({"id":"hostile_gt","version":"1.0.0","deps":[{"id":"x","version":">"}]})", &err);
    check(!gtRange && !err.empty(),
          "mods: dep range \">\" -> manifest error, no exception (App. E M1 repro 2)");
    const auto gteRange = ModManifest::parse(
        R"({"id":"hostile_gte","version":"1.0.0","deps":[{"id":"x","version":">="}]})", &err);
    check(!gteRange, "mods: dep range \">=\" (prefix, no version) -> manifest error");
    // Legitimate ranges still parse: *, bare, >=, =, ^, ~.
    bool rangesOk = true;
    for (const char* range : {"*", "1.0.0", ">=1.0.0", "=1.0.0", "^1.0.0", "~1.0.0"}) {
      const std::string text = std::string(R"({"id":"range_ok","version":"1.0.0","deps":[{"id":"x","version":")") +
                               range + R"("}]})";
      if (!ModManifest::parse(text, &err)) rangesOk = false;
    }
    check(rangesOk, "mods: valid dep ranges (*, bare, >=, =, ^, ~) still parse");

    // Boot-scan containment: a directory holding the hostile mod AND a
    // good mod loads the good one and reports the bad one in errors.
    const fs::path modsRoot = root / "modscan";
    writeFile(modsRoot / "badmod" / "mod.json",
              R"({"id":"badmod","version":"1.0.0","deps":[{"id":"x","version":""}]})");
    writeFile(modsRoot / "goodmod" / "mod.json",
              R"({"id":"goodmod","version":"1.0.0"})");
    const ModSet set = loadModDirectory(modsRoot, {});
    bool goodLoaded = false, badReported = false;
    for (const ModManifest& m : set.ordered)
      if (m.id == "goodmod") goodLoaded = true;
    for (const std::string& e : set.errors)
      if (e.find("badmod") != std::string::npos) badReported = true;
    check(goodLoaded && badReported,
          "mods: loadModDirectory skips hostile mod.json, good mod still loads, error reported");
  }
  fs::remove_all(root, ec);
  std::printf(fails ? "ASSETS SELFTEST FAILED (%d)\n" : "ASSETS SELFTEST OK\n", fails);
  return fails;
}

}  // namespace litholattice
