// Signal-processing components: repeater, thresholder, redirector, pulsor, memory cell, fast latch.
#include <algorithm>
#include <format>
#include "components.h"
#include "lithokraft/content/signal.h"

namespace lithokraft::components {
namespace {

/// Back in -> front out, one-way. Re-boosts any input to a full 64 after a 1-8 tick delay.
class Repeater final : public ComponentBehavior {
 public:
  ComponentStep tick(const ComponentInputs& in, CellState st, ComponentMemory& m) const override {
    const int f = stFacing(st), delay = stAux(st);  // aux = delay - 1
    m.history = uint8_t((m.history << 1) | (in.back(f) > 0 ? 1 : 0));
    const int out = ((m.history >> delay) & 1) ? kMaxSignal : 0;
    return {out, 0, makeState(out, f, delay, out > 0)};
  }
  std::optional<CellState> use(CellState st, ComponentMemory&) const override {  // cycle delay 1..8
    return makeState(stValue(st), stFacing(st), (stAux(st) + 1) & 7, stOn(st));
  }
  std::string describe(const BlockDef& d, CellState s) const override {
    return std::format("{} | delay {} | out {} | facing {}", d.name, stAux(s) + 1, stValue(s), facingName(stFacing(s)));
  }
};

/// Readout shared by components whose value is their output strength.
std::string describeOutput(const BlockDef& d, CellState s) {
  return std::format("{} | out {} | facing {}", d.name, stValue(s), facingName(stFacing(s)));
}

/// Back = input, left = LOW reference, right = HIGH reference; output = input when it passes.
/// One reference: threshold (in >= ref). Both: window comparator (refs sorted). None: pass-through.
class Thresholdor final : public ComponentBehavior {
 public:
  ComponentStep tick(const ComponentInputs& in, CellState st, ComponentMemory&) const override {
    // SW-H2: pass-through outputs copy the input value — clamp it here (and at
    // the sim's sampling point) so a disk-loaded 65..127 wire value can never
    // propagate into an output. In-range values are untouched.
    const int f = stFacing(st), v = std::min(in.back(f), kMaxSignal), lo = in.left(f), hi = in.right(f);
    const bool pass = (lo > 0 && hi > 0) ? (v >= std::min(lo, hi) && v <= std::max(lo, hi)) : v >= std::max(lo, hi);
    const int out = (pass && v > 0) ? v : 0;
    return {out, 0, makeState(out, f, 0, out > 0)};
  }
  std::string describe(const BlockDef& d, CellState s) const override { return describeOutput(d, s); }
};

/// Linked (default): strongest of back / left / right / wire on top, out the front, any colour.
/// Using it cycles the front N -> E -> S -> W.
/// Output is always colourless: it drives red and blue Trace alike, so a Redirector transfers
/// between colours (red in, blue out and vice versa).
/// Split (aux bit 2): red and blue Trace inputs are routed independently. The facing bits are the
/// red lane's front, aux bits 0-1 the blue lane's; either lane can be switched off (state bits 13 /
/// 14). A colourless input (Catchor) feeds both lanes. Using an adjacent red / blue trace switches
/// to split and cycles that colour's front; using the block then cycles the red front.
class Redirector final : public ComponentBehavior {
 public:
  static bool split(CellState s) { return (stAux(s) & 4) != 0; }
  static int blueFront(CellState s) { return stAux(s) & 3; }
  // Lane off flags (split only; bits 13 / 14, free on a Redirector): an off lane takes no input and
  // drives nothing, so a pixel's unused colour has nowhere to leak through the colourless output.
  static constexpr CellState kRedOff = 1u << 13, kBlueOff = 1u << 14;
  static CellState keep(CellState st) { return CellState(st & (kRedOff | kBlueOff)); }
  ComponentStep tick(const ComponentInputs& in, CellState st, ComponentMemory&) const override {
    const int f = stFacing(st);
    if (!split(st)) {
      const int out = std::min(std::max({in.back(f), in.left(f), in.right(f), in.top}), kMaxSignal);  // SW-H2 clamp
      return {out, 0, makeState(out, f, stAux(st), out > 0)};
    }
    const int bf = blueFront(st);
    int red = 0, blue = 0;
    for (int d = 0; d < 4; ++d) {
      // Neither live lane reads a front (its output would feed back through the colourless Trace);
      // a colourless input (Catchor / component) feeds both lanes.
      if ((d == f && !(st & kRedOff)) || (d == bf && !(st & kBlueOff))) continue;
      if (in.ch[d] == 1 || in.ch[d] == 0) red = std::max(red, in.side[d]);
      if (in.ch[d] == 2 || in.ch[d] == 0) blue = std::max(blue, in.side[d]);
    }
    if (st & kRedOff) red = 0;
    if (st & kBlueOff) blue = 0;
    red = std::min(red, kMaxSignal); blue = std::min(blue, kMaxSignal);  // SW-H2 clamp (pass-through lanes)
    const int v = std::max(red, blue);
    return {red, blue, CellState(makeState(v, f, stAux(st), v > 0) | keep(st))};
  }
  int emitToward(int dir, CellState st, const ComponentMemory& m) const override {
    if (!split(st)) return dir == stFacing(st) ? m.out : 0;
    return std::max(dir == stFacing(st) ? int(m.out) : 0, dir == blueFront(st) ? int(m.out2) : 0);
  }
  std::optional<CellState> use(CellState st, ComponentMemory&) const override {
    return CellState(makeState(stValue(st), (stFacing(st) + 1) & 3, stAux(st), stOn(st)) | keep(st));
  }
  // Using an adjacent Trace cycles that colour's lane: N -> E -> S -> W -> off -> N.
  std::optional<CellState> useChannel(CellState st, int channel) const override {
    int f = stFacing(st), bf = split(st) ? blueFront(st) : stFacing(st);
    CellState off = split(st) ? keep(st) : 0;
    auto step = [&](int& front, CellState bit) {
      if (off & bit) { off &= CellState(~bit); front = 0; }
      else if (front == 3) off |= bit;
      else ++front;
    };
    if (channel == 1) step(f, kRedOff);
    else if (channel == 2) step(bf, kBlueOff);
    else return std::nullopt;
    return CellState(makeState(stValue(st), f, 4 | bf, stOn(st)) | off);
  }
  std::string describe(const BlockDef& d, CellState s) const override {
    if (!split(s)) return describeOutput(d, s);
    return std::format("{} | split | red Trace -> {} | blue Trace -> {} | out {} (colourless)", d.name, (s & kRedOff) ? "off" : facingName(stFacing(s)),
                       (s & kBlueOff) ? "off" : facingName(blueFront(s)), stValue(s));
  }
};

/// Back = data, left/right = clock. A rising clock edge with data > 0 fires a 1-tick pulse of the
/// data strength that travels the connected wire run without decay.
class Pulsor final : public ComponentBehavior {
 public:
  ComponentStep tick(const ComponentInputs& in, CellState st, ComponentMemory& m) const override {
    const int f = stFacing(st), data = std::min(in.back(f), kMaxSignal), clock = std::max(in.left(f), in.right(f));  // SW-H2: data is passed through as the pulse strength
    const bool fire = clock > 0 && m.prevClk == 0 && data > 0;
    m.prevClk = uint8_t(clock);
    m.timer = fire ? kPulsorAnimTicks : (m.timer ? m.timer - 1 : 0);
    const int out = fire ? data : 0;
    ComponentStep step{out, 0, makeState(out, f, std::min<int>(m.timer, 7), m.timer > 0), fire};
    if (fire) step.sound = SoundCue::PulsorFire;
    return step;
  }
  std::string describe(const BlockDef& d, CellState s) const override {
    return std::format("{} | {} | facing {}", d.name, stOn(s) ? "FIRING" : "idle", facingName(stFacing(s)));
  }
};

/// Back = write, front = stored value (continuous), sides = clear on a rising edge. 0 = empty.
class Memor final : public ComponentBehavior {
 public:
  bool quiescent(CellState, const ComponentMemory&) const override { return true; }  // pure in inputs + edges
  ComponentStep tick(const ComponentInputs& in, CellState st, ComponentMemory& m) const override {
    const int f = stFacing(st), v = std::min(in.back(f), kMaxSignal), side = std::max(in.left(f), in.right(f));  // SW-H2: v is stored and re-emitted
    int stored = stValue(st);
    if (side > 0 && m.prevSide == 0) stored = 0;
    if (v > 0 && v != m.prevIn) stored = v;  // a pulse or a rising change stores the new value
    m.prevIn = uint8_t(v);
    m.prevSide = uint8_t(side);
    return {stored, 0, makeState(stored, f, 0, stored > 0)};
  }
  std::string describe(const BlockDef& d, CellState s) const override {
    const int v = stValue(s);
    if (!v) return std::format("{} | empty | facing {}", d.name, facingName(stFacing(s)));
    return std::format("{} | stored {} ('{}') | facing {}", d.name, v, sixbitChar(v - 1), facingName(stFacing(s)));
  }
};

/// Catchor (per-pixel address decoder). Wiring: while unwired it auto-connects to the FIRST wire
/// (red or blue) that touches one of its sides; that side becomes its input and that colour the
/// one it catches. Output: left, right or centre (straight through) relative to the input, never
/// up or down. The tick its input becomes exactly its match value (steady or pulse) it sends the
/// caught value as a one-tick pulse out of the output side (a colour lamp shows it; a Redirector
/// there takes it as its input). The line itself is never driven, so the signal carries on.
/// State: value = match (1-64, 0 = disarmed), facing = direction of travel (input is behind it),
/// aux bits 0-1 = output (0 centre, 1 left, 2 right, 3 = not wired yet), aux bit 2 = blue input.
/// Use cycles the output (centre -> left -> right); sneak + use steps the match value.
class Catchor final : public ComponentBehavior {
 public:
  static bool wired(CellState s) { return (stAux(s) & 3) != 3; }
  // Same inputs and nothing fired last tick: prevIn already equals the match result, out stays 0.
  bool quiescent(CellState s, const ComponentMemory& m) const override { return m.out == 0 && !stOn(s); }
  static int outDir(CellState s) {
    const int f = stFacing(s), m = stAux(s) & 3;
    return m == 1 ? (f + 3) & 3 : m == 2 ? (f + 1) & 3 : f;
  }
  ComponentStep tick(const ComponentInputs& in, CellState st, ComponentMemory& m) const override {
    int f = stFacing(st), aux = stAux(st);
    const int match = stValue(st);
    if ((aux & 3) == 3)
      for (int d = 0; d < 4; ++d)
        if (in.ch[d] == 1 || in.ch[d] == 2) { f = (d + 2) & 3; aux = in.ch[d] == 2 ? 4 : 0; break; }
    int v = 0;
    if ((aux & 3) != 3) {
      const int inDir = (f + 2) & 3;
      if (in.ch[inDir] == ((aux & 4) ? 2 : 1)) v = std::min(in.side[inDir], kMaxSignal);  // SW-H2 clamp
    }
    const bool hit = match > 0 && v == match;
    const bool fire = hit && !m.prevIn;
    m.prevIn = hit;
    const int out = fire ? match : 0;  // the caught value
    return {out, 0, makeState(match, f, aux, fire)};
  }
  int emitToward(int dir, CellState st, const ComponentMemory& m) const override {
    return wired(st) && dir == outDir(st) ? m.out : 0;
  }
  std::optional<CellState> use(CellState st, ComponentMemory& /*m*/) const override {
    if (!wired(st)) return std::nullopt;
    const int mode = ((stAux(st) & 3) + 1) % 3;
    return makeState(stValue(st), stFacing(st), (stAux(st) & 4) | mode, false);
  }
  std::optional<CellState> useAlt(CellState st, ComponentMemory& /*m*/) const override {
    if (wired(st))
      return makeState(stValue(st), stFacing(st), (stAux(st) & ~3) | 3, false);  // clear binding; touch to rebind
    return makeState(stValue(st) % kMaxSignal + 1, stFacing(st), stAux(st), false);
  }
  std::string describe(const BlockDef& d, CellState s) const override {
    if (!wired(s)) return std::format("{} | catches {} | not wired (touch a Trace to a side, sneak+use to set)", d.name, stValue(s));
    static constexpr const char* kOut[3] = {"centre", "left", "right"};
    return std::format("{} | catches {} on {} | in from {} | out {} ({})", d.name, stValue(s), (stAux(s) & 4) ? "blue" : "red",
                       facingName((stFacing(s) + 2) & 3), kOut[stAux(s) & 3], facingName(outDir(s)));
  }
};
/// T flip-flop: each rising edge at the back toggles; the front outputs 64 while on.
class FastLatch final : public ComponentBehavior {
 public:
  ComponentStep tick(const ComponentInputs& in, CellState st, ComponentMemory& m) const override {
    const int f = stFacing(st);
    const bool high = in.back(f) > 0;
    const bool on = (high && !m.prevIn) ? !stOn(st) : stOn(st);
    m.prevIn = high;
    const int out = on ? kMaxSignal : 0;
    return {out, 0, makeState(out, f, 0, on)};
  }
  std::string describe(const BlockDef& d, CellState s) const override {
    return std::format("{} | {} | facing {}", d.name, stOn(s) ? "ON" : "off", facingName(stFacing(s)));
  }
};

}  // namespace


/// Wireless signal source: trace in, radio out. use() toggles input/beacon mode;
/// sneak+use cycles the radio channel. First trace touching a side binds the input colour.
/// Broadcasts (mem.out = strength, mem.out2 = colour); the sim publishes after the tick.
/// Never drives wire (emitToward = 0).
class SignalTransmittor final : public ComponentBehavior {
 public:
  static bool wired(CellState s) { return (stAux(s) & 3) != 3; }
  ComponentStep tick(const ComponentInputs& in, CellState st, ComponentMemory& m) const override {
    int f = stFacing(st), aux = stAux(st);
    if ((aux & 3) == 3)
      for (int d = 0; d < 4; ++d)
        if (in.ch[d] == 1 || in.ch[d] == 2) { f = (d + 2) & 3; aux = in.ch[d] == 2 ? 4 : 0; break; }
    int v = 0, col = 0;
    if (m.beacon) {
      v = kMaxSignal;
      col = (aux & 4) ? 2 : 1;
    } else if ((aux & 3) != 3) {
      const int inDir = (f + 2) & 3;
      if (in.ch[inDir] == ((aux & 4) ? 2 : 1)) { v = std::min(in.side[inDir], kMaxSignal); col = in.ch[inDir]; }  // SW-H2: v is broadcast as the strength
    }
    const bool active = v > 0;
    return {v, col, makeState(stValue(st), f, aux, active)};
  }
  int emitToward(int /*dir*/, CellState /*st*/, const ComponentMemory& /*m*/) const override { return 0; }
  std::optional<CellState> use(CellState st, ComponentMemory& m) const override {
    m.beacon = !m.beacon;
    return st;
  }
  std::optional<CellState> useAlt(CellState st, ComponentMemory& m) const override {
    const uint8_t max = radioChannelsUnlocked();
    m.channel = uint8_t(m.channel % max + 1);
    return st;
  }
  std::string describe(const BlockDef& d, CellState s) const override {
    if (!wired(s)) return std::format("{} | not wired (touch a Trace to a side)", d.name);
    return std::format("{} | {} | {}", d.name, (stAux(s) & 4) ? "blue" : "red",
                       stOn(s) ? "broadcasting" : "idle");
  }
};

/// Wireless signal sink: radio in, trace out. The sim fills in.radioIn / in.radioCol from the
/// radio registry (1-tick latency). sneak+use cycles the listen channel. Outputs on the front,
/// on the received colour.
class SignalReceivor final : public ComponentBehavior {
 public:
  ComponentStep tick(const ComponentInputs& in, CellState st, ComponentMemory& m) const override {
    const int v = std::min(in.radioIn, kMaxSignal);  // SW-H2: received strength is passed through to the wire
    const bool active = v > 0;
    return {v, int(in.radioCol), makeState(stValue(st), stFacing(st), stAux(st), active)};
  }
  int emitChannel(int /*dir*/, CellState /*st*/, const ComponentMemory& m) const override {
    return m.out2 ? m.out2 : 0;  // onto the received colour
  }
  std::optional<CellState> useAlt(CellState st, ComponentMemory& m) const override {
    const uint8_t max = radioChannelsUnlocked();
    m.channel = uint8_t(m.channel % max + 1);
    return st;
  }
  std::string describe(const BlockDef& d, CellState s) const override {
    return std::format("{} | {}", d.name, stOn(s) ? "receiving" : "idle");
  }
};


/// The Dish: a long-range receivor. The sim queries with kDishSensitivity, so it hears
/// far beyond a plain receivor. Listens on its set channel; channel 0 (the Daemon) is
/// monitored when the lore pipeline exists. Single block for v1; multiblock later.
class Dish final : public ComponentBehavior {
 public:
  ComponentStep tick(const ComponentInputs& in, CellState st, ComponentMemory& m) const override {
    const int v = std::min(in.radioIn, kMaxSignal);  // SW-H2: received strength is passed through to the wire
    const bool active = v > 0;
    return {v, int(in.radioCol), makeState(stValue(st), stFacing(st), stAux(st), active)};
  }
  int emitChannel(int /*dir*/, CellState /*st*/, const ComponentMemory& m) const override {
    return m.out2 ? m.out2 : 0;
  }
  std::optional<CellState> useAlt(CellState st, ComponentMemory& m) const override {
    const uint8_t max = radioChannelsUnlocked();
    m.channel = uint8_t(m.channel % max + 1);
    return st;
  }
  std::string describe(const BlockDef& d, CellState s) const override {
    return std::format("{} | {}", d.name, stOn(s) ? "receiving" : "listening");
  }
};


/// The Overclockor: the beacon equivalent. Tick accelerator (tiers later) + ward mode.
/// Accepts items through its interface; the jammer item enables jamming mode: all radio
/// channels except its own are blocked within kJammerRadius. use toggles the jammer item
/// (v1 placeholder for the item UI); sneak+use cycles the jam channel.
class Overclockor final : public ComponentBehavior {
 public:
  ComponentStep tick(const ComponentInputs& /*in*/, CellState st, ComponentMemory& /*m*/) const override {
    return {0, 0, st};
  }
  std::optional<CellState> use(CellState st, ComponentMemory& m) const override {
    m.jammer = !m.jammer;
    return st;
  }
  std::optional<CellState> useAlt(CellState st, ComponentMemory& m) const override {
    const uint8_t max = radioChannelsUnlocked();
    m.channel = uint8_t(m.channel % max + 1);
    return st;
  }
  std::string describe(const BlockDef& d, CellState /*s*/) const override {
    // Note: describe() can't see memory; the game HUD shows the jam state separately.
    return std::format("{} | use: install/remove jammer item, sneak+use: jam channel", d.name);
  }
};


/// Portor (lithography port). Defines input or output for a chip face.
/// Simple by design: use toggles input/output. Wire color auto-detected from
/// adjacent Trace (like Catchor). When the Lithographer prints a chip, each
/// Portor maps to one face. 6 faces = 6 bits = 64 configurations.
/// State: aux bit 0 = output (1) or input (0), aux bit 2 = blue (like Catchor).
class PortOR final : public ComponentBehavior {
 public:
  bool quiescent(CellState, const ComponentMemory&) const override { return true; }
  ComponentStep tick(const ComponentInputs& in, CellState st, ComponentMemory& m) const override {
    // Portor is a passive doorway: input mode reads, output mode passes through.
    // The actual I/O happens at lithography time; in the build area it's just a marker.
    const bool isOutput = (stAux(st) & 1) != 0;
    if (!isOutput) {
      // Input: capture the signal value for the chip to use
      int v = 0;
      for (int d = 0; d < 4; ++d)
        if (in.ch[d] == 1 || in.ch[d] == 2) { v = std::min(in.side[d], kMaxSignal); break; }  // SW-H2 clamp
      m.out = v;
    }
    return {0, 0, st};
  }
  int emitToward(int, CellState, const ComponentMemory&) const override { return 0; }
  std::optional<CellState> use(CellState st, ComponentMemory&) const override {
    // Toggle input/output
    const int aux = stAux(st) ^ 1;
    return makeState(stValue(st), stFacing(st), aux, false);
  }
  std::string describe(const BlockDef& d, CellState s) const override {
    const bool isOutput = (stAux(s) & 1) != 0;
    const bool blue = (stAux(s) & 4) != 0;
    return std::format("{} | {} | {} | use: toggle in/out", d.name,
                       isOutput ? "output" : "input", blue ? "blue" : "red");
  }
};


/// Fusor (circuit breaker). Trips when input signal exceeds its threshold (1-64).
/// When tripped, output drops to 0 until reset. Protects chips from overload.
/// State: value = trip threshold (1-64), aux bit 0 = tripped.
/// Use cycles threshold? No — sneak+use sets threshold (like Catchor match).
/// Use toggles reset (untrip).
class Fusor final : public ComponentBehavior {
 public:
  bool quiescent(CellState s, const ComponentMemory& m) const override {
    return (stAux(s) & 1) != 0;  // tripped = quiescent (no output)
  }
  ComponentStep tick(const ComponentInputs& in, CellState st, ComponentMemory& m) const override {
    const int threshold = stValue(st) > 0 ? stValue(st) : 64;
    const bool tripped = (stAux(st) & 1) != 0;
    int v = 0;
    for (int d = 0; d < 4; ++d)
      if (in.ch[d] == 1 || in.ch[d] == 2) { v = std::min(in.side[d], kMaxSignal); break; }  // SW-H2: v is passed through unless tripped
    bool newTripped = tripped;
    if (!tripped && v > threshold) newTripped = true;  // overload! trip
    const int out = newTripped ? 0 : v;  // pass through unless tripped
    const int aux = newTripped ? (stAux(st) | 1) : (stAux(st) & ~1);
    return {out, 0, makeState(stValue(st), stFacing(st), aux, false)};
  }
  int emitToward(int dir, CellState st, const ComponentMemory& m) const override {
    return ((stAux(st) & 1) == 0) ? m.out : 0;
  }
  std::optional<CellState> use(CellState st, ComponentMemory&) const override {
    // Reset the fuse (untrip)
    return makeState(stValue(st), stFacing(st), stAux(st) & ~1, false);
  }
  std::optional<CellState> useAlt(CellState st, ComponentMemory&) const override {
    // Set threshold (1-64, wraps)
    return makeState(stValue(st) % 64 + 1, stFacing(st), stAux(st), false);
  }
  std::string describe(const BlockDef& d, CellState s) const override {
    const bool tripped = (stAux(s) & 1) != 0;
    return std::format("{} | trips at {} | {} | use: reset, sneak+use: set threshold",
                       d.name, stValue(s) > 0 ? stValue(s) : 64,
                       tripped ? "TRIPPED" : "ok");
  }
};

void registerLogic(const Registrar& add) {
  add("repeater", std::make_unique<Repeater>());
  add("thresholdor", std::make_unique<Thresholdor>());
  add("redirector", std::make_unique<Redirector>());
  add("pulsor", std::make_unique<Pulsor>());
  add("memor", std::make_unique<Memor>());
  add("catchor", std::make_unique<Catchor>());
  add("signal_transmittor", std::make_unique<SignalTransmittor>());
  add("signal_receivor", std::make_unique<SignalReceivor>());
  add("dish", std::make_unique<Dish>());
  add("overclockor", std::make_unique<Overclockor>());
  add("fast_latch", std::make_unique<FastLatch>());
  add("portor", std::make_unique<PortOR>());
  add("fusor", std::make_unique<Fusor>());
}

}  // namespace lithokraft::components
