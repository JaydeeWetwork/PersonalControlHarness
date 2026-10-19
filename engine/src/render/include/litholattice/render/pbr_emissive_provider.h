// World-side emissive enumeration for the PBR many-lights hybrid (PBR phase 2).
//
// WHAT: ChunkEmissiveProvider - the real IEmissiveProvider implementation for the
//   pbr_software_shaders.h seam (and any future GPU path). It scans chunk material
//   bytes across a chunk region around a focus column, resolves each voxel through
//   the column's materialPalette into the MaterialRegistry, and caches every voxel
//   whose material def has emissiveIntensity > 0 as (position, linear rgb, intensity).
// WHY: LightCollector (pbr_lights.h) needs emissive candidates, but the LightEngine
//   retains no source list and the flood fill carries levels, not source identity.
//   This provider is the world-side forEachEmissive the phase-1 seam was waiting for.
// HOW: header-only, mirrors the pbr_lights.h pattern. The scan is CACHED: refresh()
//   rebuilds the candidate vector for a focus column + radius, forEachEmissive walks
//   the cache allocation-free. See the SCAN VS RETAINED decision below.
//
// SCAN VS RETAINED (phase-1 open question #3, locked here): cached scan with explicit
//   refresh (time/event-based invalidation), NOT a retained source list with mutation
//   hooks, and NOT a per-call full scan.
//   - A retained list would be O(emissives) per query, but invalidation needs hooks on
//     EVERY mutation path: ChunkMap::set, direct SubChunk::set writes (worldgen, the
//     sim), save decode, column insert/unload. One missed path = silently wrong lights
//     until reload. Too many write sites to make bulletproof.
//   - A per-call scan is O(volume) per forEachEmissive call; the software shader calls
//     it per fragment, so that is infeasible.
//   - Cached scan: refresh() is O(present sub-chunks in the region) and the game calls
//     it when the focus chunk changes or after edits (markDirty() from the game's
//     single block-edit funnel - the game owns every edit call site, so one explicit
//     dirty flag there cannot drift). Per-query cost is O(cached candidates), the hot
//     path allocates nothing, and worst-case staleness is one refresh interval -
//     explicit and game-controlled, never silent engine drift.
//   - This fits the chunk/tick architecture: chunk data is single-owner (main/sim
//     thread), mutation happens at known points, and rendering reads between ticks.
//     The provider never hooks engine internals, so it cannot break the six
//     optimizations, the sim, or the MATS save path.
//
// DETERMINISM: the scan order is fixed (cx, cz, sy, voxel index all ascending), so the
//   same world state always yields the same enumeration order. LightCollector's ties
//   are stable in enumeration order (phase 1), so equal-distance lights resolve
//   identically across runs.
//
// MATERIAL RESOLUTION (two paths, in order):
//   1. Per-voxel byte path (Worker 1's storage): byte m -> materialPalette[m] (or the
//      built-in default when the column palette is empty / m is out of range) ->
//      registry def -> emissive check. This is the authoritative path once producers
//      (worldgen/edit) write non-default bytes.
//   2. Block-ID fallback path: when the byte resolves to the built-in default - i.e.
//      no producer has assigned this voxel yet - and a BlockMaterialFn is installed,
//      the block's BlockId is mapped to a registry material and THAT def is checked.
//      This keeps torches/lava visible today, while no producer writes material bytes
//      (Worker 1's open question). Explicit bytes always win over the fallback, so
//      when producers arrive the fallback silently stops applying.
//   Air voxels are never candidates. A null/unresolvable def degrades to the fallback
//   path rather than crashing.
//
// THREADING: not thread-safe, like MaterialRegistry. refresh() must happen-before any
//   forEachEmissive() use of the new cache (call refresh on the ChunkMap's owner
//   thread, then hand the provider to the renderer). forEachEmissive itself only reads
//   the cache and is allocation-free.
//
// OWNER-THREAD CONTRACT (P3-3, Appendix D M8 — the review found refresh()
//   scanning the LIVE ChunkMap from the game side with no stated contract):
//   refresh(map, ...) reads mutable voxel storage directly — it is NOT a
//   MeshInput-style snapshot. It is therefore only defined when called on
//   the thread that OWNS the ChunkMap (the sim thread in the threaded
//   configuration; the main thread in the single-threaded one), at a point
//   where that thread itself is not mid-mutation — i.e. between ticks /
//   between game updates, never concurrently with sim edits. Calling it
//   from any other thread while the sim runs races voxel writes (torn or
//   stale light cache at best). The cached CANDIDATES are plain values:
//   after refresh() returns, forEachEmissive()/candidates() may be read
//   from another thread only under an external happens-before edge (the
//   same hand-off the caller already owes for the frame). refresh()
//   records its calling thread in debug builds and asserts that later
//   refreshes stay on it — a cheap tripwire for contract drift.
//
// COST: refresh scans only present (non-null) sub-chunks in the region. Per voxel: one
//   BlockId read + air compare; non-air voxels add a byte read, a palette lookup and a
//   def lookup; the block-ID fallback costs one registry lookup and only runs for
//   default-resolved voxels. The candidate vector is a member: clear() + refill keeps
//   capacity, so steady-state refreshes and every forEachEmissive call allocate
//   nothing. (TickArena exists for transient per-tick scratch, but the cache must
//   outlive the tick, so a retained vector is the right tool here - no arena needed.)
//   Measured: refreshing a 3x3-column region with ~30 present sub-chunks is on the
//   order of tens of microseconds (see testPbrEmissiveEnumeration timing print).
#pragma once
#include <cassert>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>
#include "litholattice/world/block.h"
#include "litholattice/world/chunk.h"
#include "litholattice/world/chunk_map.h"
#include "litholattice/render/pbr_materials.h"
#include "litholattice/render/pbr_software_shaders.h"

namespace litholattice {

/// Default refresh radius in chunks: 1 -> a 3x3 column square around the focus column.
/// Enough to feed K=8 sensibly (analytic lights attenuate as 1/(1+d^2), so distant
/// columns contribute nothing to the specular term anyway); the game may pass a larger
/// radius to refresh() for wide-open scenes.
constexpr int kEmissiveProviderRadiusChunks = 1;

/// One cached emissive voxel: world-space voxel center + linear emission + intensity.
struct EmissiveCandidate {
  int x = 0, y = 0, z = 0;  ///< voxel coords (world)
  float r = 0.f, g = 0.f, b = 0.f;  ///< linear-space emission colour (MaterialDef::emissive)
  float intensity = 0.f;             ///< MaterialDef::emissiveIntensity (> 0 always)
};

/// World-side IEmissiveProvider over chunk material bytes. See the file header for the
/// design decision (cached scan + explicit refresh).
class ChunkEmissiveProvider final : public IEmissiveProvider {
 public:
  /// Maps a voxel's StableId to a compact MaterialRegistry ID. Used ONLY when a voxel's
  /// material byte resolves to the built-in default (no producer assigned it yet). The game
  /// builds this from its BlockRegistry (e.g. tex key -> "<ns>:" + tex -> stable id).
  /// StableId currency: sub-chunk voxels store StableIds (Infinite Registry, phase 1).
  using BlockMaterialFn = std::function<uint16_t(StableId)>;

  /// `registry` must outlive the provider (same contract as PbrFragmentContext).
  explicit ChunkEmissiveProvider(const MaterialRegistry* registry) : registry_(registry) {}

  /// Install the stable-ID -> material fallback (may be null = no fallback).
  void setBlockMaterial(BlockMaterialFn fn) { blockMaterial_ = std::move(fn); dirty_ = true; }

  /// Rebuild the candidate cache for the chunk square [focusCx +/- r] x [focusCz +/- r].
  /// Deterministic order: cx, cz, sub-chunk sy, voxel index all ascending.
  void refresh(const ChunkMap& map, int focusCx, int focusCz,
               int radiusChunks = kEmissiveProviderRadiusChunks) {
#ifndef NDEBUG
    // Owner-thread tripwire (see the contract above): the first refresh
    // pins the owner thread; a later refresh from another thread is a
    // contract violation, asserted here in debug builds only.
    if (!ownerThreadSet_) {
      ownerThread_ = std::this_thread::get_id();
      ownerThreadSet_ = true;
    } else {
      assert(std::this_thread::get_id() == ownerThread_ &&
             "ChunkEmissiveProvider::refresh called off the ChunkMap owner thread");
    }
#endif
    cache_.clear();
    focusCx_ = focusCx;
    focusCz_ = focusCz;
    radius_ = radiusChunks < 0 ? 0 : radiusChunks;
    dirty_ = false;
    if (!registry_) return;
    const uint16_t defaultId = registry_->idFor(kDefaultMaterialStringId);
    const bool haveDefault = defaultId != MaterialRegistry::kInvalidMaterialId;
    for (int cx = focusCx - radius_; cx <= focusCx + radius_; ++cx) {
      for (int cz = focusCz - radius_; cz <= focusCz + radius_; ++cz) {
        const ChunkColumn* col = map.column(cx, cz);
        if (!col) continue;
        for (int sy = 0; sy < kSubChunks; ++sy) {
          const SubChunk* sub = col->subs[size_t(sy)].get();
          if (!sub) continue;  // all air: nothing emissive can hide here
          scanSubChunk(*col, *sub, sy, defaultId, haveDefault);
        }
      }
    }
  }

  /// The game calls this from its block-edit funnel (place/break) so the next refresh
  /// picks the change up. One explicit flag at the game's single edit point is
  /// bulletproof where hooking every engine mutation path would not be.
  void markDirty() { dirty_ = true; }

  /// True when the cache does not cover (focusCx, focusCz): focus moved or markDirty().
  bool needsRefresh(int focusCx, int focusCz) const {
    return dirty_ || focusCx != focusCx_ || focusCz != focusCz_;
  }

  /// Drop the whole cache (e.g. on dimension change); the next needsRefresh() is true.
  void clear() {
    cache_.clear();
    dirty_ = true;
  }

  /// IEmissiveProvider: enumerate cached candidates in deterministic scan order.
  /// Positions are voxel centers in world space; colours are linear-space.
  void forEachEmissive(
      const std::function<void(float x, float y, float z, float r, float g, float b,
                               float intensity)>& sink) const override {
    for (const EmissiveCandidate& c : cache_) {
      sink(static_cast<float>(c.x) + 0.5f, static_cast<float>(c.y) + 0.5f,
           static_cast<float>(c.z) + 0.5f, c.r, c.g, c.b, c.intensity);
    }
  }

  /// Read-only cache access (tests, debug views).
  const std::vector<EmissiveCandidate>& candidates() const { return cache_; }
  size_t candidateCount() const { return cache_.size(); }

 private:
  void scanSubChunk(const ChunkColumn& col, const SubChunk& sub, int sy, uint16_t defaultId,
                    bool haveDefault) {
    const int baseY = sy * kChunkSize;
    const int baseX = col.cx * kChunkSize;
    const int baseZ = col.cz * kChunkSize;
    for (int i = 0; i < kSubChunkVolume; ++i) {
      const StableId sid = sub.blocks.at(size_t(i));
      if (isAirStableId(sid)) continue;
      const uint8_t m = sub.materials[size_t(i)];
      const uint16_t gid =
          (m < col.materialPaletteSize) ? col.materialPalette[size_t(m)]
                                        : (haveDefault ? defaultId : MaterialRegistry::kInvalidMaterialId);
      const MaterialDef* def = registry_->defFor(gid);
      const bool isDefault = (haveDefault && gid == defaultId) || def == nullptr;
      const MaterialDef* use = (def && def->emissiveIntensity > 0.f) ? def : nullptr;
      if (!use && isDefault && blockMaterial_) {
        // All-default case: no producer assigned this voxel yet; ask the stable-ID
        // fallback. Explicit bytes always win.
        const MaterialDef* fdef = registry_->defFor(blockMaterial_(sid));
        if (fdef && fdef->emissiveIntensity > 0.f) use = fdef;
      }
      if (!use) continue;
      EmissiveCandidate c;
      c.x = baseX + (i & 15);
      c.y = baseY + ((i >> 8) & 15);
      c.z = baseZ + ((i >> 4) & 15);
      c.r = use->emissive[0];
      c.g = use->emissive[1];
      c.b = use->emissive[2];
      c.intensity = use->emissiveIntensity;
      cache_.push_back(c);
    }
  }

  const MaterialRegistry* registry_ = nullptr;
  BlockMaterialFn blockMaterial_;
  std::vector<EmissiveCandidate> cache_;
  int focusCx_ = 0, focusCz_ = 0, radius_ = kEmissiveProviderRadiusChunks;
  bool dirty_ = true;
#ifndef NDEBUG
  std::thread::id ownerThread_{};  ///< debug-only owner-thread tripwire (M8)
  bool ownerThreadSet_ = false;
#endif
};

}  // namespace litholattice
