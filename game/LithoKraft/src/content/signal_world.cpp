#include "lithokraft/content/signal_world.h"
#include "lithokraft/content/blocks.h"
#include "lithokraft/content/item_use.h"
#include "litholattice/core/log.h"

namespace lithokraft {

bool SignalWorldSimulation::canPlace(const ChunkMap& w, int x, int y, int z, BlockId block) const {
  if (isFlatWire(block)) return wireSupported(w, x, y, z);
  return true;  // torches and switches: canAttach decides (they mount on the clicked face)
}

bool SignalWorldSimulation::stillSupported(const ChunkMap& w, int x, int y, int z) const {
  return !isFlatWire(w.get(x, y, z)) || wireSupported(w, x, y, z);
}

bool SignalWorldSimulation::canAttach(const ChunkMap& w, int x, int y, int z, BlockId block, int face) const {
  int dx, dy, dz;
  attachOffset(face, dx, dy, dz);
  if (!blockRegistry().has(w.get(x + dx, y + dy, z + dz), kSolid)) return false;  // needs a solid support
  if (roleOf(block) == SimRole::Torch) return face != 2;  // torches stand on top or hang on sides, never from ceilings
  return true;  // switch: top, sides or bottom
}

void SignalWorldSimulation::onBlockChanged(ChunkMap& /*w*/, int x, int y, int z, BlockId now) {
  signal_.onBlockChanged(x, y, z, now);
  sounds_.push_back({now == kAir ? SoundCue::Break : SoundCue::Place, 0, x, y, z});
}

bool SignalWorldSimulation::use(ChunkMap& w, int x, int y, int z, const DirtyFn& dirty) {
  if (!signal_.use(w, x, y, z, dirty)) return false;
  sounds_.push_back({SoundCue::Click, 0, x, y, z});
  return true;
}

void SignalWorldSimulation::command(ChunkMap& w, const SimCommand& c, const DirtyFn& dirty) {
  if (c.code == kCmdKeyPulse) signal_.keyPulse(c.x, c.y, c.z, c.a, c.b);
  else if (c.code == kCmdAltUse && signal_.use(w, c.x, c.y, c.z, dirty, true)) sounds_.push_back({SoundCue::Click, 0, c.x, c.y, c.z});
  // B1a/A3: the general per-item use seam. Dispatch fails closed (logged)
  // for items with no defined handler; a handled use clicks like block Use.
  else if (c.code == kCmdItemUse &&
           dispatchItemUse(uint32_t(c.a), ItemUseContext{w, c.x, c.y, c.z, dirty}) == ItemUseResult::Handled)
    sounds_.push_back({SoundCue::Click, 0, c.x, c.y, c.z});
}

void SignalWorldSimulation::onBlockPopped(ChunkMap& /*w*/, int x, int y, int z, BlockId was) {
  // Popped blocks become item drops (Minecraft-style): the game thread spawns the entity.
  if (was == kSilicaGrass) {
    // Silica grass drops seeds, not the grass block itself (pop policy: 1 seed).
    for (int i = 0; i < silicaBreakDrops(x, y, z); ++i)
      dropEvents_.push_back(SimEvent{kEvItemDrop, int(kItemSilicaSeed), x, y, z});
    return;
  }
  dropEvents_.push_back(SimEvent{kEvItemDrop, int(was), x, y, z});
}

void SignalWorldSimulation::onRandomTick(ChunkMap& w, int x, int y, int z, BlockId block, uint32_t rnd,
                                         const DirtyFn& dirty) {
  if (block != kSilicaGrass) return;
  // No ILighting is plumbed into the sim path yet: pass a null query and the
  // silica logic falls back to the open-sky check (surfaceY). Uproot seed drops
  // join the normal drop-event channel.
  silicaOnRandomTick(w, blockRegistry(), x, y, z, rnd, dirty, {},
                     [&](uint32_t itemId, uint16_t count, int dx, int dy, int dz) {
                       for (uint16_t i = 0; i < count; ++i)
                         dropEvents_.push_back(SimEvent{kEvItemDrop, int(itemId), dx, dy, dz});
                     });
}

void SignalWorldSimulation::takeEvents(std::vector<SimEvent>& out) {
  out.insert(out.end(), dropEvents_.begin(), dropEvents_.end());
  dropEvents_.clear();
  signal_.takeSounds(sounds_);
  for (const SoundEvent& s : sounds_) out.push_back({uint16_t(s.cue), s.value, s.x, s.y, s.z});
  sounds_.clear();
}


// ---- Timing Handshake integration (hs-design §3.4/§7.2) ----
// Registration order: flip+torches, components, mark, prep (all affine,
// CycleHead at their phase offsets), then W net shards and W lamp shards
// (pull workers). All Signal work is once per world tick: one cycle of P
// phases executes inside this tick() call frame (D2 corollary), so
// TimedSimulation / SimThread tick timings measure the same thing in both
// modes. Pull workers NEVER receive the real DirtyFn: net floods do not
// dirty at all, and lamp batches write into per-shard collectors that the
// sim thread replays through the real dirty fn at the phase commit, in
// canonical (shard, then ascending key) order (hs-design §3.3).

void SignalWorldSimulation::registerHandshake(litholattice::TimingHandshakeAuthority& authority) {
  hsAuthority_ = &authority;
  const int p = authority.phasesPerCycle();
  const int w = authority.workerCount();
  hsLampDirty_.assign(size_t(w), {});
  hsLampHolds_.assign(size_t(w), {});
  hsNames_.clear();
  auto name = [this](std::string n) -> const char* {
    hsNames_.push_back(std::move(n));
    return hsNames_.back().c_str();
  };
  using litholattice::CadenceClass;
  auto reg = [&](const char* n, int offset, bool affine, litholattice::TimingHandshakeAuthority::WorkFn fn) {
    litholattice::HsComponentDesc d;
    d.name = n;
    d.cadence = CadenceClass::CycleHead;
    d.phaseDivider = uint32_t(p);
    d.phaseOffset = uint32_t(offset);
    d.threadAffine = affine;
    d.tickCritical = true;
    d.group = 1;  // signal group; SimThread's clock domains use group 2
    authority.registerComponent(d, std::move(fn));
  };
  authority.setCommitFn([this](uint64_t phase, uint64_t cycle, uint32_t group) { onHsCommit(phase, cycle, group); });
  reg(name("signal.flip_torches"), 0, true, [this](uint64_t, uint64_t) { signal_.hsFlipAndTorches(*hsWorld_, hsDirty_); });
  reg(name("signal.components"), 1, true, [this](uint64_t, uint64_t) { signal_.hsComponents(*hsWorld_, hsDirty_); });
  reg(name("signal.mark"), 2, true, [this](uint64_t, uint64_t) { signal_.hsMark(*hsWorld_); });
  reg(name("signal.prep"), 2, true, [this](uint64_t, uint64_t) {
    if (!signal_.hsCycleSkipped() && !signal_.hsFullPass()) signal_.hsPrepIncremental(*hsWorld_);
  });
  for (int i = 0; i < w; ++i) {
    reg(name("signal.net.shard" + std::to_string(i)), 2, false,
        [this, i, w](uint64_t, uint64_t) {
          if (signal_.hsCycleSkipped() || signal_.hsFullPass()) return;
          const std::vector<int32_t>& nets = signal_.hsMarkedNets();
          const DirtyFn none = [](int, int, int) {};  // per-net floods never dirty (strength buffer only)
          for (size_t idx = size_t(i); idx < nets.size(); idx += size_t(w))
            signal_.hsPropagateNet(*hsWorld_, nets[idx], none);
        });
  }
  for (int i = 0; i < w; ++i) {
    reg(name("signal.lamps.shard" + std::to_string(i)), 3, false,
        [this, i, w](uint64_t, uint64_t) {
          if (signal_.hsCycleSkipped()) return;
          const std::vector<int64_t>& keys = signal_.hsLampKeys();
          const size_t n = keys.size(), base = w ? n / size_t(w) : 0, rem = w ? n % size_t(w) : 0;
          const size_t start = size_t(i) * base + std::min(size_t(i), rem);
          const size_t len = base + (size_t(i) < rem ? 1 : 0);
          std::vector<int64_t> batch(keys.begin() + start, keys.begin() + start + len);
          auto& collected = hsLampDirty_[size_t(i)];
          collected.clear();
          const DirtyFn collector = [&collected](int x, int y, int z) { collected.push_back(HsDirtyPos{x, y, z}); };
          signal_.hsLampsBatch(batch, *hsWorld_, collector);
          hsLampHolds_[size_t(i)] = SignalSim::hsTlsLampHolds();  // drain this thread's hold updates
          SignalSim::hsTlsLampHolds().clear();
        });
  }
}

void SignalWorldSimulation::onHsCommit(uint64_t phase, uint64_t /*cycleTick*/, uint32_t group) {
  if (group != 1 || !hsAuthority_) return;
  const int p = hsAuthority_->phasesPerCycle();
  const int sub = p > 0 ? int(phase % uint64_t(p)) : 0;
  if (sub == 2) {
    // Propagate barrier passed: replay the deferred strength writes in
    // canonical order on this (sim) thread.
    if (!signal_.hsCycleSkipped() && !signal_.hsFullPass()) signal_.hsApplyStrengthPending();
    else if (signal_.hsFullPass()) signal_.hsPropagateFull(*hsWorld_, hsDirty_);
  } else if (sub == 3) {
    // Lamp barrier passed: replay collected dirties through the real dirty
    // fn (canonical: shard order, ascending keys inside each batch), merge
    // the deferred lamp holds, finish the cycle, then flip (commit).
    for (auto& shardDirty : hsLampDirty_) {
      for (const auto& [x, y, z] : shardDirty) hsDirty_(x, y, z);
      shardDirty.clear();
    }
    for (auto& holds : hsLampHolds_) {
      signal_.hsApplyLampHolds(holds);
      holds.clear();
    }
    if (!signal_.hsCycleSkipped()) signal_.hsFinishCycle(*hsWorld_);
    signal_.hsCommit();
  }
}

void SignalWorldSimulation::tickHandshake(ChunkMap& world, const TickArea& area, const DirtyFn& dirty) {
  const int p = hsAuthority_->phasesPerCycle();
  if (p < 4) {  // the four sub-steps need four phase slots; fall back to the legacy cycle
    if (!hsLegacyFallbackLogged_) {
      hsLegacyFallbackLogged_ = true;
      litholattice::logWarn("timing-handshake: phases/cycle %d < 4 — Signal cycle runs legacy inside handshake mode", p);
    }
    signal_.tick(world, area.pcx, area.pcz, area.radius, dirty);
    return;
  }
  hsWorld_ = &world;
  hsArea_ = area;
  hsDirty_ = dirty;
  hsFrozenCycle_ = false;
  signal_.hsBeginCycle(world, area.pcx, area.pcz, area.radius);
  const uint64_t cycle = signal_.tickCount();
  const uint64_t base = (cycle - 1) * uint64_t(p);
  for (int i = 0; i < p; ++i) {
    if (!hsAuthority_->runPhase(base + uint64_t(i), cycle, 1)) {
      // Watchdog freeze mid-cycle: nothing past the last barrier committed.
      // Force a full pass on recovery so the next cycle rebuilds cleanly.
      hsFrozenCycle_ = true;
      signal_.invalidateNetwork();
      hsWorld_ = nullptr;
      return;
    }
  }
  hsWorld_ = nullptr;
}

}  // namespace lithokraft
