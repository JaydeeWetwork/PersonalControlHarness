// SpriteHost self-test (app selftest section): proves the game-side sprite
// integration end to end without a window —
//   * the starter defs register from every image-source kind the core
//     offers (Image, SVG text, PNG file, PNG bytes), first-wins intact;
//   * tinted variants are distinct defs over one generated image;
//   * spawn / burst / tick / lifetime / clear drive the SpriteSystem;
//   * emitQuads ordering + animation frame advance on sim ticks;
//   * the atlas builds with a version and per-frame rects;
//   * the software (MCP vision) form of a spawned sprite actually lands in
//     a software-rendered frame of real generated Wafer terrain — the
//     capture and the atlas page are written to ~/workspace/user/files/.
#include "lithokraft/game/sprite_host.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "litholattice/render/mesher.h"
#include "litholattice/render/software_renderer.h"
#include "litholattice/sprite/sprite.h"
#include "litholattice/world/chunk_map.h"
#include "litholattice/world/dimension.h"
#include "litholattice/world/mesh_input.h"
#include "lithokraft/content/blocks.h"
#include "lithokraft/game/dimensions.h"
#include "lithokraft/game/resource_manager.h"

namespace lithokraft {
namespace {

using namespace litholattice;
namespace spr = litholattice::sprite;

Mat4 translation(float x, float y, float z) {
  Mat4 t = Mat4::identity();
  t.m[12] = x;
  t.m[13] = y;
  t.m[14] = z;
  return t;
}

std::filesystem::path userFilesDir() {
  if (const char* home = std::getenv("HOME"); home && *home)
    return std::filesystem::path(home) / "workspace" / "user" / "files";
  return "/home/hatch/workspace/user/files";
}

}  // namespace

int runSpriteHostSelfTest(const std::vector<std::filesystem::path>& packDirs) {
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
  };

  ResourceManager rm(packDirs);
  rm.reload();

  SpriteHost host;
  host.registerStarterSprites(rm);
  spr::ISpriteRegistry& reg = host.registry();

  // -- starter defs: every source kind represented -------------------------
  const char* kStarters[] = {"fx:star5",      "fx:star5_orange", "fx:star5_pale",
                             "fx:flower",     "fx:diamond",      "fx:glyph_a",
                             "fx:icon_certificate_table", "fx:pulse", "fx:spark"};
  bool allRegistered = reg.spriteCount() >= 9;
  for (const char* n : kStarters) allRegistered = allRegistered && reg.findSprite(n) != 0;
  check(allRegistered,
        "sprite host: 9 starter defs registered (formula Image x5, SVG diamond, Systerian glyph, pack icon, animated pulse, spark)");

  // -- tinted variants: one image, distinct defs, distinct tints -----------
  const uint32_t starId = reg.findSprite("fx:star5");
  const uint32_t orangeId = reg.findSprite("fx:star5_orange");
  const spr::SpriteDef* starDef = reg.spriteDef(starId);
  const spr::SpriteDef* orangeDef = reg.spriteDef(orangeId);
  check(starId != 0 && orangeId != 0 && starId != orangeId && starDef && orangeDef &&
            starDef->tint != orangeDef->tint,
        "sprite host: fx:star5 and fx:star5_orange are distinct defs (tint is a def property) over the same generated image");

  // -- PNG entry points against the real pack file --------------------------
  {
    const std::string iconFile =
        std::string("textures/ui/icons/") + ResourceManager::iconName(kCertificateTable) + ".png";
    std::filesystem::path found;
    for (auto it = packDirs.rbegin(); it != packDirs.rend(); ++it) {  // highest priority last
      if (std::filesystem::exists(*it / iconFile)) {
        found = *it / iconFile;
        break;
      }
    }
    bool pngOk = false;
    if (!found.empty()) {
      std::string err;
      spr::SpriteDef df = {};
      df.name = "test:icon_pngfile";
      const uint32_t byFile = reg.registerPngFile(df, found, &err);
      std::ifstream in(found, std::ios::binary);
      const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
      spr::SpriteDef db = {};
      db.name = "test:icon_pngbytes";
      const uint32_t byBytes = reg.registerPng(db, bytes, &err);
      pngOk = byFile != 0 && byBytes != 0;
    }
    check(pngOk, "sprite host: PNG file + PNG bytes registration both accept the pack's certificate-table icon");
  }

  // -- first-wins: re-registering a name keeps the original def -------------
  {
    spr::SpriteDef again = {};
    again.name = "fx:star5";
    again.widthBlocks = 99.f;
    const Image tiny(4, 4, std::vector<uint8_t>(4 * 4 * 4, 255));
    const uint32_t id = reg.registerImage(again, tiny, nullptr);
    check(id == starId && reg.spriteDef(id)->widthBlocks == starDef->widthBlocks,
          "sprite host: registration is first-wins by name (re-register keeps the original def)");
  }

  // -- atlas ----------------------------------------------------------------
  reg.ensureAtlas();
  const spr::SpriteUvRect* starRect = reg.frameRect(starId, 0);
  check(reg.atlasVersion() >= 1 && !reg.atlasPages().empty() && starRect && starRect->page == 0 &&
            starRect->u1 > starRect->u0 && starRect->v1 > starRect->v0,
        "sprite host: atlas builds (version >= 1, page 0) with a placed frame rect for fx:star5");

  // -- spawn / burst / tick / emit ------------------------------------------
  host.spawnNamed("fx:star5", {0.f, 100.f, 0.f}, 3);
  host.burstNamed("fx:spark", {0.f, 100.f, 0.f});
  check(host.instanceCount() == 31, "sprite host: spawn 3 + burst 28 = 31 live instances");
  host.tickDelta(10);
  check(host.instanceCount() == 31, "sprite host: all instances survive 10 ticks (spark lifetimes are 15..40)");
  host.buildFrame({0.f, 100.f, 8.f}, {0.f, 0.f, -1.f}, {0.f, 1.f, 0.f});
  const std::vector<SpriteDrawQuad>& dq = host.drawQuads();
  check(dq.size() == 31 && host.softwareVerts().size() == 31 * 4,
        "sprite host: emitQuads -> 31 render quads and 31x4 software verts");
  check(!dq.empty() && dq.front().shade == kShadeCutout && dq.back().shade == kShadeAdditive,
        "sprite host: quad order is cutout first, additive last (the core's emission contract)");

  // -- animation advances on sim ticks (own host: the only quad is the pulse)
  {
    SpriteHost anim;
    anim.registerStarterSprites(rm);
    anim.spawnNamed("fx:pulse", {0.f, 100.f, 0.f}, 1);
    auto frameOrigin = [&]() -> std::pair<float, float> {
      anim.buildFrame({0.f, 100.f, 8.f}, {0.f, 0.f, -1.f}, {0.f, 1.f, 0.f});
      if (anim.drawQuads().empty()) return {-1.f, -1.f};
      const SpriteDrawQuad& q = anim.drawQuads().front();
      return {q.uv[0][0], q.uv[0][1]};
    };
    const auto oA = frameOrigin();
    anim.tickDelta(6);  // fx:pulse: ticksPerFrame 6 -> next frame
    const auto oB = frameOrigin();
    check(oA.first >= 0.f && oB != oA,
          "sprite host: animated def advances its atlas frame after ticksPerFrame sim ticks");
  }

  host.tickDelta(100);
  check(host.instanceCount() == 3,
        "sprite host: burst sparks die on their lifetimes; the immortal star markers remain");
  host.clearAll();
  check(host.instanceCount() == 0, "sprite host: clear removes every instance");

  // -- software-rendered world capture (the MCP vision path) ----------------
  {
    const WorldgenSettings ws = rm.worldgen(1);
    DimensionRegistry dims = createDimensions(ws);
    Dimension* wafer = dims.find("wafer");
    bool sceneOk = wafer && wafer->generator && rm.materials() != nullptr;
    std::vector<ChunkMesh> meshes;
    int surfY = -1;
    if (sceneOk) {
      const BlockRegistry& blocks = blockRegistry();
      ChunkMap map;
      map.setRegistry(&blocks);
      const ColumnGenerator& gen = *wafer->generator;
      for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -1; dz <= 1; ++dz)
          if (auto col = gen.generate(dx, dz)) map.insert(std::move(col));
      for (int y = 255; y > 0; --y)
        if (map.get(0, y, 0) != kAir) {
          surfY = y;
          break;
        }
      MeshContext mctx{blocks, *rm.materials(), nullptr};
      for (int cx = -1; cx <= 1; ++cx)
        for (int cz = -1; cz <= 1; ++cz)
          for (int sy = 0; sy < kSubChunks; ++sy) {
            auto in = snapshotForMeshing(map, blocks, cx, sy, cz, 0, 0);
            if (!in) continue;
            ChunkMesh m = meshSubChunk(*in, mctx);
            if (!m.verts.empty()) meshes.push_back(std::move(m));
          }
      sceneOk = surfY > 0 && !meshes.empty();
    }
    check(sceneOk, "sprite capture: wafer scene generated + meshed for the software render");

    if (sceneOk) {
      SpriteHost world;
      world.registerStarterSprites(rm);
      const uint32_t bigStar = world.registry().findSprite("fx:star5");
      spr::SpriteSpawn sp;
      sp.position = {0.5f, float(surfY) + 5.f, 0.5f};
      sp.scale = 6.f;
      sp.gravityScale = 0.f;
      world.system().spawn(bigStar, sp);
      const Vec3 eye{0.5f, float(surfY) + 5.f, 14.5f};
      const Vec3 fwd{0.f, 0.f, -1.f};
      world.buildFrame(eye, fwd, {0.f, 1.f, 0.f});

      constexpr int kW = 320, kH = 180;
      const Mat4 vp = mul(perspective(1.2f, float(kW) / float(kH), 0.1f, 400.f), lookDir(eye, fwd));
      auto renderScene = [&](bool withSprite) {
        SoftwareRenderer sw;
        sw.setPerPixelShading(false);  // flat: the MCP vision path's mode
        sw.resize(kW, kH);
        sw.clear(0xFF1A1D26);
        for (const ChunkMesh& m : meshes) {
          const Mat4 mvp = mul(vp, translation(float(m.cx * 16), float(m.sy * 16), float(m.cz * 16)));
          sw.renderMesh(m.verts.data(), m.verts.size(), mvp.m);
        }
        if (withSprite && !world.softwareVerts().empty()) {
          const Vec3 o = world.softwareOrigin();
          const Mat4 mvp = mul(vp, translation(o.x, o.y, o.z));
          sw.renderMesh(world.softwareVerts().data(), world.softwareVerts().size(), mvp.m);
        }
        return sw.framebuffer().pixels;
      };
      const std::vector<uint32_t> plain = renderScene(false);
      const std::vector<uint32_t> withStar = renderScene(true);
      size_t diff = 0;
      for (size_t i = 0; i < plain.size(); ++i)
        if (plain[i] != withStar[i]) ++diff;
      check(diff > 500, "sprite capture: the spawned star billboard is visible in the software-rendered world frame");

      // Save the evidence: capture frame + atlas page 0.
      std::vector<uint8_t> rgba(size_t(kW) * kH * 4);
      for (size_t i = 0; i < withStar.size(); ++i) {
        rgba[4 * i + 0] = uint8_t((withStar[i] >> 16) & 0xFF);
        rgba[4 * i + 1] = uint8_t((withStar[i] >> 8) & 0xFF);
        rgba[4 * i + 2] = uint8_t(withStar[i] & 0xFF);
        rgba[4 * i + 3] = 0xFF;
      }
      std::string err;
      std::error_code ec;
      std::filesystem::create_directories(userFilesDir(), ec);
      const bool capSaved =
          Image(kW, kH, std::move(rgba)).savePng(userFilesDir() / "sprite-in-world-2026-10-12.png", &err);
      const auto pages = world.registry().atlasPages();
      const bool atlasSaved = !pages.empty() &&
                              pages[0].savePng(userFilesDir() / "sprite-atlas-2026-10-12.png", &err);
      check(capSaved && atlasSaved,
            "sprite capture: capture PNG + atlas PNG written to ~/workspace/user/files/");
    }
  }

  return fails;
}

}  // namespace lithokraft
