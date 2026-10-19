// LithoLattice projectile: engine-side projectile system (implemented,
// P2c 2026-10-13, review ENT-1/E-H2 — previously this header declared a
// manager with no definitions at all).
//
// Locked design (JD 2026-10-10): projectiles are engine-side, tick with
// the world, and load chunks ahead of their path. How that is realised:
//
//  - ProjectileManager::tick() advances every live projectile by ONE world
//    tick. Velocity is blocks per tick (see Projectile), so tick() takes
//    no dt: the caller drives it once per world tick, exactly like the
//    sim drives its other per-tick systems. There is no gravity and no
//    drag — flight is straight; games that want arcs set vy accordingly
//    per tick from their behavior.
//  - Movement is SWEPT: a tick's travel is split into substeps of at most
//    ~0.5 blocks (the same anti-tunnelling discipline the entity
//    locomotion controller applies around moveCollide), and the voxel at
//    each substep endpoint is tested, so a fast projectile cannot jump
//    through a 1-block wall. A projectile is a POINT (no AABB): the test
//    is "the voxel containing the point", which is the honest point
//    analogue of moveCollide's destination-AABB test.
//  - Hit semantics (minimal, deliberate): on entering a solid voxel, the
//    block's density decides — penetration > density pierces (the
//    projectile continues; the behavior still hears about the hit once
//    for that tick), otherwise the projectile stops AT the contact point
//    (just outside the voxel) and despawns unless its behavior returns
//    true (e.g. a boomerang that turns around on impact). Entity hits
//    come from the optional entity seam and despawn the same way; the
//    owner is never hit by its own projectile.
//  - Despawn rules (exhaustive): explicit despawn(); traveled distance
//    reaching maxRange (for returnsToOwner projectiles the first arrival
//    flips `returning` and reverses the velocity ONCE — or steers at the
//    owner when the owner-position seam is set — and the return leg gets
//    the same maxRange budget before despawning); an unpierced block or
//    entity hit whose behavior does not keep it alive; a behavior
//    returning false on any tick.
//  - Chunk loading ahead: every tick, each projectile requests the chunk
//    columns intersecting the ray from its position along its velocity
//    for kChunkLookaheadBlocks blocks, through the chunk-request SEAM
//    (setChunkRequester). The engine never touches game streaming code;
//    the game implements the seam against its ChunkStreamer (deduplicated
//    there, in-flight-capped, lower priority than the player's own
//    requests because the game issues player requests first each frame).
//    Requests are ALSO deduplicated inside the manager (a column is only
//    forwarded to the seam once until clearRequestedChunks()), so a slow
//    projectile hovering in one column costs one seam call, not one per
//    tick. loadChunksAhead() is the public one-shot form of the same
//    request (spawn-time warm-up, tools, tests).
//
// Seams (all optional; an unset seam degrades honestly):
//  - voxel seam (setVoxelQuery): solidity + density per voxel. Unset =
//    every voxel is air (projectiles fly unimpeded). The game implements
//    it over SimThread::getBlock + the block registry, like its entity
//    behaviors do.
//  - entity seam (setEntityQuery): returns the id of an entity at a
//    point (0 = none). Unset = no entity hits.
//  - owner-position seam (setOwnerPositionQuery): boomerang steering.
//    Unset = return leg is a straight reversal.
//  - chunk seam (setChunkRequester): column requests, see above.
// Behaviors are NOT owned (the game keeps them alive); a null behavior
// means the default semantics above with no callbacks.
#pragma once
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace litholattice {

/// A projectile in flight. The engine ticks it; the game defines damage.
struct Projectile {
  uint64_t id = 0;
  double x = 0, y = 0, z = 0;      ///< position
  double vx = 0, vy = 0, vz = 0;   ///< velocity (blocks per tick)
  uint64_t ownerId = 0;            ///< player/entity that fired it (no self-hit)
  uint16_t damage = 0;             ///< base damage (0-64 scale)
  uint8_t penetration = 0;         ///< vs BlockDef::density (pierces when > density)
  uint8_t dimension = 1;           ///< which dimension it's in
  uint32_t maxRange = 64;          ///< despawn after this many blocks (default 64)
  uint32_t traveled = 0;           ///< blocks traveled so far (whole blocks, floor)
  bool returnsToOwner = false;     ///< boomerang: flies back to owner after maxRange/hit
  bool returning = false;          ///< true when on the return trip
};

/// Projectile behavior: called each tick. Return false to despawn.
class IProjectileBehavior {
 public:
  virtual ~IProjectileBehavior() = default;
  /// Tick the projectile. The engine has already moved it and loaded chunks
  /// ahead of its path. Check for hits here.
  /// @param p the projectile (mutable: update position, etc.)
  /// @param hitBlock true if it hit a block this tick (density check done by engine)
  /// @param hitEntity entity ID hit, or 0
  /// @return false to despawn the projectile
  virtual bool onTick(Projectile& p, bool hitBlock, uint64_t hitEntity) = 0;
};

/// Manages all projectiles. The engine owns this; the game spawns projectiles.
///
/// Threading: single-threaded (the owner's tick thread), like
/// EntityManager. Not thread-safe.
class ProjectileManager {
 public:
  /// Voxel seam result: `solid` = the voxel stops a point projectile;
  /// `density` is consulted only when solid (BlockDef::density currency).
  struct VoxelInfo {
    bool solid = false;
    uint8_t density = 0;
  };
  using VoxelQueryFn = std::function<VoxelInfo(int x, int y, int z)>;
  using EntityQueryFn = std::function<uint64_t(double x, double y, double z, uint64_t excludeId)>;
  using OwnerPositionFn = std::function<bool(uint64_t ownerId, double& x, double& y, double& z)>;
  using ChunkRequestFn = std::function<void(int chunkX, int chunkZ)>;

  /// Lookahead distance for per-tick chunk requests, in blocks.
  static constexpr double kChunkLookaheadBlocks = 48.0;
  /// Swept-movement substep cap, in blocks (moveCollide discipline).
  static constexpr double kMaxSubstepBlocks = 0.5;

  void setVoxelQuery(VoxelQueryFn fn) { voxel_ = std::move(fn); }
  void setEntityQuery(EntityQueryFn fn) { entity_ = std::move(fn); }
  void setOwnerPositionQuery(OwnerPositionFn fn) { ownerPos_ = std::move(fn); }
  void setChunkRequester(ChunkRequestFn fn) { chunkReq_ = std::move(fn); }

  /// Spawn a projectile (behavior not owned; may be null). A zero id in
  /// `p` is assigned the next free id; a caller-supplied id that is
  /// already live is refused (returns 0, nothing changes — first wins).
  uint64_t spawn(const Projectile& p, IProjectileBehavior* behavior);
  void despawn(uint64_t id);
  bool alive(uint64_t id) const { return entries_.find(id) != entries_.end(); }
  const Projectile* get(uint64_t id) const;
  size_t size() const { return entries_.size(); }
  void clear();

  /// Advance every live projectile by one world tick, in id order:
  /// chunk-ahead requests, swept movement, hit resolution, behavior,
  /// despawn rules (see the header comment for the full semantics).
  void tick();

  /// Requests chunk loading along a ray (for projectile paths).
  /// Requests the columns intersecting the segment from (x,y,z) along
  /// (dx,dy,dz) for `distance` blocks, deduplicated against every column
  /// this manager has already requested (see clearRequestedChunks).
  /// (dx,dy,dz) need not be normalized. No-op without a chunk seam, with
  /// a zero direction, or with distance <= 0.
  void loadChunksAhead(double x, double y, double z, double dx, double dy, double dz, uint32_t distance);

  /// Forget the deduplication set (e.g. on dimension switch: the new
  /// dimension's streamer has not seen the old requests).
  void clearRequestedChunks() { requestedChunks_.clear(); }
  size_t requestedChunkCount() const { return requestedChunks_.size(); }

 private:
  struct Entry {
    Projectile p;
    IProjectileBehavior* behavior = nullptr;
    double traveledExact = 0;  ///< fractional accumulator behind p.traveled
  };
  std::unordered_map<uint64_t, Entry> entries_;
  std::vector<uint64_t> order_;  ///< live ids, spawn order (tick order)
  uint64_t nextId_ = 1;
  std::unordered_set<uint64_t> requestedChunks_;  ///< (cx,cz) key set, dedup

  VoxelQueryFn voxel_;
  EntityQueryFn entity_;
  OwnerPositionFn ownerPos_;
  ChunkRequestFn chunkReq_;

  void requestColumn(int cx, int cz);
  void eraseEntry(uint64_t id);
};

}  // namespace litholattice
