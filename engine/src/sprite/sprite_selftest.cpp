// Sprite system self-tests (ctest: engine_sprite). Exit code = failure count.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "litholattice/sprite/sprite_registry.h"
#include "litholattice/sprite/sprite_shade_mode.h"
#include "litholattice/sprite/sprite_system.h"

namespace {

using namespace litholattice::sprite;

int g_fails = 0;
void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fails;
}
bool nearf(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }
bool nearv(litholattice::Vec3 a, litholattice::Vec3 b, float eps = 1e-5f) {
  return nearf(a.x, b.x, eps) && nearf(a.y, b.y, eps) && nearf(a.z, b.z, eps);
}

litholattice::Image solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
    px[i * 4] = r;
    px[i * 4 + 1] = g;
    px[i * 4 + 2] = b;
    px[i * 4 + 3] = a;
  }
  return litholattice::Image(w, h, std::move(px));
}

SpriteDef baseDef(const char* name) {
  SpriteDef d;
  d.name = name;
  return d;
}

/// Pixel at the centre of a frame rect on its atlas page.
const uint8_t* rectCenterPx(SpriteRegistry& reg, const SpriteUvRect& r) {
  const litholattice::Image& page = reg.atlasPages()[r.page];
  const int x = static_cast<int>((r.u0 + r.u1) * 0.5f * page.width());
  const int y = static_cast<int>((r.v0 + r.v1) * 0.5f * page.height());
  return page.pixel(x, y);
}

}  // namespace

int main() {
  using litholattice::Image;
  using litholattice::Vec3;

  // -- Registration: PNG source, first-wins, lookups ------------------------
  {
    SpriteRegistry reg;
    check(reg.atlasPageSize() == 1024, "registry: default page size 1024");
    check(SpriteRegistry(100).atlasPageSize() == 128, "registry: page size rounds up to pow2");

    // PNG fixture produced through the assets encoder (decode(encode(x))
    // exercises the real PNG path without embedding fragile byte arrays).
    const std::vector<uint8_t> png = solid(2, 2, 255, 255, 255, 255).encodePng();
    check(!png.empty(), "png: fixture encodes");

    SpriteDef d = baseDef("test:dot");
    d.widthBlocks = 1.f;
    const uint32_t id = reg.registerPng(d, png);
    check(id != 0, "register: png assigns an id");
    check(reg.findSprite("test:dot") == id, "register: findSprite by name");
    check(reg.spriteDef(id) != nullptr && std::string(reg.spriteDef(id)->name) == "test:dot",
          "register: spriteDef returns the def");

    SpriteDef dup = baseDef("test:dot");
    dup.widthBlocks = 9.f;
    check(reg.registerPng(dup, png) == id, "register: duplicate name returns existing id");
    check(reg.spriteDef(id)->widthBlocks == 1.f, "register: first-wins keeps the original def");
    check(reg.registerImage(dup, solid(2, 2, 0, 0, 0, 255)) == id,
          "register: first-wins holds across source kinds");

    std::string err;
    const uint8_t junk[3] = {1, 2, 3};
    check(reg.registerPng(baseDef("test:junk"), junk, &err) == 0 && !err.empty(),
          "register: undecodable PNG fails with an error");
    check(reg.registerPng(baseDef(""), png) == 0, "register: empty name fails");
    SpriteDef badSize = baseDef("test:badsize");
    badSize.widthBlocks = -1.f;
    check(reg.registerPng(badSize, png) == 0, "register: non-positive world size fails");
    check(reg.findSprite("test:absent") == 0 && reg.spriteDef(9999) == nullptr,
          "register: unknown lookups are inert");
    check(reg.spriteCount() == 1, "register: count tracks successful registrations only");
  }

  // -- Registration: PNG file source ----------------------------------------
  {
    SpriteRegistry reg;
    const auto path = std::filesystem::temp_directory_path() / "litho_sprite_selftest.png";
    std::string err;
    check(solid(3, 3, 10, 20, 30, 255).savePng(path, &err), "png file: fixture writes");
    check(reg.registerPngFile(baseDef("test:file"), path) != 0, "png file: registers from disk");
    std::filesystem::remove(path);
    check(reg.registerPngFile(baseDef("test:gone"), path, &err) == 0 && !err.empty(),
          "png file: missing file fails with an error");
  }

  // -- SVG source: rasterizes into the atlas ---------------------------------
  {
    SpriteRegistry reg;
    SpriteDef d = baseDef("test:red");
    d.pixelWidth = 8;
    d.pixelHeight = 8;
    const char* svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\">"
        "<rect width=\"8\" height=\"8\" fill=\"#FF0000\"/></svg>";
    const uint32_t id = reg.registerSvg(d, svg);
    check(id != 0, "svg: registers");
    check(reg.atlasDirty(), "atlas: dirty after registration");
    reg.ensureAtlas();
    check(!reg.atlasDirty() && reg.atlasVersion() >= 1, "atlas: builds, version bumps");
    const SpriteUvRect* r = reg.frameRect(id, 0);
    check(r != nullptr && r->page == 0, "svg: frame placed on page 0");
    check(r != nullptr && r->u0 >= 0.f && r->u1 <= 1.f && r->v0 >= 0.f && r->v1 <= 1.f &&
              r->u0 < r->u1 && r->v0 < r->v1,
          "svg: uv rect sane and within [0,1]");
    const uint8_t* px = r ? rectCenterPx(reg, *r) : nullptr;
    check(px && px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255,
          "svg: atlas pixels are the SVG's red");

    SpriteDef anim = baseDef("test:svganim");
    anim.frameCount = 2;
    std::string err;
    check(reg.registerSvg(anim, svg, &err) == 0, "svg: multi-frame SVG registration fails");
    check(reg.registerSvg(baseDef("test:badsvg"), "<svg><oops", &err) == 0,
          "svg: malformed SVG fails");
  }

  // -- Raw Image source + atlas packing --------------------------------------
  {
    SpriteRegistry reg(64);
    const uint32_t a = reg.registerImage(baseDef("test:a"), solid(8, 8, 255, 0, 0, 255));
    const uint32_t b = reg.registerImage(baseDef("test:b"), solid(8, 8, 0, 0, 255, 255));
    SpriteDef strip = baseDef("test:strip");
    strip.frameCount = 4;
    const uint32_t c = reg.registerImage(strip, solid(4, 16, 0, 255, 0, 255));
    check(a != 0 && b != 0 && c != 0, "packing: three sprites register");
    reg.ensureAtlas();
    const SpriteUvRect* ra = reg.frameRect(a, 0);
    const SpriteUvRect* rb = reg.frameRect(b, 0);
    check(ra && rb && ra->page == rb->page, "packing: small sprites share a page");
    auto overlap = [](const SpriteUvRect& x, const SpriteUvRect& y) {
      return x.page == y.page && x.u0 < y.u1 && y.u0 < x.u1 && x.v0 < y.v1 && y.v0 < x.v1;
    };
    check(ra && rb && !overlap(*ra, *rb), "packing: frame rects do not overlap");
    const uint8_t* pa = ra ? rectCenterPx(reg, *ra) : nullptr;
    const uint8_t* pb = rb ? rectCenterPx(reg, *rb) : nullptr;
    check(pa && pa[0] == 255 && pa[2] == 0, "packing: sprite A is red in the atlas");
    check(pb && pb[2] == 255 && pb[0] == 0, "packing: sprite B is blue in the atlas");

    bool framesOk = true, distinct = true;
    const SpriteUvRect* prev = nullptr;
    for (uint32_t f = 0; f < 4; ++f) {
      const SpriteUvRect* rf = reg.frameRect(c, f);
      if (!rf) framesOk = false;
      if (rf && prev && overlap(*rf, *prev)) distinct = false;
      if (rf) prev = rf;
    }
    check(framesOk, "packing: every strip frame is placed");
    check(distinct, "packing: strip frames get distinct, non-overlapping rects");
    check(reg.frameRect(c, 4) == nullptr, "packing: frame past the strip is null");

    const uint64_t v = reg.atlasVersion();
    reg.registerImage(baseDef("test:late"), solid(2, 2, 1, 2, 3, 255));
    check(reg.atlasDirty(), "atlas: late registration re-dirties");
    reg.ensureAtlas();
    check(reg.atlasVersion() == v + 1, "atlas: rebuild bumps the version exactly once");
  }

  // -- Instance lifecycle -----------------------------------------------------
  {
    SpriteRegistry reg;
    const uint32_t dot = reg.registerImage(baseDef("test:dot"), solid(2, 2, 255, 255, 255, 255));
    SpriteSystem sys(reg);
    sys.setGravity({0.f, 0.f, 0.f});

    SpriteSpawn s;
    s.position = {0.f, 0.f, 0.f};
    s.velocity = {1.f, 0.f, 0.f};
    s.lifetimeTicks = 3;
    const uint64_t h = sys.spawn(dot, s);
    check(h != 0, "life: spawn returns a handle");
    sys.tick(2);
    check(sys.instanceCount() == 1, "life: alive before its lifetime");
    check(sys.instances()[0].position.x == 2.f, "life: velocity integrates per tick");
    sys.tick(1);
    check(sys.instanceCount() == 0, "life: culled on the tick its age reaches its lifetime");

    SpriteSpawn forever;
    const uint64_t hf = sys.spawn(dot, forever);
    sys.tick(50);
    check(sys.instanceCount() == 1 && sys.instances()[0].ageTicks == 50,
          "life: lifetime 0 is immortal and ages");
    check(sys.spawn(9999, forever) == 0, "life: unknown def spawns nothing");
    check(sys.despawn(hf) && sys.instanceCount() == 0, "life: despawn removes");
    check(!sys.despawn(hf), "life: despawn of a dead handle fails");

    // Spawn-order stability across a middle expiry.
    SpriteSpawn a, b, c;
    const uint64_t ia = sys.spawn(dot, a);
    b.lifetimeTicks = 2;
    const uint64_t ib = sys.spawn(dot, b);
    const uint64_t ic = sys.spawn(dot, c);
    (void)ib;
    sys.tick(2);
    check(sys.instanceCount() == 2 && sys.instances()[0].id == ia && sys.instances()[1].id == ic,
          "life: survivors keep spawn order after a middle expiry");
  }

  // -- Gravity and drag --------------------------------------------------------
  {
    SpriteRegistry reg;
    const uint32_t dot = reg.registerImage(baseDef("test:dot"), solid(2, 2, 255, 255, 255, 255));

    SpriteSystem grav(reg);
    grav.setGravity({0.f, -2.f, 0.f});
    SpriteSpawn s;
    s.velocity = {1.f, 0.f, 0.f};
    grav.spawn(dot, s);
    grav.tick(1);
    check(nearv(grav.instances()[0].velocity, {1.f, -2.f, 0.f}), "gravity: velocity gains g");
    check(nearv(grav.instances()[0].position, {1.f, -2.f, 0.f}), "gravity: position follows");
    grav.tick(1);
    check(nearv(grav.instances()[0].velocity, {1.f, -4.f, 0.f}), "gravity: accumulates");
    check(nearv(grav.instances()[0].position, {2.f, -6.f, 0.f}), "gravity: position integrates");

    SpriteSpawn half = s;
    half.gravityScale = 0.5f;
    SpriteSystem grav2(reg);
    grav2.setGravity({0.f, -2.f, 0.f});
    grav2.spawn(dot, half);
    grav2.tick(1);
    check(nearv(grav2.instances()[0].velocity, {1.f, -1.f, 0.f}), "gravity: scale multiplies g");

    SpriteSystem drag(reg);
    drag.setGravity({0.f, 0.f, 0.f});
    SpriteSpawn ds;
    ds.velocity = {2.f, 0.f, 0.f};
    ds.drag = 0.5f;
    drag.spawn(dot, ds);
    drag.tick(1);
    check(nearv(drag.instances()[0].velocity, {1.f, 0.f, 0.f}) &&
              nearv(drag.instances()[0].position, {1.f, 0.f, 0.f}),
          "drag: velocity halves before the move");
    drag.tick(1);
    check(nearv(drag.instances()[0].position, {1.5f, 0.f, 0.f}), "drag: compounds per tick");

    // Chunking invariance: tick(4) == 4 x tick(1), bit-for-bit.
    SpriteSystem one(reg), four(reg);
    one.setGravity({0.f, -2.f, 0.f});
    four.setGravity({0.f, -2.f, 0.f});
    one.spawn(dot, s);
    four.spawn(dot, s);
    one.tick(4);
    for (int i = 0; i < 4; ++i) four.tick(1);
    check(one.instances()[0].position.x == four.instances()[0].position.x &&
              one.instances()[0].position.y == four.instances()[0].position.y &&
              one.instances()[0].velocity.y == four.instances()[0].velocity.y,
          "gravity: tick chunking cannot change the result");
  }

  // -- Emitters: determinism and rate ------------------------------------------
  {
    SpriteRegistry reg;
    const uint32_t dot = reg.registerImage(baseDef("test:dot"), solid(2, 2, 255, 255, 255, 255));
    ParticleEmitterDef ed;
    ed.spriteDef = dot;
    ed.burstCount = 16;
    ed.lifetimeMinTicks = 100;
    ed.lifetimeMaxTicks = 100;
    ed.speedMin = 0.1f;
    ed.speedMax = 0.3f;
    ed.coneAngleRad = 3.14159265f;

    SpriteSystem s1(reg), s2(reg), s3(reg);
    s1.setGravity({0.f, 0.f, 0.f});
    s2.setGravity({0.f, 0.f, 0.f});
    s3.setGravity({0.f, 0.f, 0.f});
    check(s1.addEmitter(ed, {1.f, 2.f, 3.f}, 42) != 0, "emitter: adds and bursts immediately");
    s2.addEmitter(ed, {1.f, 2.f, 3.f}, 42);
    s3.addEmitter(ed, {1.f, 2.f, 3.f}, 43);
    check(s1.instanceCount() == 16, "emitter: burst count lands at add time");
    bool same = s1.instanceCount() == s2.instanceCount();
    for (size_t i = 0; same && i < s1.instanceCount(); ++i) {
      const SpriteInstance& x = s1.instances()[i];
      const SpriteInstance& y = s2.instances()[i];
      same = x.velocity.x == y.velocity.x && x.velocity.y == y.velocity.y &&
             x.velocity.z == y.velocity.z && x.scale == y.scale && x.rollRad == y.rollRad &&
             x.lifetimeTicks == y.lifetimeTicks;
    }
    check(same, "emitter: same seed reproduces the burst exactly");
    bool differs = false;
    for (size_t i = 0; i < s1.instanceCount() && i < s3.instanceCount(); ++i) {
      const SpriteInstance& x = s1.instances()[i];
      const SpriteInstance& z = s3.instances()[i];
      if (x.velocity.x != z.velocity.x || x.velocity.y != z.velocity.y ||
          x.velocity.z != z.velocity.z)
        differs = true;
    }
    check(differs, "emitter: a different seed gives a different field");
    s1.tick(5);
    s2.tick(5);
    bool stillSame = s1.instanceCount() == s2.instanceCount();
    for (size_t i = 0; stillSame && i < s1.instanceCount(); ++i)
      stillSame = nearv(s1.instances()[i].position, s2.instances()[i].position, 0.f);
    check(stillSame, "emitter: fields stay identical after ticks");

    ParticleEmitterDef rate;
    rate.spriteDef = dot;
    rate.ratePerTick = 2.5f;
    rate.lifetimeMinTicks = rate.lifetimeMaxTicks = 1000;
    SpriteSystem rs(reg);
    const uint64_t eh = rs.addEmitter(rate, {0.f, 0.f, 0.f}, 7);
    rs.tick(2);
    check(rs.instanceCount() == 5, "emitter: fractional rate accumulates (2 + 3 over 2 ticks)");
    check(rs.removeEmitter(eh) && rs.emitterCount() == 0, "emitter: removes by handle");
    ParticleEmitterDef bad = ed;
    bad.spriteDef = 9999;
    check(rs.addEmitter(bad, {0.f, 0.f, 0.f}, 1) == 0, "emitter: unknown sprite def rejected");
  }

  // -- Quad emission -------------------------------------------------------------
  {
    SpriteRegistry reg;
    SpriteDef flat = baseDef("test:flat");
    flat.billboard = BillboardMode::FlatFacing;
    flat.widthBlocks = 2.f;
    flat.heightBlocks = 4.f;
    flat.tint = {0.2f, 0.4f, 0.6f, 0.8f};
    const uint32_t flatId = reg.registerImage(flat, solid(2, 2, 255, 255, 255, 255));
    SpriteDef cyl = baseDef("test:cyl");
    cyl.billboard = BillboardMode::CylindricalY;
    cyl.widthBlocks = 2.f;
    cyl.heightBlocks = 4.f;
    const uint32_t cylId = reg.registerImage(cyl, solid(2, 2, 255, 255, 255, 255));
    SpriteDef glow = baseDef("test:glow");
    glow.shade = SpriteShade::Additive;
    const uint32_t glowId = reg.registerImage(glow, solid(2, 2, 255, 255, 255, 255));
    SpriteDef animDef = baseDef("test:anim");
    animDef.frameCount = 4;
    animDef.ticksPerFrame = 2;
    const uint32_t animId = reg.registerImage(animDef, solid(4, 16, 255, 255, 255, 255));

    const Vec3 camPos{0.f, 0.f, 5.f}, camFwd{0.f, 0.f, -1.f}, camUp{0.f, 1.f, 0.f};

    // Spherical (the flat-named def is spherical? no — flatId is FlatFacing;
    // use a spherical def too).
    SpriteDef sph = baseDef("test:sph");
    sph.widthBlocks = 2.f;
    sph.heightBlocks = 4.f;
    const uint32_t sphId = reg.registerImage(sph, solid(2, 2, 255, 255, 255, 255));

    SpriteSystem sys(reg);
    sys.setGravity({0.f, 0.f, 0.f});
    SpriteSpawn at;
    at.position = {0.f, 0.f, 0.f};
    sys.spawn(sphId, at);
    auto quads = sys.emitQuads(camPos, camFwd, camUp);
    check(quads.size() == 1, "emit: one quad per instance");
    if (quads.size() == 1) {
      const SpriteQuad& q = quads[0];
      float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
      bool facing = true;
      for (const Vec3& c : q.corners) {
        minX = std::min(minX, c.x);
        maxX = std::max(maxX, c.x);
        minY = std::min(minY, c.y);
        maxY = std::max(maxY, c.y);
        if (!nearf(c.z, 0.f)) facing = false;  // every corner in the plane facing the camera
      }
      check(nearf(maxX - minX, 2.f) && nearf(maxY - minY, 4.f), "emit: world size honored");
      check(facing, "emit: spherical quad is perpendicular to the view forward");
      check(nearf(q.sortDepth, 5.f), "emit: sort depth is the view depth");
      check(q.shade == SpriteShade::Cutout && q.page == 0, "emit: shade and page ride the quad");
      check(q.tint == SpriteTint{}, "emit: untinted def tint is identity white");
    }

    // Cylindrical: top edge stays level whatever the camera height.
    SpriteSystem csys(reg);
    csys.setGravity({0.f, 0.f, 0.f});
    csys.spawn(cylId, at);
    const Vec3 cPos{4.f, 3.f, 0.f};
    const Vec3 toSprite = litholattice::normalize(Vec3{0.f - 4.f, 0.f - 3.f, 0.f});
    auto cq = csys.emitQuads(cPos, toSprite, camUp);
    check(cq.size() == 1 && nearf(cq[0].corners[2].y, 2.f) && nearf(cq[0].corners[3].y, 2.f) &&
              nearf(cq[0].corners[0].y, -2.f) && nearf(cq[0].corners[1].y, -2.f),
          "emit: cylindrical top and bottom edges stay level");

    // Flat: the plane is the facing plane (yaw 0 -> normal +X, plane x = 0).
    SpriteSystem fsys(reg);
    fsys.setGravity({0.f, 0.f, 0.f});
    fsys.spawn(flatId, at);
    auto fq = fsys.emitQuads(camPos, camFwd, camUp);
    bool planeOk = fq.size() == 1;
    if (planeOk)
      for (const Vec3& c : fq[0].corners)
        if (!nearf(c.x, 0.f)) planeOk = false;
    check(planeOk, "emit: flat quad lies in its facing plane");
    check(fq.size() == 1 && fq[0].tint == flat.tint, "emit: def tint lands on the quad");

    // Tint override beats the def tint.
    SpriteSystem tsys(reg);
    tsys.setGravity({0.f, 0.f, 0.f});
    SpriteSpawn tinted = at;
    tinted.tintOverride = SpriteTint{0.5f, 0.25f, 1.f, 1.f};
    tsys.spawn(flatId, tinted);
    auto tq = tsys.emitQuads(camPos, camFwd, camUp);
    check(tq.size() == 1 && tq[0].tint == SpriteTint{0.5f, 0.25f, 1.f, 1.f},
          "emit: spawn tint override lands on the quad");

    // Sorting: cutout group first (any depth), then far-to-near.
    SpriteSystem ssys(reg);
    ssys.setGravity({0.f, 0.f, 0.f});
    SpriteSpawn farS, nearS, cutS;
    farS.position = {0.f, 0.f, -10.f};
    nearS.position = {0.f, 0.f, -3.f};
    cutS.position = {0.f, 0.f, -1.f};
    const uint64_t farId = ssys.spawn(glowId, farS);
    const uint64_t nearId = ssys.spawn(glowId, nearS);
    const uint64_t cutId = ssys.spawn(sphId, cutS);
    auto sq = ssys.emitQuads(Vec3{0.f, 0.f, 0.f}, camFwd, camUp);
    check(sq.size() == 3 && sq[0].instanceId == cutId && sq[1].instanceId == farId &&
              sq[2].instanceId == nearId,
          "emit: cutout first, then transparent back-to-front");

    // Animation: the emitted UV rect is the frame for the instance's age.
    SpriteSystem asys(reg);
    asys.setGravity({0.f, 0.f, 0.f});
    asys.spawn(animId, at);
    reg.ensureAtlas();
    const SpriteUvRect* f0 = reg.frameRect(animId, 0);
    const SpriteUvRect* f1 = reg.frameRect(animId, 1);
    auto aq0 = asys.emitQuads(camPos, camFwd, camUp);
    check(aq0.size() == 1 && f0 && nearf(aq0[0].uvs[0][0], f0->u0) && nearf(aq0[0].uvs[0][1], f0->v1),
          "emit: age 0 shows frame 0");
    asys.tick(2);  // age 2, ticksPerFrame 2 -> frame 1
    auto aq1 = asys.emitQuads(camPos, camFwd, camUp);
    check(aq1.size() == 1 && f1 && nearf(aq1[0].uvs[0][0], f1->u0) && nearf(aq1[0].uvs[0][1], f1->v1) &&
              (f0 == nullptr || aq1[0].uvs[0][1] != f0->v1 || aq1[0].uvs[0][0] != f0->u0),
          "emit: age advances the animation frame");
  }

  // -- Shade mapping (the glue header) -------------------------------------------
  check(shadeModeFor(SpriteShade::Cutout) == litholattice::kShadeCutout,
        "shade: Cutout maps to kShadeCutout");
  check(shadeModeFor(SpriteShade::AlphaBlend) == litholattice::kShadeAlphaBlend,
        "shade: AlphaBlend maps to kShadeAlphaBlend");
  check(shadeModeFor(SpriteShade::Additive) == litholattice::kShadeAdditive,
        "shade: Additive maps to kShadeAdditive");

  std::printf("sprite selftest: %s (%d failure%s)\n", g_fails ? "FAILED" : "OK", g_fails,
              g_fails == 1 ? "" : "s");
  return g_fails;
}
