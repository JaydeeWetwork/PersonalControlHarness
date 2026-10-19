#pragma once
#include <memory>
#include <string>
#include "litholattice/world/chunk_map.h"
#include "lithokraft/content/signal_world.h"
#include "lithokraft/game/dimensions.h"

namespace lithokraft {
/// --handshake-test mode (hs-design §6): WAIT + FREEZE policy proofs on a
/// bare authority, the end-to-end SimThread freeze/save/reload proof, and
/// the determinism scenario summary. Returns the number of failed checks.
int runHandshakeTests();
/// Determinism scenario (hs-design §6.4): build the circuit scene, run
/// `ticks` world ticks in the given mode, print the outcome hash.
/// mode: "legacy" | "handshake". Returns 0 on success.
int runHandshakeScenario(int ticks, const std::string& mode, int workers, int phases);
/// Selftest section (256-tick scenario): legacy vs handshake hashes at
/// workers {1,4} x phases {4,8} must all be equal. Returns failures.
int runHandshakeSelfTest();

/// The §6.4 scenario world, exported for the control-serialization
/// harness (which drives the same world through the real SimThread and
/// the serialized control stream instead of in-process tick calls).
struct ControlScenarioWorld {
  DimensionRegistry reg;
  Dimension* wafer = nullptr;
  std::unique_ptr<SignalWorldSimulation> ws;
  int y = 0, wireX = -1, wireZ = -1, repX = -1, repZ = -1;
};
ControlScenarioWorld buildControlScenarioWorld();
/// The §6.4 outcome hash (identical definition to the handshake harness).
uint64_t hashControlWorld(const litholattice::ChunkMap& world, const SignalSim& sig, uint64_t ticks);
}  // namespace lithokraft
