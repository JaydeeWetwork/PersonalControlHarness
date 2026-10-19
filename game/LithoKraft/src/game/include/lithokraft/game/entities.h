// Game-side entities: item drops (first entity type).
// The engine owns storage/ticking (litholattice::EntityManager); the game defines
// types and behaviors. Rendering is TODO (v1 items are sim-functional, invisible).
#pragma once
#include <functional>
#include "litholattice/entity/entity.h"
#include "litholattice/entity/physics.h"

namespace lithokraft {

constexpr uint32_t kEntityItem = 1;         ///< dropped block item (Entity.data = BlockId)
constexpr uint32_t kEntityFallingBlock = 2;  ///< sand-like falling block (Entity.data = BlockId)
constexpr uint32_t kEntityDeceitor = 3;      ///< Deceitor visual mirror (Entity.data = AiWorld mob id)

/// Item physics: gravity + engine AABB-vs-voxel collision, 8-minute despawn.
/// Magnet + pickup are done by the game (needs the player position).
class ItemBehavior final : public litholattice::IEntityBehavior {
 public:
  using SolidFn = std::function<bool(int x, int y, int z)>;
  using FrictionFn = std::function<float(int x, int y, int z)>;
  ItemBehavior(SolidFn isSolid, FrictionFn friction) : isSolid_(std::move(isSolid)), friction_(std::move(friction)) {}
  void onTick(litholattice::Entity& e, double dt) override {
    ageS_ += float(dt);
    if (ageS_ > 480.f) { e.dead = true; return; }  // 8-minute despawn: 8 on its side is infinity
    if (!e.onGround || e.vy != 0) {
      e.vy -= 22.0 * dt;
      if (e.vy < -24.0) e.vy = -24.0;  // terminal velocity
    }
    // Real AABB-vs-voxel collision (engine physics); 0.3 x 0.3 x 0.3 item.
    litholattice::moveCollide(e, isSolid_, 0.15f, 0.3f, dt);
    if (e.onGround) {
      // Block friction: slippery blocks (ice-like) keep items moving fast for transport lines.
      const float f = friction_(int(std::floor(e.x)), int(std::floor(e.y)) - 1, int(std::floor(e.z)));
      e.vx *= f; e.vz *= f;
    }
  }

 private:
  float ageS_ = 0.f;
  SolidFn isSolid_;
  FrictionFn friction_;
};

/// Falling block: gravity + collision; on landing, places the block and despawns.
/// Lifetime (H-ENT-4): a block that never lands — over the Void or an unloaded
/// column, isSolid reads air forever — must not fall (and leak) without bound.
/// Mirrors the item rule in shape: after kMaxFallSeconds of falling, or below
/// the absolute y floor, it despawns WITHOUT placing (there is nowhere lawful
/// to place it). At terminal velocity 24 b/s, 60 s is ~1,370 blocks of fall —
/// far beyond any real dimension height (768 max), so no legitimate landing
/// is affected.
class FallingBlockBehavior final : public litholattice::IEntityBehavior {
 public:
  using SolidFn = ItemBehavior::SolidFn;
  using PlaceFn = std::function<void(int x, int y, int z, uint32_t blockId)>;
  static constexpr float kMaxFallSeconds = 60.f;
  static constexpr double kYFloor = -1024.0;  // below every dimension's bound
  FallingBlockBehavior(SolidFn isSolid, PlaceFn place) : isSolid_(std::move(isSolid)), place_(std::move(place)) {}
  void onTick(litholattice::Entity& e, double dt) override {
    fallS_ += float(dt);
    if (fallS_ > kMaxFallSeconds || e.y < kYFloor) { e.dead = true; return; }
    e.vy -= 22.0 * dt;
    if (e.vy < -24.0) e.vy = -24.0;
    litholattice::moveCollide(e, isSolid_, 0.5f, 1.0f, dt);  // full-block size
    if (e.onGround) {
      place_(int(std::floor(e.x)), int(std::floor(e.y)), int(std::floor(e.z)), e.data);
      e.dead = true;
    }
  }
 private:
  float fallS_ = 0.f;
  SolidFn isSolid_;
  PlaceFn place_;
};

/// Deceitor visual mirror: the mob's brain and position live in the
/// engine AiWorld (game/deceitor.h); this entity is only its body in
/// the renderer's entity pass. Game::updateDeceitors rewrites its
/// position from the AiWorld mob every frame, so the behavior does
/// nothing (no gravity, no self-motion) — honest placeholder body
/// until a real mob render path exists.
class DeceitorVisualBehavior final : public litholattice::IEntityBehavior {
 public:
  void onTick(litholattice::Entity&, double) override {}
};

/// Factory for game entity behaviors. The game passes its solidity callback so items
/// collide with the real world (thread-safe via SimThread::getBlock).
inline std::unique_ptr<litholattice::IEntityBehavior> makeEntityBehavior(
    uint32_t type, ItemBehavior::SolidFn isSolid, ItemBehavior::FrictionFn friction,
    FallingBlockBehavior::PlaceFn place = {}) {
  if (type == kEntityItem) return std::make_unique<ItemBehavior>(std::move(isSolid), std::move(friction));
  if (type == kEntityFallingBlock) return std::make_unique<FallingBlockBehavior>(std::move(isSolid), std::move(place));
  if (type == kEntityDeceitor) return std::make_unique<DeceitorVisualBehavior>();
  return nullptr;
}

}  // namespace lithokraft
