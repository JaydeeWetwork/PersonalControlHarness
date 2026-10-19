#include "litholattice/script/suspension_policy.h"
#include <algorithm>
#include "litholattice/core/log.h"

namespace litholattice {

// The cap is exactly base << 6: 64 -> 128 -> 256 -> 512 -> 1024 -> 2048 -> 4096.
static_assert(SuspensionPolicy::kBackoffBaseTicks << 6 == SuspensionPolicy::kBackoffMaxTicks,
              "backoff cap must equal base<<6");

uint32_t SuspensionPolicy::backoffTicksFor(uint32_t consecutive) {
  if (consecutive == 0) return 0;
  // Doubling per consecutive suspension, capped: shift never exceeds 6.
  const uint32_t shift = consecutive - 1 > 6 ? 6 : consecutive - 1;
  return kBackoffBaseTicks << shift;
}

bool SuspensionPolicy::onSuspended(const std::string& reason) {
  ++consecutiveSuspensions_;
  cleanStreak_ = 0;
  logWarn("modsys: suspended mod '%s': %s", displayId(), reason.c_str());
  if (consecutiveSuspensions_ >= kMaxConsecutiveSuspensions) {
    // Circuit breaker: no cooldown, no auto-recovery. An admin must call
    // ScriptEngine::resume(). Logged at Error: this must be unmissable.
    state_ = State::kEscalated;
    cooldownRemaining_ = 0;
    probationRemaining_ = 0;
    logError("modsys: mod '%s' reached %u consecutive suspensions: AUTO-RECOVERY DISABLED, "
             "manual re-enable required",
             displayId(), consecutiveSuspensions_);
    return false;
  }
  state_ = State::kCooldown;
  cooldownRemaining_ = backoffTicksFor(consecutiveSuspensions_);
  probationRemaining_ = 0;
  logInfo("modsys: mod '%s' auto-recovery in %u ticks (consecutive suspension #%u of %u)", displayId(),
          cooldownRemaining_, consecutiveSuspensions_, kMaxConsecutiveSuspensions);
  return true;
}

SuspensionPolicy::TickAction SuspensionPolicy::onTick(bool clean) {
  switch (state_) {
    case State::kCooldown: {
      // The mod ran nothing while cooling down: no clean credit accrues.
      if (cooldownRemaining_ == 0) return TickAction::kNone;  // defensive; unreachable
      if (--cooldownRemaining_ > 0) return TickAction::kNone;
      // Cooldown elapsed: re-admit on probation. The host clears the suspend
      // state when it sees kAutoRecover.
      state_ = State::kProbation;
      probationRemaining_ = kProbationTicks;
      logInfo("modsys: attempting auto-recovery of mod '%s' (cooldown of %u ticks elapsed)", displayId(),
              backoffTicksFor(consecutiveSuspensions_));
      logInfo("modsys: auto-recovered mod '%s' after %u ticks; probation %u ticks at half CPU budget",
              displayId(), backoffTicksFor(consecutiveSuspensions_), kProbationTicks);
      return TickAction::kAutoRecover;
    }
    case State::kProbation:
    case State::kMonitoring: {
      if (clean) {
        if (++cleanStreak_ >= kCleanTicksPerDecay) {
          cleanStreak_ = 0;
          if (consecutiveSuspensions_ > 0) {
            --consecutiveSuspensions_;
            logInfo("modsys: mod '%s' served %u clean ticks: consecutive suspensions decayed to %u",
                    displayId(), kCleanTicksPerDecay, consecutiveSuspensions_);
          }
        }
      } else {
        cleanStreak_ = 0;  // decay needs *consecutive* clean ticks
      }
      if (state_ == State::kProbation && probationRemaining_ > 0 && --probationRemaining_ == 0) {
        state_ = State::kMonitoring;
        logInfo("modsys: mod '%s' completed probation with %u clean ticks since recovery", displayId(),
                cleanStreak_);
      }
      return TickAction::kNone;
    }
    case State::kEscalated:
      return TickAction::kNone;  // only a manual resume() leaves this state
  }
  return TickAction::kNone;  // unreachable
}

bool SuspensionPolicy::reset() {
  const bool hadState =
      state_ != State::kMonitoring || consecutiveSuspensions_ > 0 || cleanStreak_ > 0;
  state_ = State::kMonitoring;
  consecutiveSuspensions_ = 0;
  cooldownRemaining_ = 0;
  probationRemaining_ = 0;
  cleanStreak_ = 0;
  return hadState;
}

uint32_t SuspensionPolicy::effectiveCpuBudgetUs(uint32_t baseBudgetUs) const {
  if (!probationActive()) return baseBudgetUs;
  // Halved on probation, but never zero: a zero budget would suspend the mod
  // for executing at all.
  return std::max(baseBudgetUs / 2u, 1u);
}

}  // namespace litholattice
