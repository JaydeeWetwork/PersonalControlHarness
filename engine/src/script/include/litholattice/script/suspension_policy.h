// Automatic suspension-recovery policy for per-mod quotas.
//
// A mod suspended by the quota system (CPU or memory overrun) is neither left
// suspended forever nor re-admitted blindly. Instead it enters a cooldown with
// exponential backoff, is auto re-admitted on probation, and escalates to a
// manual re-enable if it keeps misbehaving. Every transition is logged through
// the general engine-log record path ("modsys:" prefix, see core/log.h), so the
// debug console shows the full suspension/recovery history.
//
// Policy numbers (all powers of two, per JD's convention):
//   kBackoffBaseTicks = 64    cooldown after the 1st consecutive suspension
//   kBackoffMaxTicks  = 4096  cooldown cap; doubling stops here (64 << 6)
//   kMaxConsecutiveSuspensions = 8
//                             suspensions in a row -> MANUAL re-enable
//   kProbationTicks   = 64    heightened-scrutiny window after auto-recovery
//   kCleanTicksPerDecay = 256 clean ticks reduce the consecutive count by 1
//
// Backoff schedule (ticks of cooldown per consecutive suspension #):
//   #1: 64, #2: 128, #3: 256, #4: 512, #5: 1024, #6: 2048, #7: 4096.
// The 8th consecutive suspension never cools down: it escalates (see below).
//
// Probation: the first 64 ticks after an auto-recovery run under BOTH a
// reduced effective CPU budget (half the configured budget, minimum 1 us)
// AND zero tolerance: any single quota overrun suspends immediately,
// bypassing the throttle ladder. Rationale: the mod has demonstrated it
// cannot stay inside its budget, so probation asks it to prove it can run
// under a stricter one. A genuinely fixed mod passes trivially; a
// still-broken one trips the breaker faster, and each probation suspension
// still counts toward the 8 that escalate to manual. (Memory overruns always
// suspend immediately, so scrutiny there is unchanged.)
//
// Decay: every 256 consecutive clean ticks (hooks inside budget, mod neither
// throttled nor suspended) reduce the consecutive-suspension count by 1, down
// to 0. A reformed mod earns its way back; one clean probation alone does not
// clear the record. Any dirty tick resets the 256-tick clock.
//
// Escalation (circuit breaker): the 8th consecutive suspension does NOT start
// a cooldown. The mod stays suspended until an admin calls
// ScriptEngine::resume(), which clears all policy state. Logged at Error
// level so it is unmissable in the console. This bounds a mod that would
// otherwise ride auto-recovery forever.
//
// Driven by ScriptEngine (one policy per mod): suspend() -> onSuspended(),
// beginTick() -> onTick(), resume() -> reset(). Same threading assumptions as
// ScriptEngine: driven from the mod's tick path on one thread; the log calls
// themselves are thread-safe.
#pragma once
#include <cstdint>
#include <string>

namespace litholattice {

class SuspensionPolicy {
 public:
  static constexpr uint32_t kBackoffBaseTicks = 64;          // 2^6
  static constexpr uint32_t kBackoffMaxTicks = 4096;        // 2^12
  static constexpr uint32_t kMaxConsecutiveSuspensions = 8;  // 2^3
  static constexpr uint32_t kProbationTicks = 64;           // 2^6
  static constexpr uint32_t kCleanTicksPerDecay = 256;      // 2^8

  /// Cooldown ticks earned by the Nth consecutive suspension (1-based):
  /// 64, 128, 256, 512, 1024, 2048, 4096, then capped at 4096.
  /// Pure function of the count, so the schedule is unit-testable.
  static uint32_t backoffTicksFor(uint32_t consecutive);

  /// Owning mod's id, used in the modsys: log lines. Empty renders as
  /// "<unknown>" (the ModLoader sets the real id when it creates the engine).
  void setModId(const std::string& id) { modId_ = id; }
  const char* displayId() const { return modId_.empty() ? "<unknown>" : modId_.c_str(); }

  /// Record a suspension. Starts the backoff cooldown, or escalates to
  /// manual re-enable at kMaxConsecutiveSuspensions. Logs the suspension
  /// (reason + numbers) and the cooldown/escalation decision.
  /// Returns true when auto-recovery is still armed; false when escalated.
  /// Call once per suspension (ScriptEngine guards re-entry).
  bool onSuspended(const std::string& reason);

  /// Advance one tick. `clean` = the previous tick ran with no quota overrun
  /// and the mod was neither throttled nor suspended (computed by the host).
  /// Returns kAutoRecover exactly once, on the tick the cooldown elapses;
  /// the host must then clear the mod's suspend state (the policy has
  /// already entered probation).
  enum class TickAction { kNone, kAutoRecover };
  TickAction onTick(bool clean);

  /// Admin override: clear consecutive count, cooldown, probation, and the
  /// clean-tick clock. Returns true if any policy state was active.
  bool reset();

  bool probationActive() const { return state_ == State::kProbation; }
  bool escalated() const { return state_ == State::kEscalated; }
  /// Effective per-tick CPU budget: halved (min 1 us) on probation, the
  /// configured budget otherwise.
  uint32_t effectiveCpuBudgetUs(uint32_t baseBudgetUs) const;

  // Introspection (tests, admin tooling).
  uint32_t consecutiveSuspensions() const { return consecutiveSuspensions_; }
  uint32_t cooldownRemaining() const { return cooldownRemaining_; }
  uint32_t probationRemaining() const { return probationRemaining_; }

 private:
  enum class State { kMonitoring, kCooldown, kProbation, kEscalated };

  std::string modId_;
  State state_ = State::kMonitoring;
  uint32_t consecutiveSuspensions_ = 0;
  uint32_t cooldownRemaining_ = 0;
  uint32_t probationRemaining_ = 0;
  uint32_t cleanStreak_ = 0;  ///< consecutive clean ticks toward the next decay
};

}  // namespace litholattice
