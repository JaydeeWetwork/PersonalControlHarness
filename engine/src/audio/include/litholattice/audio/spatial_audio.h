// 3D spatial audio for LithoLattice.
//
// WHAT: Positional audio with distance, panning, occlusion, and reverb.
// WHY: Players hear where sounds come from. SMART MOBS (Daemon) track
//      players by sound — footsteps, echoes, block breaks.
//      This is both a rendering system (player hears) and an AI system (mobs hear).
//
// HOW:
// - SoundEvent: position + type + volume + timestamp
// - Player: distance attenuation (1/d), stereo panning, wall occlusion
// - AI: query "what sounds can mob at X hear?" (with occlusion + hearing range)
// - Reverb: estimated from nearby solid blocks (small room = more reverb)
// - OcclusionField: flood-fill visibility baked once per tick from the
//      listener, then O(1) occlusion queries per voice (amortized path)
#pragma once
#include <cstdint>
#include <vector>
#include <span>
#include <cmath>
#include <algorithm>

namespace litholattice {

/// A sound in the world. Both players and AI mobs can hear these.
struct SoundEvent {
  float x, y, z;        ///< world position (block coordinates)
  uint32_t type;        ///< sound type (footstep, break, explosion, etc.)
  float volume;         ///< base volume 0.0-1.0
  float pitch = 1.0f;   ///< pitch multiplier
  uint64_t tick;        ///< when it happened (for AI memory)
  uint32_t sourceId;    ///< who made it (player ID, mob ID, 0 = environment)
};

/// How a listener perceives a sound.
struct PerceivedSound {
  float volume;    ///< after distance + occlusion (0.0-1.0)
  float pan;       ///< stereo pan (-1.0 left, 1.0 right)
  float reverb;    ///< reverb amount (0.0-1.0)
  float delay;     ///< echo delay in ms (for distant sounds)
};

/// 3D position of a listener (player or mob).
struct Listener {
  float x, y, z;       ///< position
  float dirX, dirZ;    ///< facing direction (normalized, XZ plane)
  float hearingRange;  ///< max distance they can hear (blocks)
};

/// Flood-fill occlusion field: listener-centric visibility baked once,
/// queried per voice. (Research §2.1 UnityVoxelAudio: flood-fill outward
/// from the listener through the voxel grid, store per-cell visibility /
/// distance — voxel-space visibility propagation instead of per-source
/// raycasts.)
///
/// WHAT: a 6-connected BFS from the listener's voxel through air cells.
/// A cell is *visible* iff the flood reaches it; solid cells and
/// unreachable air read as *occluded*. One bake per tick (or on demand)
/// replaces one DDA raycast per voice with O(1) field lookups.
///
/// MEMORY (bounded): dense (2*radius+1)^3 cells x 2 bytes (visibility +
/// BFS depth). radius is clamped to [1, kMaxRadius] (32), so the worst
/// case is 65^3 x 2 B = 549,250 B (~537 KiB) per field. Dense array, not
/// a hash map: iteration order is array order, which determinism needs
/// (hash-map iteration order is unspecified).
///
/// DETERMINISM: the baked bytes are a pure function of (listener voxel,
/// radius, isSolid answers):
///   1. BFS with a FIXED neighbor order (+X,-X,+Y,-Y,+Z,-Z) and a FIFO
///      queue -> identical visitation order for identical inputs;
///   2. integer voxel coordinates only — no floating point in the fill;
///   3. dense row-major array — no hash maps, no address-dependent order.
/// (This assumes the caller's isSolid is itself a pure function of
/// integer coordinates.) checksum() exposes an FNV-1a hash of the baked
/// bytes for lockstep verification across sim clients.
///
/// MODEL NOTE: flood visibility ("is there an air path?") is not the
/// same question as a straight-ray DDA ("does the segment hit solid?").
/// They coincide for open space and full-spanning walls, and differ only
/// at corners/edges (the flood leaks around corners a ray would clip).
/// perceive()/audibleSounds() keep the per-pair IsOccluded callable
/// contract (raycast-style); the OcclusionField overloads answer from the
/// baked field instead.
///
/// Voxel convention: world position -> voxel via floor(), matching
/// voxel_raycast.h. Out-of-field positions read as occluded (conservative:
/// distant unknowns get muffled, never amplified).
///
/// Threading: bake() mutates; all other methods are const and safe for
/// concurrent reads once bake() has returned.
class OcclusionField {
 public:
  /// Maximum bake radius (blocks). Larger radii clamp to this.
  static constexpr int kMaxRadius = 32;

  /// Bake the field from the listener's voxel (lx,ly,lz).
  /// isSolid(x,y,z) answers for integer voxel coordinates.
  /// radius is clamped to [1, kMaxRadius].
  template <typename IsSolid>
  void bake(int lx, int ly, int lz, IsSolid isSolid, int radius);

  /// Is the world position (x,y,z) occluded from the baked listener?
  /// True for solid cells, unreachable air, and out-of-field positions.
  bool occluded(float x, float y, float z) const {
    if (vis_.empty()) return true;
    const int ix = int(std::floor(x));
    const int iy = int(std::floor(y));
    const int iz = int(std::floor(z));
    if (ix < lx_ - radius_ || ix > lx_ + radius_ ||
        iy < ly_ - radius_ || iy > ly_ + radius_ ||
        iz < lz_ - radius_ || iz > lz_ + radius_)
      return true;  // outside the baked box: conservative
    return vis_[index(ix, iy, iz)] != kVisible;
  }

  /// Drop-in IsOccluded(x0,y0,z0,x1,y1,z1) adapter for the per-pair
  /// perceive()/audibleSounds() templates. Answers from the baked field;
  /// the listener arguments are ignored (the field is listener-baked).
  /// The field must outlive the returned callable.
  auto query() const {
    return [this](float, float, float, float x1, float y1, float z1) {
      return occluded(x1, y1, z1);
    };
  }

  /// Radius the field was actually baked with (after clamping).
  int radius() const { return radius_; }
  /// True after bake() has run at least once.
  bool baked() const { return !vis_.empty(); }
  /// Was this field baked for the listener voxel (x,y,z)?
  bool bakedFor(int x, int y, int z) const {
    return baked() && x == lx_ && y == ly_ && z == lz_;
  }

  /// Enclosure ratio 0.0 (open) .. 1.0 (fully solid) over the baked box —
  /// the same idea as SpatialAudio::estimateReverb(), measured at bake
  /// radius instead of a fixed 5x5x5 cube, so reverb-style queries can
  /// reuse the bake instead of re-scanning geometry.
  float enclosure() const { return enclosure_; }

  /// BFS depth (in cells) from the listener voxel, or -1 if unreachable.
  /// Matches the research guidance to store per-cell distance.
  int distance(float x, float y, float z) const {
    if (vis_.empty()) return -1;
    const int ix = int(std::floor(x));
    const int iy = int(std::floor(y));
    const int iz = int(std::floor(z));
    if (ix < lx_ - radius_ || ix > lx_ + radius_ ||
        iy < ly_ - radius_ || iy > ly_ + radius_ ||
        iz < lz_ - radius_ || iz > lz_ + radius_)
      return -1;
    const uint8_t d = dist_[index(ix, iy, iz)];
    return d == 0xFF ? -1 : int(d);
  }

  /// FNV-1a 64-bit hash over (listener, radius, baked bytes).
  /// Deterministic across runs/platforms (assumes 32-bit int).
  uint64_t checksum() const {
    uint64_t h = 1469598103934665603ull;
    const uint64_t prime = 1099511628211ull;
    auto mix64 = [&](uint64_t v) { h ^= v; h *= prime; };
    mix64(uint64_t(int64_t(lx_)));
    mix64(uint64_t(int64_t(ly_)));
    mix64(uint64_t(int64_t(lz_)));
    mix64(uint64_t(radius_));
    for (uint8_t b : vis_) { h ^= b; h *= prime; }
    for (uint8_t b : dist_) { h ^= b; h *= prime; }
    return h;
  }

  /// Raw baked bytes (for exact-equality / determinism checks).
  std::span<const uint8_t> visBytes() const { return vis_; }
  std::span<const uint8_t> distBytes() const { return dist_; }

 private:
  static constexpr uint8_t kVisible = 1;
  static constexpr uint8_t kSolid = 2;  // occluded; flood does not pass through

  size_t index(int x, int y, int z) const {
    const size_t s = size_t(2 * radius_ + 1);
    return (size_t(z - (lz_ - radius_)) * s + size_t(y - (ly_ - radius_))) * s +
           size_t(x - (lx_ - radius_));
  }

  int lx_ = 0, ly_ = 0, lz_ = 0;
  int radius_ = 0;
  float enclosure_ = 0.0f;
  std::vector<uint8_t> vis_;   // 0 = occluded air, 1 = visible, 2 = solid
  std::vector<uint8_t> dist_;  // BFS depth, 0xFF = unreachable
};

template <typename IsSolid>
void OcclusionField::bake(int lx, int ly, int lz, IsSolid isSolid, int radius) {
  radius_ = radius < 1 ? 1 : (radius > kMaxRadius ? kMaxRadius : radius);
  lx_ = lx;
  ly_ = ly;
  lz_ = lz;
  const int s = 2 * radius_ + 1;
  const size_t n = size_t(s) * size_t(s) * size_t(s);
  vis_.assign(n, 0);
  dist_.assign(n, 0xFF);

  // Pass 1: classify every cell in the box; count solids for enclosure().
  size_t solids = 0;
  for (int dz = -radius_; dz <= radius_; ++dz)
    for (int dy = -radius_; dy <= radius_; ++dy)
      for (int dx = -radius_; dx <= radius_; ++dx) {
        const int x = lx_ + dx, y = ly_ + dy, z = lz_ + dz;
        if (isSolid(x, y, z)) {
          vis_[index(x, y, z)] = kSolid;
          ++solids;
        }
      }
  enclosure_ = float(solids) / float(n);

  // Pass 2: 6-connected BFS from the listener through air cells.
  // Fixed neighbor order (+X,-X,+Y,-Y,+Z,-Z) + FIFO queue => the
  // visitation order, and therefore the baked bytes, are a pure function
  // of the inputs (determinism requirement).
  static constexpr int kNeighbors[6][3] = {
      {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  const size_t start = index(lx_, ly_, lz_);
  if (vis_[start] == kSolid) return;  // listener inside solid: all occluded
  vis_[start] = kVisible;
  dist_[start] = 0;
  struct Cell { int x, y, z; };
  std::vector<Cell> queue;
  queue.reserve(1024);
  queue.push_back({lx_, ly_, lz_});
  size_t head = 0;
  while (head < queue.size()) {
    const Cell c = queue[head++];
    const uint8_t nd = uint8_t(dist_[index(c.x, c.y, c.z)] + 1);
    for (const auto& nb : kNeighbors) {
      const int nx = c.x + nb[0], ny = c.y + nb[1], nz = c.z + nb[2];
      if (nx < lx_ - radius_ || nx > lx_ + radius_ || ny < ly_ - radius_ ||
          ny > ly_ + radius_ || nz < lz_ - radius_ || nz > lz_ + radius_)
        continue;
      const size_t ni = index(nx, ny, nz);
      if (vis_[ni] != 0) continue;  // solid, already visible, or enqueued
      vis_[ni] = kVisible;
      dist_[ni] = nd;
      queue.push_back({nx, ny, nz});
    }
  }
}

/// Spatial audio engine.
class SpatialAudio {
 public:
  /// Emit a sound into the world (heard by players and AI).
  void emit(const SoundEvent& event);

  /// How does a listener perceive a sound?
  /// isOccluded(x0,y0,z0,x1,y1,z1) returns true if walls block the path.
  template <typename IsOccluded>
  PerceivedSound perceive(const SoundEvent& event, const Listener& listener,
                          IsOccluded isOccluded) const {
    return perceiveImpl(event, listener,
                        isOccluded(listener.x, listener.y, listener.z, event.x,
                                   event.y, event.z));
  }

  /// perceive() reading occlusion from a baked OcclusionField (amortized
  /// path): bake once per tick, then one O(1) lookup per voice instead of
  /// one raycast per voice. The field must have been baked for this
  /// listener's voxel; a stale field yields stale occlusion (same caveat
  /// as the occlusion thread's cached gains).
  PerceivedSound perceive(const SoundEvent& event, const Listener& listener,
                          const OcclusionField& field) const {
    return perceiveImpl(event, listener,
                        field.occluded(event.x, event.y, event.z));
  }

  /// What sounds can a mob hear? (for AI)
  /// Returns events within hearing range, sorted by PERCEIVED volume
  /// (loudest first, as the listener actually hears them).
  template <typename IsOccluded>
  std::vector<SoundEvent> audibleSounds(const Listener& mob,
                                        IsOccluded isOccluded) const {
    // H-AUD-5: sort by perceived volume, not the source SoundEvent volume —
    // a distant loud event must not outrank a near quiet one it is actually
    // quieter than at the listener. Perceived volumes are computed once here
    // (perceive() is already run per event for the filter) and carried
    // alongside the events for the sort; ties break on sourceId so the order
    // is deterministic.
    std::vector<std::pair<float, SoundEvent>> scored;
    for (const auto& e : events_) {
      PerceivedSound p = perceive(e, mob, isOccluded);
      if (p.volume > 0.01f) scored.emplace_back(p.volume, e);
    }
    std::sort(scored.begin(), scored.end(),
              [](const std::pair<float, SoundEvent>& a, const std::pair<float, SoundEvent>& b) {
                if (a.first != b.first) return a.first > b.first;
                return a.second.sourceId < b.second.sourceId;
              });
    std::vector<SoundEvent> result;
    result.reserve(scored.size());
    for (auto& s : scored) result.push_back(s.second);
    return result;
  }

  /// audibleSounds() reading occlusion from a baked OcclusionField
  /// (amortized path). Same contract as above; occlusion answers come
  /// from the field instead of per-pair queries.
  std::vector<SoundEvent> audibleSounds(const Listener& mob,
                                        const OcclusionField& field) const {
    // H-AUD-5: perceived-volume sort, same as the callable overload above.
    std::vector<std::pair<float, SoundEvent>> scored;
    for (const auto& e : events_) {
      PerceivedSound p = perceive(e, mob, field);
      if (p.volume > 0.01f) scored.emplace_back(p.volume, e);
    }
    std::sort(scored.begin(), scored.end(),
              [](const std::pair<float, SoundEvent>& a, const std::pair<float, SoundEvent>& b) {
                if (a.first != b.first) return a.first > b.first;
                return a.second.sourceId < b.second.sourceId;
              });
    std::vector<SoundEvent> result;
    result.reserve(scored.size());
    for (auto& s : scored) result.push_back(s.second);
    return result;
  }

  /// Clear old sounds (call each tick to age out).
  void update(uint64_t currentTick, uint64_t maxAge = 512);

  /// Estimate reverb from nearby geometry.
  /// Counts solid blocks in a radius; more enclosure = more reverb.
  template <typename IsSolid>
  float estimateReverb(float x, float y, float z, IsSolid isSolid) const;

 private:
  /// Shared perceive() body: occlusion is pre-resolved to a bool so the
  /// per-pair and baked-field paths compute identical results.
  PerceivedSound perceiveImpl(const SoundEvent& e, const Listener& l,
                              bool occluded) const {
    PerceivedSound p{};

    const float dx = e.x - l.x, dy = e.y - l.y, dz = e.z - l.z;
    const float dist = std::sqrt(dx*dx + dy*dy + dz*dz);

    if (dist > l.hearingRange) return p;  // too far

    // Distance attenuation: 1 / (1 + d/16) — gentle falloff
    float vol = e.volume / (1.0f + dist / 16.0f);

    // Occlusion: walls halve the volume (simplified)
    if (occluded) vol *= 0.5f;

    // Stereo panning: project onto listener's right vector
    // Right = (dirZ, -dirX) for facing (dirX, dirZ)
    const float rightX = l.dirZ, rightZ = -l.dirX;
    float pan = 0;
    if (dist > 0.001f) pan = (dx * rightX + dz * rightZ) / dist;
    pan = std::max(-1.0f, std::min(1.0f, pan));

    // Echo delay for distant sounds (speed of sound ~ 340 m/s, 1 block = 1m)
    const float delayMs = dist / 340.0f * 1000.0f;

    p.volume = vol;
    p.pan = pan;
    p.delay = delayMs;
    // Reverb set by caller via estimateReverb()
    return p;
  }

  std::vector<SoundEvent> events_;
};

// --- Inline implementations ---

template <typename IsSolid>
float SpatialAudio::estimateReverb(float x, float y, float z, IsSolid isSolid) const {
  // Count solid blocks in a 5x5x5 cube around the position.
  // More solid = more enclosed = more reverb.
  int solid = 0, total = 0;
  for (int dy = -2; dy <= 2; ++dy)
    for (int dz = -2; dz <= 2; ++dz)
      for (int dx = -2; dx <= 2; ++dx) {
        if (dx == 0 && dy == 0 && dz == 0) continue;
        ++total;
        if (isSolid(int(x)+dx, int(y)+dy, int(z)+dz)) ++solid;
      }
  return float(solid) / float(total);  // 0.0 (open) to 1.0 (enclosed)
}

}  // namespace litholattice
