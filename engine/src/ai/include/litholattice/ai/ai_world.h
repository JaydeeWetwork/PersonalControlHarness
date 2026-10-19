// AiWorld: owns the mobs and runs the AI tick (8-16 Hz) with a per-tick path budget, distance
// LOD and a mob cap. Path searches run in parallel but are applied in mob-id order, so the
// outcome never depends on thread count or timing (server-authoritative friendly: all state is
// here, nothing client-side).
#pragma once
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "litholattice/ai/brain.h"
#include "litholattice/ai/hpa.h"
#include "litholattice/ai/nav.h"
#include "litholattice/ai/senses.h"
#include "litholattice/core/threading.h"

namespace litholattice::ai {

struct AiConfig {
  int hz = 10;              ///< AI ticks per second, clamped to 8..16
  int mobCap = 64;          ///< 64 default, at most 128
  int pathsPerTick = 16;    ///< path-search budget per AI tick (excess waits, in id order)
  int nodesPerPath = 3000;   ///< A* expansion budget per search (unreachable goals end as a hop)
  int planRange = 48;       ///< A* box half-size (32-64); longer trips plan in hops
  int lodNear = 32, lodFar = 96;  ///< think every tick within near, every 2 within far, else every 4
  uint64_t seed = 1;
  // P4 scale (2026-10-14): hierarchical planning + flow fields, both
  // DEFAULT OFF (opt-in per world; behaviour is byte-identical with them
  // off). useHpa: goals beyond planRange route over the HPA* entrance
  // graph first (full refined path), falling back to the normal hop.
  // useFlowField: mobs whose goal matches the world's current flow field
  // descend it instead of searching (pack movement). Both consume the
  // plan budget exactly like a search (one plan per mob per tick).
  bool useHpa = false;
  bool useFlowField = false;
  int flowRange = 64;  ///< flow-field box half-size around its goal (JD-review-pending)
};

/// Final-boss slot: hooks a boss encounter (or any game system) can use. Defaults are neutral.
class IBossSlot {
 public:
  virtual ~IBossSlot() = default;
  /// May mob `mob` drop `item` now? (drop gating)
  virtual bool allowDrop(const Mob& /*mob*/, uint32_t /*item*/) const { return true; }
  /// Activity multiplier at AI tick `tick` (surges): >1 = mobs move extra steps.
  virtual float surge(uint64_t /*tick*/) const { return 1.f; }
  /// Learned scorer for boss brains (stub: none).
  virtual const ILearnedScorer* learnedScorer() const { return nullptr; }
};
/// Simple surge schedule: `mult` for `length` ticks every `period` ticks.
class SurgeSchedule final : public IBossSlot {
 public:
  SurgeSchedule(uint64_t period, uint64_t length, float mult) : period_(period), length_(length), mult_(mult) {}
  float surge(uint64_t tick) const override { return period_ && tick % period_ < length_ ? mult_ : 1.f; }
 private:
  uint64_t period_, length_;
  float mult_;
};

struct Mob {
  uint32_t id = 0;
  Cell pos;
  const IMovementMode* mode = nullptr;
  BtPtr brain;                    ///< may be null (idle)
  std::optional<Cell> goal;       ///< set by brains / setGoal; the AI plans toward it
  std::vector<Cell> path;
  /// Step tags parallel to `path` (P1/P2): pathSteps[i] is the edge that
  /// arrives at path[i]. Presentation consumes the tag of the step a mob
  /// just took (pathSteps[pathAt]) to animate the leg -- a parabolic arc
  /// on kJump/kGap. The sim itself never reads the tags.
  std::vector<StepTag> pathSteps;
  size_t pathAt = 0;
  bool wantPath = false;
  int lodEvery = 1;
  uint64_t thoughts = 0;
};

class AiWorld;
struct AiContext {
  AiWorld& world;
  Mob& mob;
  AiRng rng;
  uint64_t tick = 0;
  float surge = 1.f;
  std::string lastAction;
};

struct AiStats {
  uint64_t ticks = 0;
  int mobs = 0, thought = 0, pathsRun = 0, pathsDeferred = 0;
  double tickMs = 0;
};

class AiWorld {
 public:
  AiWorld(const WalkCache& nav, AiConfig cfg, JobPool* pool = nullptr);
  /// nullopt when the cap is reached.
  std::optional<uint32_t> spawn(Cell at, const IMovementMode* mode, BtPtr brain = nullptr);
  bool despawn(uint32_t id);
  Mob* mob(uint32_t id);
  const std::map<uint32_t, Mob>& mobs() const { return mobs_; }

  void setGoal(uint32_t id, Cell goal);
  /// Named waypoint targets (patrol points, boss arena marks).
  void setWaypoint(const std::string& name, Cell at) { waypoints_[name] = at; }
  std::optional<Cell> waypoint(const std::string& name) const;
  /// Observers (players) for distance LOD.
  void setObservers(std::vector<Cell> o) { observers_ = std::move(o); }
  void setBossSlot(const IBossSlot* slot) { boss_ = slot; }
  const IBossSlot& boss() const { return boss_ ? *boss_ : neutral_; }
  /// Drop gating through the boss slot.
  bool requestDrop(uint32_t id, uint32_t item) const;

  /// Advance by `simTicks` world ticks at `worldTps`; runs every AI tick that falls due.
  void advance(int simTicks, int worldTps);
  /// One AI tick now.
  void tick();
  const AiStats& stats() const { return stats_; }
  const AiConfig& config() const { return cfg_; }
  const WalkCache& nav() const { return nav_; }

 private:
  const WalkCache& nav_;
  AiConfig cfg_;
  HpaGraph hpa_;  // P4: lazy/repaired from nav_ versions; only consulted when cfg_.useHpa
  // P4 flow-field state (single current field, keyed by goal + nav
  // generation; rebuilt in the plan collect phase, never in a job).
  std::optional<FlowField> flow_;
  Cell flowGoal_{};
  uint64_t flowGen_ = ~uint64_t(0);
  bool flowOk_ = false;
  JobPool* pool_;
  std::map<uint32_t, Mob> mobs_;  // id order = deterministic order
  // Despawns requested DURING tick() (e.g. a brain despawning itself) are
  // deferred to the end of tick: erasing mid-iteration would invalidate the
  // loop iterator and the Mob& held by AiContext (UB).
  std::vector<uint32_t> pendingDespawn_;
  bool ticking_ = false;
  std::map<std::string, Cell> waypoints_;
  std::vector<Cell> observers_;
  const IBossSlot* boss_ = nullptr;
  IBossSlot neutral_;
  uint32_t nextId_ = 1;
  uint64_t tick_ = 0;
  double acc_ = 0;
  AiStats stats_;
};

}  // namespace litholattice::ai
