// SpriteSystem implementation. See sprite_system.h for the simulation and
// emission ordering rules, and sprite.h for the determinism doctrine.
//
// EMITTER RNG: splitmix64, seeded at addEmitter. Each emitted particle
// consumes EXACTLY 10 draws, in this fixed order:
//   1 lifetime, 2 cone cos(theta), 3 azimuth phi, 4 speed, 5 scale,
//   6 roll, 7..10 tint r/g/b/a.
// Each draw is a uniform float in [0,1) (top 24 bits). Because the draw
// count and order never depend on the def's ranges, a seed + def +
// emission count fully determines every particle, bit-for-bit.
#include "litholattice/sprite/sprite_system.h"

#include <algorithm>
#include <cmath>

namespace litholattice::sprite {
namespace {

Vec3 vsub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 vadd(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 vmul(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

constexpr float kTwoPi = 6.28318530717958647692f;

/// splitmix64 (Stafford / Vigna): a counter-based generator — tiny, fast,
/// and fully portable, so particle fields reproduce identically on every
/// platform and build. next() returns the top 24 bits scaled to [0,1).
float rngUniform(uint64_t& state) {
  state += 0x9E3779B97F4A7C15ull;
  uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z = z ^ (z >> 31);
  return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);
}
float rngRange(uint64_t& state, float lo, float hi) { return lo + (hi - lo) * rngUniform(state); }

uint32_t frameIndexFor(const SpriteDef& def, uint32_t ageTicks) {
  if (def.frameCount <= 1) return 0;
  uint32_t f = ageTicks / (def.ticksPerFrame == 0 ? 1 : def.ticksPerFrame);
  if (def.loop) return f % def.frameCount;
  return std::min(f, def.frameCount - 1);
}

}  // namespace

uint64_t SpriteSystem::spawn(uint32_t defId, const SpriteSpawn& params) {
  const SpriteDef* def = registry_.spriteDef(defId);
  if (!def) return 0;
  SpriteInstance inst;
  inst.id = nextInstanceId_++;
  inst.defId = defId;
  inst.position = params.position;
  inst.velocity = params.velocity;
  inst.lifetimeTicks = params.lifetimeTicks;
  inst.scale = params.scale;
  inst.rollRad = params.rollRad;
  inst.facingYawRad = params.facingYawRad;
  inst.tint = params.tintOverride.has_value() ? *params.tintOverride : def->tint;
  inst.gravityScale = params.gravityScale;
  inst.drag = params.drag;
  instances_.push_back(inst);
  return inst.id;
}

bool SpriteSystem::despawn(uint64_t instanceId) {
  for (auto it = instances_.begin(); it != instances_.end(); ++it) {
    if (it->id == instanceId) {
      instances_.erase(it);  // vector erase is order-stable
      return true;
    }
  }
  return false;
}

uint64_t SpriteSystem::addEmitter(const ParticleEmitterDef& def, Vec3 origin, uint64_t seed) {
  const SpriteDef* sdef = registry_.spriteDef(def.spriteDef);
  if (!sdef) return 0;
  Emitter e;
  e.handle = nextEmitterId_++;
  e.def = def;
  e.origin = origin;
  e.rngState = seed;
  // Emission basis: axis + an orthonormal pair around it.
  Vec3 axis = def.direction;
  if (dot(axis, axis) < 1e-12f) axis = {0.f, 1.f, 0.f};
  e.axis = normalize(axis);
  const Vec3 arb = std::fabs(e.axis.x) > 0.9f ? Vec3{0.f, 0.f, 1.f} : Vec3{1.f, 0.f, 0.f};
  e.basisT = normalize(cross(e.axis, arb));
  e.basisB = cross(e.axis, e.basisT);
  emitters_.push_back(e);
  if (def.burstCount > 0) emitFromEmitter(emitters_.back(), def.burstCount);
  return e.handle;
}

bool SpriteSystem::removeEmitter(uint64_t emitterHandle) {
  for (auto it = emitters_.begin(); it != emitters_.end(); ++it) {
    if (it->handle == emitterHandle) {
      emitters_.erase(it);
      return true;
    }
  }
  return false;
}

void SpriteSystem::emitFromEmitter(Emitter& e, uint32_t count) {
  const SpriteDef* sdef = registry_.spriteDef(e.def.spriteDef);
  if (!sdef) return;
  const uint32_t lifeLo = std::min(e.def.lifetimeMinTicks, e.def.lifetimeMaxTicks);
  const uint32_t lifeHi = std::max(e.def.lifetimeMinTicks, e.def.lifetimeMaxTicks);
  const float cosCone = std::cos(std::clamp(e.def.coneAngleRad, 0.f, kTwoPi * 0.5f));
  for (uint32_t i = 0; i < count; ++i) {
    // Fixed draw order (10 draws — see the header note). Never branch the
    // draw count on the def's ranges.
    const float uLife = rngUniform(e.rngState);
    const float uCos = rngUniform(e.rngState);
    const float uAz = rngUniform(e.rngState);
    const float uSpeed = rngUniform(e.rngState);
    const float uScale = rngUniform(e.rngState);
    const float uRoll = rngUniform(e.rngState);
    const float uTintR = rngUniform(e.rngState);
    const float uTintG = rngUniform(e.rngState);
    const float uTintB = rngUniform(e.rngState);
    const float uTintA = rngUniform(e.rngState);

    SpriteSpawn s;
    s.position = e.origin;
    const uint64_t spanLife = static_cast<uint64_t>(lifeHi - lifeLo) + 1;
    s.lifetimeTicks = lifeLo + static_cast<uint32_t>(uLife * static_cast<float>(spanLife));
    if (s.lifetimeTicks > lifeHi) s.lifetimeTicks = lifeHi;
    const float cosT = 1.f - uCos * (1.f - cosCone);
    const float sinT = std::sqrt(std::max(0.f, 1.f - cosT * cosT));
    const float phi = uAz * kTwoPi;
    const Vec3 dir = vadd(vmul(e.axis, cosT),
                          vmul(vadd(vmul(e.basisT, std::cos(phi)), vmul(e.basisB, std::sin(phi))),
                               sinT));
    s.velocity = vmul(dir, e.def.speedMin + (e.def.speedMax - e.def.speedMin) * uSpeed);
    s.scale = e.def.scaleMin + (e.def.scaleMax - e.def.scaleMin) * uScale;
    s.rollRad = uRoll * kTwoPi;
    s.facingYawRad = phi;
    s.gravityScale = e.def.gravityScale;
    s.drag = e.def.drag;
    // Emitter tint range modulates the def's base tint.
    SpriteTint t;
    t.r = sdef->tint.r * (e.def.tintMin.r + (e.def.tintMax.r - e.def.tintMin.r) * uTintR);
    t.g = sdef->tint.g * (e.def.tintMin.g + (e.def.tintMax.g - e.def.tintMin.g) * uTintG);
    t.b = sdef->tint.b * (e.def.tintMin.b + (e.def.tintMax.b - e.def.tintMin.b) * uTintB);
    t.a = sdef->tint.a * (e.def.tintMin.a + (e.def.tintMax.a - e.def.tintMin.a) * uTintA);
    s.tintOverride = t;
    spawn(e.def.spriteDef, s);
  }
}

void SpriteSystem::tick(uint32_t dtTicks) {
  for (uint32_t step = 0; step < dtTicks; ++step) {
    // (a) Instances, in spawn order: integrate, then age. The tick that
    // brings a finite-lifetime instance's age to its lifetime is its
    // last — it is culled at the end of that tick, having integrated
    // exactly lifetimeTicks times.
    for (SpriteInstance& inst : instances_) {
      inst.velocity = vadd(inst.velocity, vmul(gravity_, inst.gravityScale));
      const float keep = 1.f - std::clamp(inst.drag, 0.f, 1.f);
      inst.velocity = vmul(inst.velocity, keep);
      inst.position = vadd(inst.position, inst.velocity);
      ++inst.ageTicks;
    }
    instances_.erase(std::remove_if(instances_.begin(), instances_.end(),
                                    [](const SpriteInstance& i) {
                                      return i.lifetimeTicks > 0 && i.ageTicks >= i.lifetimeTicks;
                                    }),
                     instances_.end());
    // (b) Emitters, in add order. Newborns first move on the next tick.
    for (Emitter& e : emitters_) {
      if (e.def.durationTicks > 0 && e.age >= e.def.durationTicks) {
        e.dead = true;
        continue;
      }
      e.rateAcc += e.def.ratePerTick;
      const uint32_t n = static_cast<uint32_t>(std::floor(e.rateAcc));
      e.rateAcc -= static_cast<float>(n);
      if (n > 0) emitFromEmitter(e, n);
      ++e.age;
    }
    emitters_.erase(std::remove_if(emitters_.begin(), emitters_.end(),
                                   [](const Emitter& e) { return e.dead; }),
                    emitters_.end());
  }
}

void SpriteSystem::appendQuad(const SpriteInstance& inst, Vec3 cameraPos, Vec3 cameraForward,
                              Vec3 camRight, Vec3 camTrueUp) {
  const SpriteDef* def = registry_.spriteDef(inst.defId);
  if (!def) return;
  const SpriteUvRect* rect = registry_.frameRect(inst.defId, frameIndexFor(*def, inst.ageTicks));
  if (!rect || rect->page < 0) return;

  Vec3 right, up;
  switch (def->billboard) {
    case BillboardMode::Spherical:
      right = camRight;
      up = camTrueUp;
      break;
    case BillboardMode::CylindricalY: {
      const Vec3 d = vsub(inst.position, cameraPos);
      const Vec3 f2{d.x, 0.f, d.z};
      if (dot(f2, f2) < 1e-12f) {
        right = camRight;  // camera on the sprite's vertical axis: fall back
      } else {
        right = normalize(cross(normalize(f2), Vec3{0.f, 1.f, 0.f}));
      }
      up = {0.f, 1.f, 0.f};
      break;
    }
    case BillboardMode::FlatFacing: {
      // Plane normal by the forward convention at pitch 0.
      const Vec3 n{std::cos(inst.facingYawRad), 0.f, std::sin(inst.facingYawRad)};
      right = normalize(cross(Vec3{0.f, 1.f, 0.f}, n));
      up = {0.f, 1.f, 0.f};
      break;
    }
  }
  // In-plane roll (CCW as seen from the front for positive angles).
  if (inst.rollRad != 0.f) {
    const float c = std::cos(inst.rollRad), s = std::sin(inst.rollRad);
    const Vec3 r2 = vadd(vmul(right, c), vmul(up, s));
    const Vec3 u2 = vsub(vmul(up, c), vmul(right, s));
    right = r2;
    up = u2;
  }
  const float hw = def->widthBlocks * inst.scale * 0.5f;
  const float hh = def->heightBlocks * inst.scale * 0.5f;
  const Vec3 c = inst.position;
  const Vec3 rh = vmul(right, hw), uh = vmul(up, hh);

  SpriteQuad q;
  q.corners[0] = vsub(vsub(c, rh), uh);  // bottom-left
  q.corners[1] = vsub(vadd(c, rh), uh);  // bottom-right
  q.corners[2] = vadd(vadd(c, rh), uh);  // top-right
  q.corners[3] = vadd(vsub(c, rh), uh);  // top-left
  // UVs pair with corners; v runs down in image space, so the quad's top
  // corners take the rect's smaller v (the frame's top edge).
  q.uvs[0][0] = rect->u0; q.uvs[0][1] = rect->v1;
  q.uvs[1][0] = rect->u1; q.uvs[1][1] = rect->v1;
  q.uvs[2][0] = rect->u1; q.uvs[2][1] = rect->v0;
  q.uvs[3][0] = rect->u0; q.uvs[3][1] = rect->v0;
  q.tint = inst.tint;
  q.shade = def->shade;
  q.page = rect->page;
  q.sortDepth = dot(vsub(c, cameraPos), cameraForward);
  q.instanceId = inst.id;
  q.defId = inst.defId;
  quadBuffer_.push_back(q);
}

std::span<const SpriteQuad> SpriteSystem::emitQuads(Vec3 cameraPos, Vec3 cameraForward,
                                                    Vec3 cameraUp) {
  registry_.ensureAtlas();
  quadBuffer_.clear();
  const Vec3 camRight = normalize(cross(cameraForward, cameraUp));
  const Vec3 camTrueUp = cross(camRight, cameraForward);

  // Pass 1: cutout in spawn order (order-independent: alpha test + depth
  // write). Pass 2: blend + additive sorted back-to-front, id tiebreak.
  for (const SpriteInstance& inst : instances_) {
    const SpriteDef* def = registry_.spriteDef(inst.defId);
    if (def && def->shade == SpriteShade::Cutout)
      appendQuad(inst, cameraPos, cameraForward, camRight, camTrueUp);
  }
  sortScratch_.clear();
  for (size_t i = 0; i < instances_.size(); ++i) {
    const SpriteDef* def = registry_.spriteDef(instances_[i].defId);
    if (def && def->shade != SpriteShade::Cutout) sortScratch_.push_back(i);
  }
  const Vec3 cp = cameraPos, cf = cameraForward;
  std::sort(sortScratch_.begin(), sortScratch_.end(), [&](size_t a, size_t b) {
    const float da = dot(vsub(instances_[a].position, cp), cf);
    const float db = dot(vsub(instances_[b].position, cp), cf);
    if (da != db) return da > db;  // far to near
    return instances_[a].id < instances_[b].id;
  });
  for (const size_t i : sortScratch_)
    appendQuad(instances_[i], cameraPos, cameraForward, camRight, camTrueUp);
  return quadBuffer_;
}

}  // namespace litholattice::sprite
