// Network confinement: per-mod rate limiting + the send-path guard.
// See confinement.h for the design and threat model.
#include "litholattice/net/confinement.h"

#include "litholattice/core/log.h"

namespace litholattice {
namespace net {
namespace {

constexpr uint64_t kWindowMs = 1000;
constexpr uint64_t kDenyLogWindowMs = 1000;

}  // namespace

bool ModNetGate::denyLogGate(DenyLogWindow& w, uint64_t nowMs, const std::string& modId) {
  // First denial in the window logs in full; the rest are counted. When the
  // window rolls with a pending count, the summary goes out before the new
  // window's first full line — at most two lines per mod per second.
  if (!w.open || nowMs - w.startMs >= kDenyLogWindowMs) {
    if (w.open && w.suppressed > 0)
      logError("netconf: ... and %llu further denials for mod '%s' in the last 1 s window",
               (unsigned long long)w.suppressed, modId.c_str());
    w.open = true;
    w.startMs = nowMs;
    w.suppressed = 0;
    return true;
  }
  ++w.suppressed;
  return false;
}

bool ModNetGate::isValidModId(const std::string& id) {
  // Same rule as ExtensionRegistry::isValidId ([a-z0-9_]{1,64}); duplicated
  // so the net module never depends on the mods module.
  if (id.empty() || id.size() > 64) return false;
  for (char ch : id) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
    if (!ok) return false;
  }
  return true;
}

bool ModNetGate::registerMod(const std::string& modId, ModNetBudgets budgets) {
  if (!isValidModId(modId)) {
    logError("netconf: refusing to register invalid mod id '%s'", modId.c_str());
    return false;
  }
  if (mods_.count(modId) != 0) {
    logError("netconf: mod '%s' is already registered", modId.c_str());
    return false;
  }
  State s;
  s.budgets = budgets;
  mods_.emplace(modId, s);
  logInfo("netconf: registered mod '%s' (budgets %llu B/s, %llu pkt/s)", modId.c_str(),
          (unsigned long long)budgets.bytesPerSec, (unsigned long long)budgets.packetsPerSec);
  return true;
}

ModNetVerdict ModNetGate::checkSend(const std::string& modId, size_t bytes, uint64_t nowMs) {
  auto it = mods_.find(modId);
  if (it == mods_.end()) {
    // Fail closed: a mod the gate never heard of gets nothing on the wire.
    // (Denial log rate-limited: no State exists yet, so the gate-level
    // unknown-mod window suppresses.)
    if (denyLogGate(unknownDenyLog_, nowMs, modId))
      logError("netconf: DENIED send from unregistered mod '%s' (fail-closed)", modId.c_str());
    return ModNetVerdict::Throttle;
  }
  State& s = it->second;
  if (s.suspended) {
    if (denyLogGate(s.denyLog_, nowMs, modId))
      logError("netconf: DENIED send from suspended mod '%s' (%s)", modId.c_str(),
               s.suspendReason.c_str());
    return ModNetVerdict::Suspended;
  }
  if (!s.windowOpen || nowMs - s.windowStartMs >= kWindowMs) {
    s.windowOpen = true;
    s.windowStartMs = nowMs;
    s.windowBytes = 0;
    s.windowPackets = 0;
    s.windowThrottles = 0;
  }
  // Wraparound-safe: windowBytes + bytes could overflow past the budget check
  // (a huge single send would look "under budget"). Compare via subtraction.
  const bool overBytes = bytes > s.budgets.bytesPerSec - std::min(s.windowBytes, s.budgets.bytesPerSec);
  const bool overPackets = s.windowPackets + 1 > s.budgets.packetsPerSec;
  if (overBytes || overPackets) {
    ++s.windowThrottles;
    // Denial log rate-limited per mod per window (audit LOW 2026-10-11):
    // the verdict is unchanged, only the log volume is capped.
    if (denyLogGate(s.denyLog_, nowMs, modId))
      logError("netconf: THROTTLED mod '%s' send (%zu bytes): over budget "
               "(window %llu/%llu B, %llu/%llu pkt)",
               modId.c_str(), bytes, (unsigned long long)s.windowBytes,
               (unsigned long long)s.budgets.bytesPerSec, (unsigned long long)s.windowPackets,
               (unsigned long long)s.budgets.packetsPerSec);
    if (s.windowThrottles >= kModNetAbuseThrottleLimit) {
      s.suspended = true;
      s.suspendedAtMs = nowMs;
      s.suspendReason = "sustained budget abuse: " + std::to_string(s.windowThrottles) +
                        " throttles in one window";
      logError("netconf: SUSPENDED mod '%s' network access (%s)", modId.c_str(),
               s.suspendReason.c_str());
      return ModNetVerdict::Suspended;
    }
    return ModNetVerdict::Throttle;
  }
  s.windowBytes += bytes;
  s.windowPackets += 1;
  return ModNetVerdict::Allow;
}

bool ModNetGate::isSuspended(const std::string& modId) const {
  const auto it = mods_.find(modId);
  return it != mods_.end() && it->second.suspended;
}

bool ModNetGate::resetSuspension(const std::string& modId) {
  auto it = mods_.find(modId);
  if (it == mods_.end() || !it->second.suspended) return false;
  it->second.suspended = false;
  it->second.suspendReason.clear();
  it->second.windowOpen = false;  // fresh window on re-enable
  logInfo("netconf: re-enabled network access for mod '%s' (engine reset)", modId.c_str());
  return true;
}

bool ModNetGate::setBudgets(const std::string& modId, ModNetBudgets budgets) {
  auto it = mods_.find(modId);
  if (it == mods_.end()) return false;
  it->second.budgets = budgets;
  logInfo("netconf: mod '%s' budgets now %llu B/s, %llu pkt/s", modId.c_str(),
          (unsigned long long)budgets.bytesPerSec, (unsigned long long)budgets.packetsPerSec);
  return true;
}

bool NetConfinement::checkSend(NetOrigin origin, const std::string& modId, size_t bytes,
                               uint64_t nowMs, ModNetGate* gate) {
  if (origin == NetOrigin::Engine) return true;  // engine traffic is ungated
  if (gate == nullptr) {
    // Fail closed: mod traffic with no gate configured never reaches the wire.
    // (Denial log rate-limited process-wide: no gate exists to hold a window.)
    static ModNetGate::DenyLogWindow w;
    if (ModNetGate::denyLogGate(w, nowMs, modId))
      logError("netconf: DENIED mod '%s' send: no gate configured (fail-closed)", modId.c_str());
    return false;
  }
  return gate->checkSend(modId, bytes, nowMs) == ModNetVerdict::Allow;
}

bool ConfinedChannelMux::sendMod(const std::string& modId, uint16_t type, const uint8_t* payload,
                                 size_t len, uint64_t nowMs, std::vector<uint8_t>& datagram) {
  if (type < kModNetTypeBase) {
    if (ModNetGate::denyLogGate(denyLog_, nowMs, modId))
      logError("netconf: DENIED mod '%s' send: wire type %u is reserved for engine traffic",
               modId.c_str(), type);
    return false;
  }
  if (!NetConfinement::checkSend(NetOrigin::Mod, modId, len, nowMs, gate_)) return false;
  return mux_.send(static_cast<PayloadType>(type), payload, len, nowMs, datagram);
}

}  // namespace net
}  // namespace litholattice
