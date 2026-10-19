// Net confinement selftest: ModNetGate rate limiting, NetConfinement
// send-path guard, ConfinedChannelMux. See confinement.h for the design.
#include "litholattice/net/confinement_selftest.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "litholattice/net/confinement.h"

namespace litholattice::net {
namespace {

class Checker {
 public:
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] netconf: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }

 private:
  int fails_ = 0;
};

void testRegistration(Checker& c) {
  ModNetGate gate;
  c(gate.registerMod("chatmod"), "valid mod id registers");
  c(!gate.registerMod("chatmod"), "duplicate registration refused");
  c(!gate.registerMod("Bad-Id") && !gate.registerMod("") &&
        !gate.registerMod(std::string(65, 'a')),
    "invalid mod ids refused");
  c(gate.modCount() == 1, "only the valid mod is registered");
}

void testBudgets(Checker& c) {
  // Inside the budget everything passes.
  {
    ModNetGate gate;
    gate.registerMod("okmod", ModNetBudgets{128, 4});
    bool all = true;
    for (int i = 0; i < 4; ++i)
      all = all && gate.checkSend("okmod", 32, 1000) == ModNetVerdict::Allow;
    c(all, "sends inside the byte+packet budgets are allowed");
  }
  // ADVERSARIAL: packet-budget overrun -> throttled (dropped, logged).
  {
    ModNetGate gate;
    gate.registerMod("spammod", ModNetBudgets{1 << 20, 4});
    bool ok = true;
    for (int i = 0; i < 4; ++i)
      ok = ok && gate.checkSend("spammod", 8, 2000) == ModNetVerdict::Allow;
    ok = ok && gate.checkSend("spammod", 8, 2000) == ModNetVerdict::Throttle;
    c(ok, "packet overrun is throttled (dropped with a logged reason)");
  }
  // ADVERSARIAL: byte-budget overrun -> throttled.
  {
    ModNetGate gate;
    gate.registerMod("bigmod", ModNetBudgets{128, 1 << 10});
    c(gate.checkSend("bigmod", 129, 3000) == ModNetVerdict::Throttle,
      "byte overrun is throttled (dropped with a logged reason)");
  }
  // Budgets are powers of two by default (JD's convention).
  {
    const auto isPow2 = [](uint64_t v) { return v != 0 && (v & (v - 1)) == 0; };
    c(isPow2(kModNetBytesPerSecDefault) && isPow2(kModNetPacketsPerSecDefault) &&
          isPow2(kModNetAbuseThrottleLimit),
      "default budgets and the abuse threshold are powers of two");
  }
  // Windows roll over: a new window gets a fresh budget.
  {
    ModNetGate gate;
    gate.registerMod("winmod", ModNetBudgets{64, 2});
    c(gate.checkSend("winmod", 32, 1000) == ModNetVerdict::Allow &&
          gate.checkSend("winmod", 32, 1000) == ModNetVerdict::Allow &&
          gate.checkSend("winmod", 32, 1000) == ModNetVerdict::Throttle &&
          gate.checkSend("winmod", 32, 2000) == ModNetVerdict::Allow,
      "budget windows roll over (fresh budget in the next window)");
  }
}

void testSuspension(Checker& c) {
  // ADVERSARIAL: sustained abuse -> suspended, then re-enabled by the engine.
  ModNetGate gate;
  gate.registerMod("abuser", ModNetBudgets{64, 1 << 10});
  ModNetVerdict last = ModNetVerdict::Allow;
  for (int i = 0; i < 16; ++i) last = gate.checkSend("abuser", 65, 4000);
  c(last == ModNetVerdict::Suspended && gate.isSuspended("abuser"),
    "16 throttles in one window suspends the mod's network access");
  c(gate.checkSend("abuser", 1, 4000) == ModNetVerdict::Suspended,
    "a suspended mod's sends stay denied");
  c(gate.resetSuspension("abuser") && !gate.isSuspended("abuser") &&
        gate.checkSend("abuser", 1, 4000) == ModNetVerdict::Allow,
    "the engine can re-enable a suspended mod (logged)");
  c(!gate.resetSuspension("nosuchmod"), "resetting an unknown mod fails cleanly");
  c(gate.setBudgets("abuser", ModNetBudgets{1 << 20, 1 << 10}) &&
        !gate.setBudgets("nosuchmod", ModNetBudgets{}),
    "budgets are adjustable for registered mods only");
}

void testFailClosed(Checker& c) {
  ModNetGate gate;
  c(gate.checkSend("ghost", 8, 5000) == ModNetVerdict::Throttle,
    "unregistered mod is denied (fail-closed)");
  c(!NetConfinement::checkSend(NetOrigin::Mod, "ghost", 8, 5000, nullptr),
    "mod traffic with no gate configured is denied (fail-closed)");
  c(!NetConfinement::checkSend(NetOrigin::Mod, "ghost", 8, 5000, &gate),
    "unregistered mod fails the send-path guard");
  c(NetConfinement::checkSend(NetOrigin::Engine, "ghost", 8, 5000, nullptr),
    "engine traffic always passes the guard (ungated by design)");
}

void testConfinedMux(Checker& c) {
  ModNetGate gate;
  gate.registerMod("muxmod");
  ConfinedChannelMux mux(ChannelMap{}, &gate);
  std::vector<uint8_t> d;
  const std::vector<uint8_t> p = {1, 2, 3, 4};
  c(mux.sendEngine(PayloadType::Event, p.data(), p.size(), 6000, d) && !d.empty(),
    "engine traffic frames through the confined mux");
  d.clear();
  c(!mux.sendMod("muxmod", 4, p.data(), p.size(), 6000, d),
    "mod traffic with an engine wire type (< 256) is refused");
  d.clear();
  c(mux.sendMod("muxmod", 256, p.data(), p.size(), 6000, d) && !d.empty(),
    "mod traffic with a mod wire type (>= 256) frames when budgeted");
  // The framed datagram really is an unreliable-channel datagram of type 256.
  quantize::ByteReader r(d.data(), d.size());
  PacketHeader h;
  c(decodeHeader(r, h) && h.type == static_cast<PayloadType>(256) && !(h.flags & kFlagReliable),
    "mod datagram carries the mod wire type on the unreliable channel");
  d.clear();
  c(!mux.sendMod("ghost", 256, p.data(), p.size(), 6000, d),
    "unregistered mod gets nothing through the mux (fail-closed)");
}

void testSustainedAbuseThroughMux(Checker& c) {
  // ADVERSARIAL: a mod hammering the mux exhausts its budget and is cut off.
  ModNetGate gate;
  gate.registerMod("hammer", ModNetBudgets{64, 1 << 10});
  ConfinedChannelMux mux(ChannelMap{}, &gate);
  std::vector<uint8_t> d;
  const std::vector<uint8_t> p(65, 0xAB);
  int denied = 0;
  for (int i = 0; i < 32; ++i)
    if (!mux.sendMod("hammer", 257, p.data(), p.size(), 7000, d)) ++denied;
  c(denied == 32 && gate.isSuspended("hammer"),
    "sustained over-budget sends through the mux end in suspension");
}

}  // namespace

int runNetConfinementSelfTest() {
  Checker c;
  testRegistration(c);
  testBudgets(c);
  testSuspension(c);
  testFailClosed(c);
  testConfinedMux(c);
  testSustainedAbuseThroughMux(c);
  return c.fails();
}

}  // namespace litholattice::net
