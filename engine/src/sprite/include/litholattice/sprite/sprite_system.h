// SpriteSystem: live sprite instances, particle emitters, and per-frame
// quad emission. The system borrows a registry (defs + atlas) and owns the
// instances; it never touches the registry's defs.
//
// SIMULATION: integer ticks only (tick(n) == n single ticks — chunking
// cannot change the result). Per tick, each instance integrates in spawn
// order: velocity += gravity * gravityScale; velocity *= (1 - drag);
// position += velocity; age += 1. An instance with a finite lifetime is
// culled at the end of the tick that brings its age to lifetimeTicks,
// having integrated exactly that many times. Removal is stable:
// survivors keep spawn order.
//
// EMISSION: emitQuads fills a reusable buffer (no allocation in steady
// state) with one quad per live instance. Cutout quads come first, in
// spawn order — alpha-tested sprites write depth, so their draw order is
// irrelevant, and grouping them first lets a renderer draw the buffer
// front-to-back in a single pass. AlphaBlend and Additive quads follow,
// sorted back-to-front by view depth (instance id breaks ties), which is
// the order kShadeAlphaBlend requires; Additive needs no sorting but
// rides the same pass deterministically. The returned span is valid until
// the next mutating call (tick / spawn / despawn / emitQuads).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "litholattice/core/math.h"
#include "litholattice/sprite/sprite.h"

namespace litholattice::sprite {

class SpriteSystem {
 public:
  explicit SpriteSystem(ISpriteRegistry& registry) : registry_(registry) {}

  /// Spawn an instance of a registered sprite def. Returns the instance
  /// handle (never 0), or 0 when the def id is unknown. The instance's
  /// tint resolves now: the spawn's tintOverride when set, else the def's
  /// tint.
  uint64_t spawn(uint32_t defId, const SpriteSpawn& params);
  /// Remove one instance by handle. False when the handle is unknown.
  bool despawn(uint64_t instanceId);
  size_t instanceCount() const { return instances_.size(); }
  /// All live instances, in spawn order.
  std::span<const SpriteInstance> instances() const { return instances_; }

  /// Add a particle emitter at `origin`, driven by `seed`. Returns the
  /// emitter handle (never 0), or 0 when the def's spriteDef is unknown.
  /// The burst is spawned immediately (before the next tick).
  uint64_t addEmitter(const ParticleEmitterDef& def, Vec3 origin, uint64_t seed);
  /// Remove one emitter by handle (its live particles remain). False when
  /// the handle is unknown.
  bool removeEmitter(uint64_t emitterHandle);
  size_t emitterCount() const { return emitters_.size(); }

  /// Advance the whole system by dtTicks sim ticks (see header for the
  /// per-tick order). Emitters emit after instance integration within
  /// each tick, so a newborn particle first moves on the following tick.
  void tick(uint32_t dtTicks);

  /// System gravity: acceleration in blocks per tick^2, applied to every
  /// instance scaled by its gravityScale. Default (0, -0.005, 0) — gentle
  /// in tick space; games tune it.
  void setGravity(Vec3 gravity) { gravity_ = gravity; }
  Vec3 gravity() const { return gravity_; }

  /// Build this frame's quads for a camera at `cameraPos` looking along
  /// `cameraForward` with up `cameraUp` (the camera module's CameraPose
  /// fields drop in directly). See the header for ordering guarantees.
  std::span<const SpriteQuad> emitQuads(Vec3 cameraPos, Vec3 cameraForward, Vec3 cameraUp);

 private:
  struct Emitter {
    uint64_t handle = 0;
    ParticleEmitterDef def;
    Vec3 origin{};
    uint64_t rngState = 0;  // splitmix64 state (see sprite_system.cpp)
    float rateAcc = 0.f;    // fractional emission accumulator
    uint32_t age = 0;       // ticks since added (for durationTicks)
    bool dead = false;      // duration elapsed; removed at the end of the tick
    // Emission basis, precomputed on add (axis normalized; T/B orthonormal).
    Vec3 axis{0.f, 1.f, 0.f}, basisT{1.f, 0.f, 0.f}, basisB{0.f, 0.f, 1.f};
  };
  void emitFromEmitter(Emitter& e, uint32_t count);
  void appendQuad(const SpriteInstance& inst, Vec3 cameraPos, Vec3 cameraForward, Vec3 camRight,
                  Vec3 camTrueUp);

  ISpriteRegistry& registry_;
  std::vector<SpriteInstance> instances_;  // spawn order, always
  std::vector<Emitter> emitters_;          // add order, always
  uint64_t nextInstanceId_ = 1;            // 0 = "no instance" sentinel
  uint64_t nextEmitterId_ = 1;             // 0 = "no emitter" sentinel
  Vec3 gravity_{0.f, -0.005f, 0.f};
  std::vector<SpriteQuad> quadBuffer_;   // reused across frames
  std::vector<size_t> sortScratch_;      // reused index list for the transparent pass
};

}  // namespace litholattice::sprite
