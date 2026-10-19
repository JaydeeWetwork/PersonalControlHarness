// Entities: the engine's entity module (greenfield, 2026-10-10).
//
// The engine owns entity storage, ticking, spatial queries, and save/load.
// The game defines entity types and their behaviors (IEntityBehavior).
// Entities tick on the game thread; the sim thread stays blocks-only.
//
// Binary Pair Principle: every entity carries a `polarity` (uint8). Items are
// 0 (neutral); mobs interpret it per dimension (P/N, +/-, M/F, signal/echo,
// matter/antimatter).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace litholattice {

/// One entity in a dimension.
struct Entity {
  uint64_t id = 0;
  uint32_t type = 0;      ///< game-defined type id
  double x = 0, y = 0, z = 0;
  double vx = 0, vy = 0, vz = 0;
  uint8_t polarity = 0;   ///< binary-pair value (dimension-interpreted)
  uint32_t age = 0;       ///< ticks since spawn
  uint32_t data = 0;      ///< type-specific payload (item: block id; ...)
  uint32_t count = 1;     ///< type-specific count (item stack size)
  bool onGround = false;
  bool dead = false;      ///< set by behavior; manager culls at tick end
};

/// Per-type logic, implemented game-side and registered by type id.
class IEntityBehavior {
 public:
  virtual ~IEntityBehavior() = default;
  /// One game-thread tick (dt seconds). May move the entity, set dead, etc.
  virtual void onTick(Entity& e, double dt) = 0;
};

/// Owns every entity in one dimension.
class EntityManager {
 public:
  using BehaviorFactory = std::function<std::unique_ptr<IEntityBehavior>(uint32_t type)>;

  explicit EntityManager(BehaviorFactory factory) : factory_(std::move(factory)) {}

  /// Spawn an entity; returns its id.
  uint64_t spawn(uint32_t type, double x, double y, double z);
  /// Mark an entity dead (culled at the end of the tick).
  void despawn(uint64_t id);
  /// Mutable access, or nullptr.
  Entity* get(uint64_t id);
  /// All entities within `radius` of (x, y, z), optionally filtered by type.
  /// Returned in id order (never unordered_map bucket order).
  std::vector<Entity*> querySphere(double x, double y, double z, double radius,
                                   uint32_t type = UINT32_MAX);
  /// Advance all entities by dt seconds: behaviors tick in id order, dead are culled.
  /// Exception boundary (H-ENT-5): a throwing behavior does NOT propagate —
  /// the offending entity is marked dead (culled at the end of this same
  /// tick, so the dead-cull and age bookkeeping always complete) and the
  /// remaining entities still tick. Behaviors may rely on this; they do not
  /// need their own try/catch for manager safety.
  void tick(double dt);
  size_t size() const { return entities_.size(); }
  void clear();

 private:
  BehaviorFactory factory_;
  std::unordered_map<uint64_t, Entity> entities_;
  std::unordered_map<uint64_t, std::unique_ptr<IEntityBehavior>> behaviors_;
  uint64_t nextId_ = 1;
  // H-ENT-5: persistent sorted id list, maintained incrementally — spawn
  // appends (ids are issued monotonically, so appending keeps it sorted),
  // the tick-end cull erases the dead, clear() empties it. tick() and
  // querySphere() iterate THIS instead of rebuilding + sorting a fresh
  // vector of every id on every call.
  std::vector<uint64_t> sortedIds_;
};

}  // namespace litholattice
