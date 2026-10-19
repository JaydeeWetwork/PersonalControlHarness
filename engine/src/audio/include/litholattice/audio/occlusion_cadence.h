// Cadence control for OcclusionField bakes.
//
// WHAT: the field used to re-bake on demand (or once per tick wherever the
// caller remembered to). This scheduler owns the "should I re-bake?" decision:
// re-bake every N ticks, when the listener moves more than X blocks, or when
// the listener enters a new chunk - whichever comes first. Every knob can be
// disabled (ticks: 0 = never; distance: 0 = disabled; chunk-change: false).
// WHY: a full bake is a 6-connected flood over up to 65^3 cells. A stationary
// listener in a static world re-baking every tick burns that cost for
// byte-identical output; a fast-moving listener with a long tick cadence gets
// stale occlusion. The scheduler keeps the field fresh exactly when it can
// have changed.
//
// DEFAULTS (JD's call 2026-10-11): bakeOnChunkChange = true - re-bake only
// when the listener crosses a chunk border. Baking every tick is far too
// expensive for the default path. High-end machines can opt back into
// per-tick bakes with bakeEveryTicks = 1 (documented "ultra" option below).
// moveThreshold = 0 (disabled), radius = 16.
#pragma once
#include <cmath>
#include <cstdint>
#include "litholattice/audio/spatial_audio.h"

namespace litholattice {

/// Bake cadence knobs. All are live-tunable via setCadence().
/// Default policy: bake only when the listener changes chunks.
/// High-end "ultra" option: bakeEveryTicks = 1 restores per-tick bakes.
struct OcclusionCadence {
  uint64_t bakeEveryTicks = 0;  ///< re-bake when tick - lastBake >= this; 0 = never on tick cadence
  float moveThreshold = 0.f;    ///< re-bake early if the listener moved more than this (blocks); 0 = disabled
  bool bakeOnChunkChange = true;  ///< re-bake when the listener enters a new chunk (default policy)
  int radius = 16;              ///< bake radius (clamped by OcclusionField to [1, kMaxRadius])
};

/// Chunk edge length the chunk-change detector uses. Mirrors
/// world::kChunkSize; kept local so audio does not depend on world.
inline constexpr int kCadenceChunkSize = 16;

/// Owns one OcclusionField and decides when it needs a re-bake.
/// Game thread only (bake() mutates; the audio thread reads the field).
class OcclusionBakeScheduler {
 public:
  explicit OcclusionBakeScheduler(OcclusionCadence cadence = {}) : cadence_(cadence) {}

  void setCadence(const OcclusionCadence& c) { cadence_ = c; }
  const OcclusionCadence& cadence() const { return cadence_; }

  /// Maybe re-bake for the listener at (lx,ly,lz) on `tick`.
  /// Returns true when a bake happened. The first call always bakes.
  template <typename IsSolid>
  bool maybeBake(uint64_t tick, float lx, float ly, float lz, IsSolid isSolid) {
    const int vx = int(std::floor(lx));
    const int vy = int(std::floor(ly));
    const int vz = int(std::floor(lz));
    bool due = !field_.baked();
    // H-AUD-6: `tick < lastTick_` (tick counter reset, e.g. world reload) also
    // counts as due — the unsigned subtraction would wrap and force a bake
    // anyway; stating it keeps the intent explicit and the arithmetic safe.
    if (!due && cadence_.bakeEveryTicks > 0 &&
        (tick < lastTick_ || tick - lastTick_ >= cadence_.bakeEveryTicks))
      due = true;
    if (!due && cadence_.moveThreshold > 0.f && field_.baked()) {
      const float dx = lx - lastX_, dy = ly - lastY_, dz = lz - lastZ_;
      const float moved2 = dx * dx + dy * dy + dz * dz;
      const float thresh2 = cadence_.moveThreshold * cadence_.moveThreshold;
      due = moved2 > thresh2;
    }
    if (!due && cadence_.bakeOnChunkChange && field_.baked()) {
      const int cx = int(std::floor(lx / kCadenceChunkSize));
      const int cy = int(std::floor(ly / kCadenceChunkSize));
      const int cz = int(std::floor(lz / kCadenceChunkSize));
      due = (cx != lastCx_ || cy != lastCy_ || cz != lastCz_);
    }
    if (!due) return false;
    field_.bake(vx, vy, vz, isSolid, cadence_.radius);
    lastTick_ = tick;
    lastX_ = lx;
    lastY_ = ly;
    lastZ_ = lz;
    lastCx_ = int(std::floor(lx / kCadenceChunkSize));
    lastCy_ = int(std::floor(ly / kCadenceChunkSize));
    lastCz_ = int(std::floor(lz / kCadenceChunkSize));
    ++bakes_;
    return true;
  }

  /// The (possibly stale) baked field. Valid after the first maybeBake().
  const OcclusionField& field() const { return field_; }
  /// How many bakes have happened (lets callers/tests prove the cadence).
  uint64_t bakes() const { return bakes_; }

 private:
  OcclusionCadence cadence_;
  OcclusionField field_;
  uint64_t lastTick_ = 0;
  uint64_t bakes_ = 0;
  float lastX_ = 0.f, lastY_ = 0.f, lastZ_ = 0.f;
  int lastCx_ = 0, lastCy_ = 0, lastCz_ = 0;
};

}  // namespace litholattice
