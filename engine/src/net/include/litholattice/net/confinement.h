// Network confinement for mods (mod hardening item 4, 2026-10-11).
//
// Mods may ONLY use the engine's net API — no raw sockets. Enforcement has two
// layers:
//
//   1. API surface (compile-time): LithoLatticeModApi exposes no socket
//      primitives at all — no socket(), sendto(), fds, addresses, or DNS.
//      The only network entry point is netSend (requires kPermNet), which the
//      engine binds to the guarded send path below. The audit pinning the
//      surface lives in the mods selftest (mod_interface.h audit table).
//   2. Runtime guard (this header): every outbound datagram passes through
//      NetConfinement::checkSend, which tags traffic by origin (engine vs
//      mod) and fails closed — an unregistered mod, a null gate, or an
//      exceeded budget drops the datagram with a logged reason. Mod traffic
//      is additionally rate-limited per mod (bytes/sec + packets/sec,
//      powers-of-two defaults): overruns are throttled (dropped, logged),
//      and sustained abuse suspends the mod's network access (logged) until
//      the engine re-enables it.
//
// Wire hygiene: engine traffic uses PayloadType 1..4 (< 256). Mod traffic
// must use types >= kModNetTypeBase (256), so a mod can never spoof engine
// traffic kinds on the wire; sendMod refuses lower types.
//
// THREAT MODEL (honest scope): for native (C++) mods the API surface is the
// real control — in-process code can always call libc directly, and only OS
// sandboxing (future work, builder note) changes that. What this layer
// guarantees: (a) the engine never forwards mod traffic except through the
// gated path, (b) unbudgeted/unknown mod traffic can never reach the socket,
// (c) abusive mods lose network access automatically with an audit trail.
// Script mods additionally sit behind the binding allowlist (no raw
// file/network/process/system bindings exist there).
//
// THREADING: not thread-safe by design — owned by the net tick, like
// PrioManager and the channels (see channels.h). Time is a game-provided
// monotonic millisecond clock (fakeable in tests); never wall-clock, keeping
// the sim deterministic.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "litholattice/net/channels.h"
#include "litholattice/net/protocol.h"

namespace litholattice {
namespace net {

/// Traffic origin, tagged at the send-path choke point.
enum class NetOrigin : uint8_t {
  Engine = 0,
  Mod = 1,
};

/// Per-mod network budgets (powers of two, per JD's convention).
inline constexpr uint64_t kModNetBytesPerSecDefault = 4096;    ///< 2^12 bytes/sec
inline constexpr uint64_t kModNetPacketsPerSecDefault = 64;    ///< 2^6 packets/sec
/// Sustained-abuse threshold: this many throttles inside one window suspends
/// the mod's network access.
inline constexpr uint64_t kModNetAbuseThrottleLimit = 16;      ///< 2^4
/// Mod wire types start here; engine PayloadType values are all < 256.
inline constexpr uint16_t kModNetTypeBase = 256;

/// Outcome of a mod's send request.
enum class ModNetVerdict : uint8_t {
  Allow = 0,    ///< within budget: the datagram may go.
  Throttle = 1,  ///< over budget (or unknown mod): dropped, logged reason.
  Suspended = 2,  ///< network access suspended: dropped, logged reason.
};

struct ModNetBudgets {
  uint64_t bytesPerSec = kModNetBytesPerSecDefault;
  uint64_t packetsPerSec = kModNetPacketsPerSecDefault;
};

/// Per-mod rate limiter + abuse suspension. The game registers every
/// network-capable mod here before its netSend can reach the wire.
///
/// DENIAL LOGGING (audit LOW 2026-10-11): every denial path logs, but the
/// log lines are rate-limited — the first denial per mod per 1 s window
/// logs in full, the rest are counted and summarized when the window rolls.
/// A buggy/malicious mod waving kPermNet can no longer fill the log volume.
class ModNetGate {
 public:
  /// Denial-log suppression window: the first denial in each window logs in
  /// full; the rest are counted and summarized at rollover.
  struct DenyLogWindow {
    uint64_t startMs = 0;
    uint64_t suppressed = 0;
    bool open = false;
  };

  /// Log-suppression gate for denial lines. Returns true when the caller
  /// should log the full line now; false means the denial was counted into
  /// the window summary. When the window rolls over with a pending count,
  /// the summary ("... and N further denials for mod 'x' in the last 1 s
  /// window") is logged before the new window's first full line.
  static bool denyLogGate(DenyLogWindow& w, uint64_t nowMs, const std::string& modId);

  /// Register a mod for network access. False on an invalid id or a
  /// duplicate (logged). Ids are [a-z0-9_]{1,64} (same rule as
  /// ExtensionRegistry::isValidId; duplicated here so net never depends
  /// on the mods module).
  bool registerMod(const std::string& modId, ModNetBudgets budgets = {});

  /// Gate one outbound datagram. Fails closed: unknown mod -> Throttle
  /// (logged, rate-limited). Over budget -> Throttle (logged, rate-limited);
  /// kModNetAbuseThrottleLimit throttles inside one window -> the mod is
  /// Suspended (logged) and later calls return Suspended until
  /// resetSuspension().
  ModNetVerdict checkSend(const std::string& modId, size_t bytes, uint64_t nowMs);

  /// True when the mod's network access is currently suspended.
  bool isSuspended(const std::string& modId) const;
  /// The engine/game re-enables a suspended mod (logged). False when the mod
  /// is unknown or not suspended.
  bool resetSuspension(const std::string& modId);
  /// Adjust a registered mod's budgets (logged). False when unknown.
  bool setBudgets(const std::string& modId, ModNetBudgets budgets);

  size_t modCount() const { return mods_.size(); }

 private:
  struct State {
    ModNetBudgets budgets;
    uint64_t windowStartMs = 0;
    uint64_t windowBytes = 0;
    uint64_t windowPackets = 0;
    uint64_t windowThrottles = 0;
    bool suspended = false;
    uint64_t suspendedAtMs = 0;
    std::string suspendReason;
    bool windowOpen = false;
    DenyLogWindow denyLog_;  // per-mod denial-log suppression window
  };
  static bool isValidModId(const std::string& id);
  std::map<std::string, State> mods_;
  DenyLogWindow unknownDenyLog_;  // denials for never-registered mod ids (no State yet)
};

/// The send-path guard. Every outbound datagram goes through here:
/// origin Engine always passes; origin Mod must be a registered,
/// non-suspended mod inside its budgets. Fails closed (false = drop the
/// datagram) on: null gate with Mod origin, unknown mod, throttled budget,
/// suspended mod. Denials are logged with a reason, rate-limited per the
/// ModNetGate::DenyLogWindow policy (audit LOW 2026-10-11).
class NetConfinement {
 public:
  static bool checkSend(NetOrigin origin, const std::string& modId, size_t bytes,
                        uint64_t nowMs, ModNetGate* gate);
};

/// ChannelMux with the confinement guard wired into the send path. The game
/// hands ALL outbound traffic — engine and mod — to one of these two entry
/// points; there is no unguarded path to the socket from here.
class ConfinedChannelMux {
 public:
  explicit ConfinedChannelMux(ChannelMap map = ChannelMap{}, ModNetGate* gate = nullptr)
      : mux_(map), gate_(gate) {}

  /// Engine traffic (origin Engine): straight to the channels, no gate.
  bool sendEngine(PayloadType type, const uint8_t* payload, size_t len, uint64_t nowMs,
                  std::vector<uint8_t>& datagram) {
    return mux_.send(type, payload, len, nowMs, datagram);
  }

  /// Mod traffic (origin Mod): confinement-checked, then framed. Refuses
  /// (false + logged, rate-limited) on: confinement denial, or a wire type
  /// below kModNetTypeBase (mods may not use engine traffic kinds).
  bool sendMod(const std::string& modId, uint16_t type, const uint8_t* payload, size_t len,
               uint64_t nowMs, std::vector<uint8_t>& datagram);

  ChannelMux& mux() { return mux_; }
  const ChannelMux& mux() const { return mux_; }

 private:
  ChannelMux mux_;
  ModNetGate* gate_;  // not owned; must outlive the mux
  ModNetGate::DenyLogWindow denyLog_;  // reserved-type denial suppression
};

}  // namespace net
}  // namespace litholattice
