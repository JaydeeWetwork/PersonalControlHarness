// SpriteHost implementation: starter-def registration from every sprite
// source kind, console operations, and the per-frame conversion of the
// sprite system's emitted quads into the render (SpriteDrawQuad) and
// software (PackedVertex) draw forms. See sprite_host.h for the design.
#include "lithokraft/game/sprite_host.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "litholattice/console/concmd.h"
#include "litholattice/core/log.h"
#include "litholattice/procgen/litho_formula_2d.h"
#include "litholattice/sprite/sprite_shade_mode.h"
#include "litholattice/systerian/isysterian.h"
#include "lithokraft/content/blocks.h"  // kCertificateTable
#include "lithokraft/game/resource_manager.h"

namespace lithokraft {
namespace {

using litholattice::Image;
using litholattice::Vec3;
using namespace litholattice::sprite;
namespace lf = litholattice::procgen::litho_formula;

Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

/// A Litho Formula 2D preset rasterized in flat white (tints do the
/// coloring, so one image serves every tinted variant of the shape).
std::optional<Image> formulaImage(const char* preset, int px) {
  const lf::LithoParams* p = lf::lithoPreset2D(preset);
  if (!p) {
    litholattice::logError("sprite: litho preset '%s' not found", preset);
    return std::nullopt;
  }
  lf::LithoStyle2D style;
  style.fill = {255, 255, 255, 255};
  style.strokeWidth = 0.0;
  std::string err;
  std::optional<Image> img = lf::lithoTexture2D(*p, style, px, px, 256, &err);
  if (!img) litholattice::logError("sprite: litho '%s' raster failed: %s", preset, err.c_str());
  return img;
}

/// `src` blitted into `dst` rows [dstY0, dstY0 + src.height()*scale) at
/// scale, centered horizontally and within its frame band (nearest).
void blitScaled(std::vector<uint8_t>& dst, int dstW, const Image& src, int dstY0, float scale) {
  const int dw = std::max(1, int(std::lround(src.width() * scale)));
  const int dh = std::max(1, int(std::lround(src.height() * scale)));
  const int ox = (dstW - dw) / 2, oy = dstY0 + (src.height() - dh) / 2;
  const std::vector<uint8_t>& sp = src.pixels();
  for (int y = 0; y < dh; ++y) {
    const int sy = std::min(src.height() - 1, int(y / scale));
    for (int x = 0; x < dw; ++x) {
      const int sx = std::min(src.width() - 1, int(x / scale));
      const size_t s = (size_t(sy) * src.width() + sx) * 4;
      const size_t d = (size_t(oy + y) * dstW + (ox + x)) * 4;
      std::memcpy(&dst[d], &sp[s], 4);
    }
  }
}

/// The animated starter: a star pulsing 60/80/100/80% over 4 frames of a
/// vertical strip (the assets strip convention the registry splits on).
std::optional<Image> pulseStrip() {
  std::optional<Image> full = formulaImage("star5", 64);
  if (!full) return std::nullopt;
  constexpr int kFrame = 64, kFrames = 4;
  std::vector<uint8_t> px(size_t(kFrame) * kFrame * kFrames * 4, 0);
  const float scales[kFrames] = {0.6f, 0.8f, 1.0f, 0.8f};
  for (int f = 0; f < kFrames; ++f) blitScaled(px, kFrame, *full, f * kFrame, scales[f]);
  return Image(kFrame, kFrame * kFrames, std::move(px));
}

SpriteDef baseDef(const char* name, float w, float h, SpriteShade shade) {
  SpriteDef d;
  d.name = name;
  d.widthBlocks = w;
  d.heightBlocks = h;
  d.shade = shade;
  return d;
}

const char* shadeName(SpriteShade s) {
  switch (s) {
    case SpriteShade::Cutout: return "cutout";
    case SpriteShade::AlphaBlend: return "alpha";
    case SpriteShade::Additive: return "additive";
  }
  return "?";
}

}  // namespace

SpriteHost::SpriteHost() : system_(registry_) {}

void SpriteHost::registerStarterSprites(const ResourceManager& resources) {
  std::string err;
  auto adopted = [&](uint32_t id, const char* what) {
    if (!id) litholattice::logError("sprite: registering %s failed: %s", what, err.c_str());
    return id;
  };

  // Litho Formula 2D stars: ONE generated image, three tinted defs.
  if (std::optional<Image> star = formulaImage("star5", 128)) {
    SpriteDef violet = baseDef("fx:star5", 1.2f, 1.2f, SpriteShade::Cutout);
    violet.tint = {0.663f, 0.439f, 1.0f, 1.0f};  // JD violet #A970FF
    adopted(registry_.registerImage(violet, *star, &err), "fx:star5");
    SpriteDef orange = baseDef("fx:star5_orange", 1.2f, 1.2f, SpriteShade::Cutout);
    orange.tint = {1.0f, 0.435f, 0.0f, 1.0f};  // JD orange #FF6F00
    adopted(registry_.registerImage(orange, *star, &err), "fx:star5_orange");
    SpriteDef pale = baseDef("fx:star5_pale", 1.2f, 1.2f, SpriteShade::Cutout);
    pale.tint = {0.75f, 0.85f, 1.0f, 1.0f};
    adopted(registry_.registerImage(pale, *star, &err), "fx:star5_pale");
  }
  // Litho Formula 2D flower (alpha blend, so its antialiased edge shows).
  if (std::optional<Image> flower = formulaImage("flower", 128)) {
    SpriteDef d = baseDef("fx:flower", 1.4f, 1.4f, SpriteShade::AlphaBlend);
    d.tint = {0.663f, 0.439f, 1.0f, 1.0f};
    adopted(registry_.registerImage(d, *flower, &err), "fx:flower");
  }
  // Litho Formula 2D diamond via SVG text (the registry rasterizes it).
  if (const lf::LithoParams* dp = lf::lithoPreset2D("diamond")) {
    lf::LithoStyle2D style;
    style.fill = {255, 255, 255, 255};
    style.strokeWidth = 0.0;
    SpriteDef d = baseDef("fx:diamond", 1.0f, 1.0f, SpriteShade::Cutout);
    d.tint = {0.45f, 0.8f, 1.0f, 1.0f};
    d.pixelWidth = 96;
    d.pixelHeight = 96;
    adopted(registry_.registerSvg(d, lf::lithoSvg2D(*dp, style, 128), &err), "fx:diamond");
  } else {
    litholattice::logError("sprite: litho preset 'diamond' not found");
  }
  // Systerian glyph 'A' (violet on dark), rasterized by ISysterian.
  {
    litholattice::systerian::Systerian sys;
    std::optional<Image> glyph =
        sys.pngForChar('A', litholattice::systerian::SysterianStyle::violetOnDark(), 100, 140, &err);
    if (glyph) {
      SpriteDef d = baseDef("fx:glyph_a", 1.0f, 1.4f, SpriteShade::Cutout);
      adopted(registry_.registerImage(d, *glyph, &err), "fx:glyph_a");
    } else {
      litholattice::logError("sprite: systerian glyph 'A' failed: %s", err.c_str());
    }
  }
  // Pack PNG provenance: the certificate table's UI icon, decoded by the
  // ResourceManager from textures/ui/icons/certificate_table.png.
  if (const Image* icon = resources.icon(kCertificateTable)) {
    SpriteDef d = baseDef("fx:icon_certificate_table", 1.0f, 1.0f, SpriteShade::Cutout);
    adopted(registry_.registerImage(d, *icon, &err), "fx:icon_certificate_table");
  } else {
    litholattice::logError("sprite: no pack icon for the certificate table block");
  }
  // Animated pulse strip (built in code from the star's frame sequence).
  if (std::optional<Image> strip = pulseStrip()) {
    SpriteDef d = baseDef("fx:pulse", 1.1f, 1.1f, SpriteShade::Additive);
    d.tint = {1.0f, 0.435f, 0.0f, 1.0f};
    d.frameCount = 4;
    d.ticksPerFrame = 6;
    d.loop = true;
    adopted(registry_.registerImage(d, *strip, &err), "fx:pulse");
  }
  // Burst workhorse: a small additive mote.
  if (std::optional<Image> dot = formulaImage("circle", 64)) {
    SpriteDef d = baseDef("fx:spark", 0.28f, 0.28f, SpriteShade::Additive);
    d.tint = {1.0f, 0.6f, 0.15f, 1.0f};
    adopted(registry_.registerImage(d, *dot, &err), "fx:spark");
  }
  registry_.ensureAtlas();
}

std::string SpriteHost::spawnNamed(std::string_view name, Vec3 at, int count) {
  const uint32_t id = registry_.findSprite(name);
  if (!id) return "sprite: unknown def '" + std::string(name) + "' (sprite list)";
  count = std::clamp(count, 1, 64);
  // Deterministic scatter: golden-angle spiral, slight height stagger, so
  // repeated spawns of a def overlap exactly instead of spraying randomly.
  constexpr double kGolden = 2.39996322972865332;
  for (int i = 0; i < count; ++i) {
    const double r = count > 1 ? 0.4 * std::sqrt(double(i)) : 0.0;
    const double a = kGolden * i;
    SpriteSpawn sp;
    sp.position = {at.x + float(r * std::cos(a)), at.y + 0.2f * float(i % 3),
                   at.z + float(r * std::sin(a))};
    sp.gravityScale = 0.f;  // markers hang where they are put
    system_.spawn(id, sp);
  }
  return "sprite: spawned " + std::to_string(count) + " x " + std::string(name) + " (" +
         std::to_string(system_.instanceCount()) + " live)";
}

std::string SpriteHost::burstNamed(std::string_view name, Vec3 at) {
  const uint32_t id = registry_.findSprite(name);
  if (!id) return "sprite: unknown def '" + std::string(name) + "' (sprite list)";
  ParticleEmitterDef ed;
  ed.spriteDef = id;
  ed.burstCount = 28;
  ed.ratePerTick = 0.f;
  ed.durationTicks = 1;  // burst only; the emitter retires on its first tick
  ed.lifetimeMinTicks = 15;
  ed.lifetimeMaxTicks = 40;
  ed.speedMin = 0.04f;
  ed.speedMax = 0.22f;
  ed.direction = {0.f, 1.f, 0.f};
  ed.coneAngleRad = 3.14159265358979323846f;  // full sphere
  ed.gravityScale = 0.25f;
  ed.drag = 0.03f;
  ed.scaleMin = 0.5f;
  ed.scaleMax = 1.2f;
  ed.tintMin = {0.6f, 0.6f, 0.6f, 1.0f};
  ed.tintMax = {1.0f, 1.0f, 1.0f, 1.0f};
  burstSeed_ += 0x9E3779B97F4A7C15ull;  // presentation-only seed stream
  if (!system_.addEmitter(ed, at, burstSeed_))
    return "sprite: burst failed for '" + std::string(name) + "'";
  return "sprite: burst " + std::string(name) + " (" + std::to_string(system_.instanceCount()) +
         " live)";
}

std::string SpriteHost::clearAll() {
  const size_t n = system_.instanceCount();
  std::vector<uint64_t> ids;  // instances() is a live span; despawn mutates it
  ids.reserve(n);
  for (const SpriteInstance& inst : system_.instances()) ids.push_back(inst.id);
  for (uint64_t id : ids) system_.despawn(id);
  return "sprite: cleared " + std::to_string(n) + " instances";
}

std::string SpriteHost::listDefs() const {
  std::string out = "sprite: " + std::to_string(registry_.spriteCount()) + " defs, " +
                    std::to_string(system_.instanceCount()) + " live instances";
  size_t found = 0;
  for (uint32_t id = 1; found < registry_.spriteCount() && id < 100000; ++id) {
    const SpriteDef* d = registry_.spriteDef(id);
    if (!d) continue;
    ++found;
    out += "\n  ";
    out += d->name ? d->name : "?";
    out += "  " + std::to_string(d->widthBlocks) + "x" + std::to_string(d->heightBlocks) + " blocks, ";
    out += shadeName(d->shade);
    if (d->frameCount > 1) out += ", " + std::to_string(d->frameCount) + " frames";
  }
  return out;
}

void SpriteHost::tickDelta(uint32_t ticks) {
  if (ticks) system_.tick(ticks);
}

std::array<float, 3> SpriteHost::avgColorFor(uint32_t defId) {
  if (defId < avgColorByDef_.size() && avgColorByDef_[defId][0] >= 0.f) return avgColorByDef_[defId];
  std::array<float, 3> avg{1.f, 1.f, 1.f};  // fallback: white
  const SpriteUvRect* rect = registry_.frameRect(defId, 0);
  const std::span<const Image> pages = registry_.atlasPages();
  if (rect && rect->page >= 0 && size_t(rect->page) < pages.size()) {
    const Image& page = pages[size_t(rect->page)];
    const int x0 = int(std::lround(rect->u0 * page.width()));
    const int y0 = int(std::lround(rect->v0 * page.height()));
    const int x1 = int(std::lround(rect->u1 * page.width()));
    const int y1 = int(std::lround(rect->v1 * page.height()));
    if (x1 > x0 && y1 > y0) {
      std::vector<uint8_t> px(size_t(x1 - x0) * size_t(y1 - y0) * 4);
      for (int y = y0; y < y1; ++y)
        std::memcpy(&px[size_t(y - y0) * size_t(x1 - x0) * 4],
                    page.pixel(x0, y), size_t(x1 - x0) * 4);
      avg = Image(x1 - x0, y1 - y0, std::move(px)).averageColor();
    }
  }
  if (defId >= avgColorByDef_.size()) avgColorByDef_.resize(defId + 1, {-1.f, -1.f, -1.f});
  avgColorByDef_[defId] = avg;
  return avg;
}

void SpriteHost::buildFrame(Vec3 camPos, Vec3 camFwd, Vec3 camUp) {
  registry_.ensureAtlas();
  const std::span<const SpriteQuad> quads = system_.emitQuads(camPos, camFwd, camUp);
  drawQuads_.clear();
  softwareVerts_.clear();
  drawQuads_.reserve(quads.size());
  // Software-path origin: 16-aligned, 1024 blocks below the camera on each
  // axis, so live sprites land inside the uint16 local range (4096 blocks)
  // on the positive side; anything outside is culled per quad below.
  softwareOrigin_ = {std::floor((camPos.x - 1024.f) / 16.f) * 16.f,
                     std::floor((camPos.y - 1024.f) / 16.f) * 16.f,
                     std::floor((camPos.z - 1024.f) / 16.f) * 16.f};
  for (const SpriteQuad& q : quads) {
    litholattice::SpriteDrawQuad d;
    for (int i = 0; i < 4; ++i) {
      d.corners[i] = q.corners[i];
      d.uv[i][0] = q.uvs[i][0];
      d.uv[i][1] = q.uvs[i][1];
    }
    d.r = q.tint.r;
    d.g = q.tint.g;
    d.b = q.tint.b;
    d.a = q.tint.a;
    d.shade = litholattice::sprite::shadeModeFor(q.shade);
    d.page = q.page;
    d.sortDepth = q.sortDepth;
    drawQuads_.push_back(d);

    // Software (flat) form: brightness = luminance of average color x tint.
    const std::array<float, 3> avg = avgColorFor(q.defId);
    const float lum = std::clamp(0.2126f * avg[0] * q.tint.r + 0.7152f * avg[1] * q.tint.g +
                                     0.0722f * avg[2] * q.tint.b,
                                 0.f, 1.f);
    litholattice::PackedVertex pv[4];
    bool inRange = true;
    for (int i = 0; i < 4; ++i) {
      const Vec3 local = sub(q.corners[i], softwareOrigin_);
      const float lx = local.x * 16.f, ly = local.y * 16.f, lz = local.z * 16.f;
      if (lx < 0.f || ly < 0.f || lz < 0.f || lx > 65535.f || ly > 65535.f || lz > 65535.f)
        inRange = false;
      pv[i].x = uint16_t(std::clamp(lx, 0.f, 65535.f));
      pv[i].y = uint16_t(std::clamp(ly, 0.f, 65535.f));
      pv[i].z = uint16_t(std::clamp(lz, 0.f, 65535.f));
      pv[i].u = 0;
      pv[i].v = 0;
      pv[i].faceAo = 2;  // face 2 = top: faceColor's unshaded (1.0) face
      pv[i].layer = 0;
      pv[i].value = uint8_t(std::lround(lum * 255.f));
      pv[i].mode = litholattice::kShadeTex;
      pv[i].slot = 0;
    }
    if (inRange)
      for (const litholattice::PackedVertex& v : pv) softwareVerts_.push_back(v);
  }
}

void registerSpriteCommands(litholattice::console::ConsoleRegistry& r, SpriteCommandBindings b) {
  using litholattice::console::ConsoleArgs;
  using litholattice::console::ConsoleRegistry;
  std::string err;
  r.registerCommand(
      "sprite",
      "sprite list | sprite spawn <name> [count] | sprite burst <name> | sprite clear",
      [b](ConsoleRegistry& r, const ConsoleArgs& a) {
        if (a.size() == 1 || (a.size() == 2 && a[1] == "list")) {
          r.print(b.list());
          return;
        }
        if (a.size() == 2 && a[1] == "clear") {
          r.print(b.clear());
          return;
        }
        if (a.size() >= 3 && a[1] == "spawn") {
          int count = 1;
          if (a.size() >= 4) {
            try {
              count = std::stoi(a[3]);
            } catch (...) {
              r.print("sprite: count must be a number");
              return;
            }
          }
          r.print(b.spawn(a[2], count));
          return;
        }
        if (a.size() == 3 && a[1] == "burst") {
          r.print(b.burst(a[2]));
          return;
        }
        r.print("usage: sprite list | sprite spawn <name> [count] | sprite burst <name> | sprite clear");
      },
      &err);
}

}  // namespace lithokraft
