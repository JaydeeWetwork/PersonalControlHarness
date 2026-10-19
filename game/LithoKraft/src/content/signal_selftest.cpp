// Headless checks of every signal block's logic (run by --selftest).
#include <chrono>
#include <cstdio>
#include "lithokraft/content/component.h"
#include "lithokraft/content/signal.h"
#include "lithokraft/content/appearance.h"
#include "lithokraft/content/blocks.h"
#include "lithokraft/content/circuit_scene.h"
#include "lithokraft/content/signal_world.h"

namespace lithokraft {

int runSignalSelfTest() {
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
  };
  ChunkMap w;
  w.setRegistry(&blockRegistry());
  for (int cz = 0; cz < 4; ++cz)
    for (int cx = 0; cx < 6; ++cx) {
      auto col = std::make_unique<ChunkColumn>(); col->cx = cx; col->cz = cz;
      w.insert(std::move(col));
    }
  const int y = 10;  // flat floor at y-1
  for (int z = 0; z < 64; ++z)
    for (int x = 0; x < 96; ++x) w.set(x, y - 1, z, kWaferSilicon);
  SignalSim sim;
  auto put = [&](int x, int yy, int z, BlockId b) { w.set(x, yy, z, b); sim.onBlockChanged(x, yy, z, b); };
  auto run = [&](int n) { for (int i = 0; i < n; ++i) sim.tick(w, 0, 0, 100, [](int, int, int) {}); };
  const SignalSim::DirtyFn none = [](int, int, int) {};
  auto putF = [&](int x, int yy, int z, BlockId b, int f) { w.set(x, yy, z, b); w.setState(x, yy, z, makeState(0, f)); sim.onBlockChanged(x, yy, z, b); };
  auto setW = [&](int x, int yy, int z, int s) { w.setState(x, yy, z, CellState(s)); sim.invalidateNetwork(); };  // drive a wire for one tick
  auto val = [&](int x, int yy, int z) { return stValue(w.getState(x, yy, z)); };

  std::printf("LithoLattice sim self-test\n");
  // A) torch -> 10 red trace -> lamp, plus an unpowered blue trace alongside.
  put(0, y, 0, kRedTorch);
  for (int x = 1; x <= 10; ++x) { put(x, y, 0, kRedTrace); put(x, y, 1, kBlueTrace); }
  put(11, y, 0, kLamp);
  // B) 70-long run: signal must die after 64 blocks.
  put(0, y, 4, kRedTorch);
  for (int x = 1; x <= 70; ++x) put(x, y, 4, kRedTrace);
  put(71, y, 4, kLamp);
  // C) inverter: torch -> wire (gap) -> block -> torch on block -> wire -> lamp.
  put(0, y, 8, kRedTorch);
  put(1, y, 8, kRedTrace); put(3, y, 8, kRedTrace);  // gap at x=2
  put(4, y, 8, kWaferSilicon);
  put(4, y + 1, 8, kRedTorch);
  for (int x = 5; x <= 8; ++x) { put(x, y, 8, kWaferSilicon); put(x, y + 1, 8, kRedTrace); }
  put(9, y + 1, 8, kLamp);
  // D) staircase: wire climbs one block, then a block above cuts it.
  put(0, y, 12, kRedTorch);
  put(1, y, 12, kRedTrace); put(2, y, 12, kWaferSilicon); put(2, y + 1, 12, kRedTrace);
  put(0, y, 14, kRedTorch);
  put(1, y, 14, kRedTrace); put(1, y + 1, 14, kWaferSilicon);  // cut: block above the low wire
  put(2, y, 14, kWaferSilicon); put(2, y + 1, 14, kRedTrace);

  run(3);
  check(w.getState(0, y, 0) == 1, "torch on unpowered block is lit");
  check(w.getState(1, y, 0) == 64 && w.getState(2, y, 0) == 63 && w.getState(10, y, 0) == 55,
        "red wire strength 64 at torch, decays 1 per block (55 after 10)");
  check(w.getState(11, y, 0) == 1, "lamp lit by red wire");
  bool blueDark = true;
  for (int x = 1; x <= 10; ++x) blueDark &= w.getState(x, y, 1) == 0;
  check(blueDark, "blue wire beside red wire stays at 0 (colours never connect)");
  check(w.getState(64, y, 4) == 1 && w.getState(65, y, 4) == 0 && w.getState(71, y, 4) == 0,
        "signal reaches 64 blocks then dies; lamp at 71 stays off");
  check(w.getState(4, y + 1, 8) == 1 && w.getState(9, y + 1, 8) == 1,
        "inverter: block unpowered -> torch lit -> lamp lit");
  put(2, y, 8, kRedTrace);  // close the gap
  run(1);
  check(w.getState(4, y + 1, 8) == 1 && w.getState(3, y, 8) > 0, "tick 1 after closing gap: wire powers block, torch still lit (1-tick inverter delay)");
  run(1);
  check(w.getState(4, y + 1, 8) == 0 && w.getState(9, y + 1, 8) == 0, "tick 2: torch off, downstream lamp off");
  put(2, y, 8, kAir);  // open again
  run(2);
  check(w.getState(9, y + 1, 8) == 1, "after re-opening gap: lamp back on");
  check(w.getState(2, y + 1, 12) == 63, "wire climbs a staircase (63 one block up)");
  check(w.getState(2, y + 1, 14) == 0, "staircase cut by a block above the lower wire");
  put(10, y, 0, kAir);
  run(1);
  check(w.getState(11, y, 0) == 0, "breaking the last wire turns the lamp off");
  // E) torch colours: each torch feeds only its own wire colour; any colour powers lamps/blocks.
  put(0, y, 18, kRedTorch); put(1, y, 18, kBlueTrace);
  put(0, y, 20, kBlueTorch); put(1, y, 20, kBlueTrace); put(2, y, 20, kBlueTrace); put(3, y, 20, kLamp);
  put(5, y, 22, kBlueTorch); put(6, y, 22, kRedTrace);
  // blue trace powering the block under a red torch turns that red torch off
  put(10, y, 24, kBlueTorch); put(11, y, 24, kBlueTrace); put(12, y, 24, kWaferSilicon); put(12, y + 1, 24, kRedTorch);
  run(3);
  check(w.getState(1, y, 18) == 0, "red torch does not power blue wire");
  check(w.getState(6, y, 22) == 0, "blue torch does not power red wire");
  check(w.getState(1, y, 20) == 64 && w.getState(2, y, 20) == 63, "blue torch powers blue wire (64, 63)");
  check(w.getState(3, y, 20) == 1, "lamp lit by blue wire");
  check(w.getState(12, y + 1, 24) == 0, "blue wire powering a block turns the red torch on it off");
  // F) torch powers the solid block above it; that block emits colourless power.
  put(20, y, 26, kBlueTorch); put(20, y + 1, 26, kWaferSilicon); put(20, y + 2, 26, kRedTrace);
  put(21, y, 26, kWaferSilicon); put(21, y + 1, 26, kRedTrace); put(22, y, 26, kWaferSilicon); put(22, y + 1, 26, kLamp);
  put(19, y, 26, kWaferSilicon); put(19, y + 1, 26, kBlueTrace);
  put(30, y, 26, kRedTorch); put(30, y + 1, 26, kWaferSilicon); put(31, y, 26, kWaferSilicon); put(31, y + 1, 26, kLamp);
  put(40, y, 26, kRedTorch); put(40, y + 1, 26, kWaferSilicon); put(40, y + 2, 26, kBlueTorch);  // torch tower
  put(50, y, 26, kRedTorch); put(50, y + 1, 26, kLamp);
  put(60, y, 28, kRedTorch); put(61, y + 1, 28, kRedTrace); put(61, y, 28, kWaferSilicon);  // air above torch
  run(3);
  check(w.getState(21, y + 1, 26) == 64 && w.getState(19, y + 1, 26) == 64 && w.getState(20, y + 2, 26) == 64,
        "blue torch powers block above; block feeds red, blue and on-top wire at 64");
  check(w.getState(22, y + 1, 26) == 1, "lamp lit through wire from a torch-powered block");
  check(w.getState(31, y + 1, 26) == 1, "lamp lit directly by an adjacent torch-powered block");
  check(w.getState(40, y + 2, 26) == 0, "torch on a torch-powered block is off (torch tower)");
  check(w.getState(50, y + 1, 26) == 1, "lamp directly above a torch is lit");
  check(w.getState(61, y + 1, 28) == 0, "air above a torch carries no power");
  put(40, y, 26, kAir);
  run(2);
  check(w.getState(40, y + 2, 26) == 1, "removing the lower torch relights the tower torch");

  // G) Repeater: re-boost to 64, one-way, delay 1-8 (right-click cycles).
  constexpr int E = 1, W = 3;
  put(0, y, 34, kSwitch); for (int x = 1; x <= 3; ++x) put(x, y, 34, kRedTrace);
  putF(4, y, 34, kRepeater, E); for (int x = 5; x <= 8; ++x) put(x, y, 34, kBlueTrace);
  put(0, y, 36, kSwitch); put(1, y, 36, kRedTrace); putF(2, y, 36, kRepeater, W); put(3, y, 36, kRedTrace);
  sim.use(w, 0, y, 34, none); sim.use(w, 0, y, 36, none);
  run(4);
  check(val(3, y, 34) == 62 && val(5, y, 34) == 64 && val(8, y, 34) == 61, "repeater re-boosts 62 -> 64 into the other colour");
  check(val(1, y, 36) == 64 && val(3, y, 36) == 0, "repeater is one-way (front never feeds back)");
  put(0, y, 38, kSwitch); put(1, y, 38, kRedTrace); putF(2, y, 38, kRepeater, E); put(3, y, 38, kRedTrace);
  put(0, y, 40, kSwitch); put(1, y, 40, kRedTrace); putF(2, y, 40, kRepeater, E); put(3, y, 40, kRedTrace);
  for (int i = 0; i < 3; ++i) sim.use(w, 2, y, 38, none);  // delay 1 -> 4
  check(stAux(w.getState(2, y, 38)) == 3, "right-click cycles repeater delay (1 -> 4)");
  sim.use(w, 0, y, 38, none); sim.use(w, 0, y, 40, none);
  int t4 = -1, t1 = -1;
  for (int t = 1; t <= 12; ++t) {
    run(1);
    if (t1 < 0 && val(3, y, 40) > 0) t1 = t;
    if (t4 < 0 && val(3, y, 38) > 0) t4 = t;
  }
  check(t1 > 0 && t4 - t1 == 3, "repeater delay 4 is 3 ticks slower than delay 1");

  // H) Thresholdor: back = input, left = LOW, right = HIGH; output = input strength.
  putF(10, y, 46, kThresholdor, E); put(9, y, 46, kRedTrace); put(10, y, 45, kRedTrace); put(10, y, 47, kRedTrace);
  put(11, y, 46, kRedTrace);
  auto thr = [&](int in, int lo, int hi) { setW(9, y, 46, in); setW(10, y, 45, lo); setW(10, y, 47, hi); run(1); return val(10, y, 46); };
  check(thr(30, 0, 0) == 30, "thresholder: no reference -> passes the input");
  check(thr(30, 20, 0) == 30 && thr(10, 20, 0) == 0, "thresholder: LOW ref only = threshold (in >= ref)");
  check(thr(30, 0, 40) == 0 && thr(50, 0, 40) == 50, "thresholder: HIGH ref only = threshold too");
  check(thr(30, 20, 40) == 30 && thr(45, 20, 40) == 0 && thr(10, 20, 40) == 0, "thresholder: both refs = window comparator");
  check(thr(30, 40, 20) == 30, "thresholder: swapped refs are sorted (min/max)");
  thr(33, 0, 0);
  check(val(11, y, 46) == 33, "thresholder output drives its front wire at the input strength");

  // I) Redirector: any side in, front out.
  putF(20, y, 46, kRedirector, E); put(20, y, 45, kBlueTrace); put(21, y, 46, kRedTrace); put(19, y, 46, kRedTrace);
  setW(20, y, 45, 25); run(1);
  check(val(20, y, 46) == 25 && val(21, y, 46) == 25, "redirector: side input 25 comes out the front");
  setW(21, y, 46, 30); run(1);
  check(val(20, y, 46) == 0, "redirector ignores signal arriving at its front");

  // J) Memory cell: back = write, sides = clear, front = stored value.
  putF(30, y, 46, kMemor, E); put(29, y, 46, kRedTrace); put(30, y, 45, kRedTrace); put(31, y, 46, kBlueTrace);
  setW(29, y, 46, 17); run(1);
  check(val(30, y, 46) == 17, "memory cell stores a pulse of 17");
  run(3);
  check(val(30, y, 46) == 17 && val(31, y, 46) == 17, "memory cell holds 17 and outputs it continuously");
  setW(29, y, 46, 40); run(1);
  check(val(30, y, 46) == 40, "memory cell overwritten by a new value (40)");
  setW(30, y, 45, 5); run(1);
  check(val(30, y, 46) == 0, "memory cell cleared by a side pulse (0 = empty)");

  // K) Pulsor -> 38 wires -> memory cell: the pulse keeps its strength (no decay).
  putF(2, y, 50, kPulsor, E); put(1, y, 50, kRedTrace); put(2, y, 49, kBlueTrace);
  for (int x = 3; x <= 40; ++x) put(x, y, 50, kRedTrace);
  putF(41, y, 50, kMemor, E);
  setW(1, y, 50, 23); setW(2, y, 49, 10); run(1);
  check(val(3, y, 50) == 23 && val(40, y, 50) == 23 && stOn(w.getState(2, y, 50)), "pulsar: clock edge fires data 23 down 38 wires without decay");
  run(1);
  check(val(41, y, 50) == 23 && val(20, y, 50) == 0, "pulse lasts 1 tick and the memory cell stored 23");
  // Hold the clock high with a switch on the right side: one pulse on the edge, then nothing.
  w.set(2, y, 51, kSwitch); w.setState(2, y, 51, 0); sim.onBlockChanged(2, y, 51, kSwitch);
  run(2);
  sim.use(w, 2, y, 51, none);
  int pulses = 0;
  for (int i = 0; i < 6; ++i) { setW(1, y, 50, 9); run(1); pulses += val(20, y, 50) == 9; }
  check(pulses == 1, "pulsar: clock held high fires once (rising edge only), carrying the new data (9)");
  sim.use(w, 2, y, 51, none); run(2);
  sim.use(w, 2, y, 51, none);
  pulses = 0;
  for (int i = 0; i < 4; ++i) { setW(1, y, 50, 12); run(1); pulses += val(20, y, 50) == 12; }
  check(pulses == 1, "pulsar: next rising clock edge fires again (12)");

  // L) Fast latch: each rising edge at the back toggles the front (binary 64).
  putF(50, y, 46, kFastLatch, E); put(49, y, 46, kRedTrace); put(51, y, 46, kRedTrace);
  setW(49, y, 46, 10); run(1);
  const bool l1 = stOn(w.getState(50, y, 46)) && val(51, y, 46) == 64;
  setW(49, y, 46, 10); run(1); run(1);
  const bool l2 = stOn(w.getState(50, y, 46));
  setW(49, y, 46, 5); run(1);
  check(l1 && l2 && !stOn(w.getState(50, y, 46)) && val(51, y, 46) == 0, "fast latch toggles on a pulse, holds, toggles off on the next");

  // M) Switch and button.
  put(60, y, 46, kSwitch); put(61, y, 46, kRedTrace); put(59, y, 46, kBlueTrace);
  sim.use(w, 60, y, 46, none); run(1);
  check(val(61, y, 46) == 64 && val(59, y, 46) == 64, "switch on powers wire on every side (64)");
  sim.use(w, 60, y, 46, none); run(1);
  check(val(61, y, 46) == 0, "switch off");
  put(70, y, 46, kButton); put(71, y, 46, kRedTrace);
  sim.use(w, 70, y, 46, none);
  int pressed = 0;
  for (int i = 0; i < 20; ++i) { run(1); pressed += val(71, y, 46) > 0; }
  check(pressed == kButtonTicks, "button pulses for 10 ticks");

  // N) Keyboard: SIXBIT char pulses out the front (code + 1), Enter/Backspace out the right.
  putF(80, y, 46, kKeyboard, E); put(81, y, 46, kRedTrace); put(80, y, 47, kBlueTrace);
  check(sixbitCode('A') == 33 && sixbitCode('a') == 33 && sixbitCode('0') == 16 && sixbitCode(' ') == 0 &&
        sixbitCode('_') == 63 && sixbitCode('~') == -1 && sixbitChar(33) == 'A', "SIXBIT mapping (A=33, 0=16, space=0, _=63)");
  sim.use(w, 80, y, 46, none);
  check(stOn(w.getState(80, y, 46)), "right-click toggles keyboard typing capture");
  sim.keyPulse(80, y, 46, sixbitCode('A') + 1, 0); run(1);
  const int kA = val(81, y, 46);
  run(1);
  const int kA2 = val(81, y, 46);
  sim.keyPulse(80, y, 46, 0, kKeyEnter); run(1);
  check(kA == 34 && kA2 == 0, "keyboard: 'A' = 1-tick pulse of 34 out the front");
  check(val(80, y, 47) == 1 && val(81, y, 46) == 0, "keyboard: Enter = 1 on the right control port");

  // O) Color lamps: wrap mapping, terminate the signal; plain lamp lit by a component.
  put(0, y, 56, kSwitch); for (int x = 1; x <= 4; ++x) put(x, y, 56, kRedTrace);
  put(5, y, 56, kColorLamp16); put(6, y, 56, kRedTrace);
  put(0, y, 58, kSwitch); for (int x = 1; x <= 4; ++x) put(x, y, 58, kRedTrace); put(5, y, 58, kColorLamp64);
  put(90, y, 56, kSwitch); put(91, y, 56, kLamp);
  sim.use(w, 0, y, 56, none); sim.use(w, 0, y, 58, none); sim.use(w, 90, y, 56, none);
  run(3);
  check(val(5, y, 56) == 61 && colorLampIndex(61, 16) == 12 && colorLampIndex(61, 64) == 60 && val(5, y, 58) == 61,
        "color lamps take strength 61: tier 16 -> colour 12 (wrap), tier 64 -> 60");
  check(colorLampIndex(0, 16) == -1 && colorLampIndex(1, 16) == 0 && colorLampIndex(17, 16) == 0 && colorLampIndex(33, 32) == 0,
        "wrap mapping: 0 off, 1 = first colour, N+1 wraps");
  check(val(6, y, 56) == 0, "lamps terminate the signal (wire behind a color lamp stays 0)");
  check(val(91, y, 56) == 1, "plain lamp lit by an adjacent switch");
  // P) Tone Block: back strength = pitch; a Tone cue on every change, 0 = silent; consumes the signal.
  {
    std::vector<SoundEvent> ev;
    sim.takeSounds(ev);  // everything since the start: the pulsor tests fired with data 23, 9 and 12
    int fired = 0;
    for (const SoundEvent& e : ev) fired += e.cue == SoundCue::PulsorFire && (e.value == 23 || e.value == 9 || e.value == 12);
    check(fired == 3, "pulsar: every fire reports a PulsorFire sound cue carrying its data strength");
    ev.clear();
    putF(41, y, 60, kToneBlock, E); put(40, y, 60, kRedTrace); put(42, y, 60, kRedTrace);
    for (int i = 0; i < 3; ++i) { setW(40, y, 60, 34); run(1); }
    sim.takeSounds(ev);
    const bool on = ev.size() == 1 && ev[0].cue == SoundCue::Tone && ev[0].value == 34 && ev[0].x == 41 && val(42, y, 60) == 0;
    ev.clear();
    run(2);
    sim.takeSounds(ev);
    check(on && ev.size() == 1 && ev[0].value == 0 && val(41, y, 60) == 0,
          "tone block: s=34 -> one Tone cue (A4), held without repeats; 0 -> silent cue; output stays 0");
  }
  std::printf("%s (%d failure%s)\n", fails ? "SELFTEST FAILED" : "SELFTEST OK", fails, fails == 1 ? "" : "s");
  {  // Attachables: a switch on top of a block powers the block (lamp beside it) and the dust
     // directly below the block; a wall torch turns off when its support is powered.
    auto putA = [&](int x, int yy, int z, BlockId b, int face) {
      w.set(x, yy, z, b); w.setState(x, yy, z, withAttach(0, face)); sim.onBlockChanged(x, yy, z, b);
    };
    put(82, y + 2, 60, kWaferSilicon);
    putA(82, y + 3, 60, kSwitch, 1);
    put(82, y + 1, 60, kRedTrace);
    put(83, y + 2, 60, kLamp);
    put(86, y, 60, kWaferSilicon);
    putA(85, y, 60, kRedTorch, 6);
    putA(86, y + 1, 60, kSwitch, 1);
    run(3);
    const bool before = val(82, y + 1, 60) == 0 && w.getState(83, y + 2, 60) == 0 && stValue(w.getState(85, y, 60)) == 1;
    sim.use(w, 82, y + 3, 60, none);
    sim.use(w, 86, y + 1, 60, none);
    run(4);
    check(before && val(82, y + 1, 60) == 64 && w.getState(83, y + 2, 60) == 1,
          "attached switch on top powers its support (lamp beside it) and the dust below the support");
    check(stValue(w.getState(85, y, 60)) == 0 && stAttach(w.getState(85, y, 60)) == 6 && stAttach(w.getState(82, y + 3, 60)) == 1,
          "wall torch turns off when its support is powered; attach faces survive ticks and toggles");
  }
  {  // Attached button (on a wall): pulses its support for kButtonTicks; the lamp beside it follows.
    put(88, y + 2, 60, kWaferSilicon);
    w.set(87, y + 2, 60, kButton); w.setState(87, y + 2, 60, withAttach(0, 6)); sim.onBlockChanged(87, y + 2, 60, kButton);
    put(88, y + 3, 60, kLamp);
    run(2);
    const bool idle = w.getState(88, y + 3, 60) == 0;
    sim.use(w, 87, y + 2, 60, none);
    run(3);
    const bool lit = w.getState(88, y + 3, 60) == 1 && stAttach(w.getState(87, y + 2, 60)) == 6;
    run(kButtonTicks + 4);
    check(idle && lit && w.getState(88, y + 3, 60) == 0, "attached button pulses its support (lamp on, then off) and keeps its attach face");
    const LithoKraftAppearance look;
    check(look.boxModel(blockRegistry().def(kButton)) != nullptr && look.boxModel(blockRegistry().def(kSwitch)) != nullptr,
          "button and switch have ll-box-1 box models");
  }
  {  // Display bench scene: the ring clock starts and keeps firing the cache banks into the lamps.
    ChunkMap dw;
    dw.setRegistry(&blockRegistry());
    for (int cz = 0; cz < 4; ++cz)
      for (int cx = 0; cx < 4; ++cx) { auto col = std::make_unique<ChunkColumn>(); col->cx = cx; col->cz = cz; dw.insert(std::move(col)); }
    buildDisplayScene(dw, 0, y, 0, 40);
    SignalSim ds;
    ds.scan(dw);
    int litTicks = 0, edges = 0; bool prev = false;
    for (int i = 0; i < 400; ++i) {
      ds.tick(dw, 0, 0, 100, [](int, int, int) {});
      const bool on = stValue(dw.getState(20, y, 1 + 5 + 2)) > 0;  // a lamp in bank 0
      litTicks += on; edges += on && !prev; prev = on;
    }
    std::printf("  display scene: lamp lit %d / 400 ticks, %d flashes\n", litTicks, edges);
    check(edges >= 8, "display scene: ring clock runs and the bank fires its stored colour repeatedly");
  }
  {  // Framebuffer scene: the controller ring sequences the frame cache and the screen scrolls.
    ChunkMap fw;
    fw.setRegistry(&blockRegistry());
    for (int cz = 0; cz < 8; ++cz)
      for (int cx = 0; cx < 6; ++cx) { auto col = std::make_unique<ChunkColumn>(); col->cx = cx; col->cz = cz; fw.insert(std::move(col)); }
    const FramebufferLayout fl = buildFramebufferScene(fw, 0, y, 0, 64);
    SignalSim fs;
    fs.scan(fw);
    const int memX = 4 + 2, memZ = fl.screenZ0 + 1 + 4 * 3 - 1;  // stage 3's memory cell
    int changes = 0, lampFlashes = 0, last = stValue(fw.getState(memX, y, memZ));
    bool prevLit = false;
    for (int i = 0; i < 2000; ++i) {
      fs.tick(fw, 0, 0, 100, [](int, int, int) {});
      const int v = stValue(fw.getState(memX, y, memZ));
      if (v != last) { ++changes; last = v; }
      const bool lit = stValue(fw.getState(30, y + 1, memZ + 2)) > 0;  // a lamp of stage 3
      lampFlashes += lit && !prevLit; prevLit = lit;
    }
    std::printf("  framebuffer scene: %d stages... stage-3 cell changed %d times, lamp flashed %d times in 2000 ticks\n",
                (fl.screenZ1 - fl.screenZ0) / 4, changes, lampFlashes);
    check(changes >= 20 && lampFlashes >= 20, "framebuffer scene: cache controller runs and the screen shifts frames down");
  }
  {  // Incremental re-flood == full pass: same scene, one sim forced to a full pass every tick.
    auto make = [&](ChunkMap& m) {
      for (int cz = 0; cz < 8; ++cz)
        for (int cx = 0; cx < 6; ++cx) { auto col = std::make_unique<ChunkColumn>(); col->cx = cx; col->cz = cz; m.insert(std::move(col)); }
      buildFramebufferScene(m, 0, y, 0, 64);
      buildDisplayScene(m, 0, y, 100, 24);
    };
    ChunkMap a, b;
    a.setRegistry(&blockRegistry());
    b.setRegistry(&blockRegistry());
    make(a); make(b);
    SignalSim sa, sb;
    sa.scan(a); sb.scan(b);
    sa.setLampHoldTicks(5); sb.setLampHoldTicks(5);
    int mismatch = 0;
    for (int i = 0; i < 1500 && !mismatch; ++i) {
      sa.tick(a, 0, 0, 100, [](int, int, int) {});
      sb.invalidateNetwork(); sb.tick(b, 0, 0, 100, [](int, int, int) {});
      for (int z = 0; z < 128; ++z)
        for (int x = 0; x < 96; ++x)
          for (int yy = y; yy <= y + 1; ++yy) mismatch += a.getState(x, yy, z) != b.getState(x, yy, z);
    }
    check(mismatch == 0, "incremental per-network re-flood matches a full pass every tick (framebuffer + display, 1500 ticks)");
  }
  {  // Upright framebuffer wall: wire stairs carry clock, data and links up the back face.
    ChunkMap vw;
    vw.setRegistry(&blockRegistry());
    for (int cz = 0; cz < 4; ++cz)
      for (int cx = 0; cx < 6; ++cx) { auto col = std::make_unique<ChunkColumn>(); col->cx = cx; col->cz = cz; vw.insert(std::move(col)); }
    const FramebufferLayout vl = buildFramebufferWall(vw, 0, y, 0, 64);
    SignalSim vs;
    vs.scan(vw);
    vs.setLampHoldTicks(17);  // as in game at 512 TPS
    int litTicks = 0;
    const int sx = 10, memY = y + 3 + 4 * 6 + 1;  // stage 6's memory cell (high up the wall)
    int changes = 0, flashes = 0, last = stValue(vw.getState(sx - 1, memY, vl.wallZ + 1));
    bool prevLit = false;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 2000; ++i) {
      vs.tick(vw, 0, 0, 100, [](int, int, int) {});
      const int v = stValue(vw.getState(sx - 1, memY, vl.wallZ + 1));
      if (v != last) { ++changes; last = v; }
      const bool lit = stValue(vw.getState(sx + 30, memY, vl.wallZ)) > 0;  // a lamp on that band
      flashes += lit && !prevLit; prevLit = lit; litTicks += lit;
    }
    std::printf("  framebuffer wall: lamp lit %d / 2000 ticks with afterglow 17\n", litTicks);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 2000.0;
    std::printf("  framebuffer wall: stage-6 cell changed %d times, lamp flashed %d times in 2000 ticks (%.3f ms/tick incl. probes, %zu blocks)\n", changes, flashes, ms, size_t(vl.blocks));
    check(changes >= 20 && litTicks > 1900, "framebuffer wall: vertical wire stairs carry clock + data, frames shift up the wall");
  }
  {  // Redirector split routing: each pixel lights only from its intended wire colour.
    ChunkMap rw;
    rw.setRegistry(&blockRegistry());
    for (int cz = 0; cz < 2; ++cz)
      for (int cx = 0; cx < 3; ++cx) { auto col = std::make_unique<ChunkColumn>(); col->cx = cx; col->cz = cz; rw.insert(std::move(col)); }
    constexpr int n = 8, x0 = 4, z0 = 4;
    buildRedirectorScene(rw, x0, y, z0, n);
    SignalSim rs;
    rs.scan(rw);
    auto runR = [&](int k) { for (int i = 0; i < k; ++i) rs.tick(rw, 0, 0, 100, [](int, int, int) {}); };
    auto lamp = [&](int i) { return stValue(rw.getState(x0 + 2 * i + 1, y, z0 + 1)); };  // pixel i
    auto setBlock = [&](int x, int z, BlockId b) { rw.set(x, y, z, b); rs.onBlockChanged(x, y, z, b); };
    auto pattern = [&](bool red, bool blue) {
      for (int i = 0; i < n; ++i) { const bool want = (i & 1) ? blue : red; if ((lamp(i) > 0) != want) return false; }
      return true;
    };
    runR(4);
    const bool both = pattern(true, true);
    setBlock(x0 - 1, z0 + 2, kAir); runR(4);
    const bool redOnly = pattern(true, false);
    setBlock(x0 - 1, z0 + 2, kBlueTorch); setBlock(x0 - 1, z0, kAir); runR(4);
    const bool blueOnly = pattern(false, true);
    check(both && redOnly && blueOnly, "redirector scene: every pixel lights only from its own wire colour (red / blue / both)");
    setBlock(x0 - 1, z0, kRedTorch); runR(4);
    const bool lit0 = lamp(0) > 0;
    rs.use(rw, x0, y, z0, [](int, int, int) {});  // use the red trace next to Redirector 0: red front E -> S
    runR(4);
    const CellState st = rw.getState(x0, y, z0 + 1);
    check(lit0 && lamp(0) == 0 && stFacing(st) == 2 && (stAux(st) & 3) == 0 && pattern(false, true) == false,
          "redirector: using the adjacent red wire cycles only the red front (pixel 0 goes dark, blue front unchanged)");
  }
  {  // Vias (vertical, lossless, colour-isolated) and glass (cuts wire stairs).
    ChunkMap vw;
    vw.setRegistry(&blockRegistry());
    { auto col = std::make_unique<ChunkColumn>(); col->cx = 0; col->cz = 0; vw.insert(std::move(col)); }
    for (int x = 0; x < 12; ++x) for (int z = 0; z < 6; ++z) vw.set(x, y - 1, z, kWaferSilicon);
    vw.set(1, y, 1, kRedTorch); vw.set(2, y, 1, kRedTrace);
    for (int yy = y + 1; yy <= y + 3; ++yy) { vw.set(2, yy, 1, kRedVia); vw.set(3, yy, 1, kBlueVia); }
    vw.set(2, y + 4, 1, kRedTrace); vw.set(3, y + 4, 1, kBlueTrace);
    vw.set(4, y, 3, kRedTorch); vw.set(5, y, 3, kRedTrace); vw.set(6, y + 1, 3, kRedTrace); vw.set(5, y + 1, 3, kGlass);
    vw.set(4, y, 5, kRedTorch); vw.set(5, y, 5, kRedTrace); vw.set(6, y + 1, 5, kRedTrace);
    SignalSim vs;
    vs.scan(vw);
    for (int i = 0; i < 4; ++i) vs.tick(vw, 0, 0, 100, [](int, int, int) {});
    const int bot = stValue(vw.getState(2, y, 1)), top = stValue(vw.getState(2, y + 4, 1));
    check(bot == 64 && top == 64 && stValue(vw.getState(2, y + 2, 1)) == 64,
          "via: a red via stack carries the wire up losslessly (64 bottom -> 64 top)");
    check(stValue(vw.getState(3, y + 4, 1)) == 0 && stValue(vw.getState(3, y + 2, 1)) == 0,
          "via: a blue via standing against a lit red via stays dark (no sideways links)");
    check(stValue(vw.getState(6, y + 1, 3)) == 0 && stValue(vw.getState(6, y + 1, 5)) == 63,
          "glass: glass over a wire blocks the stair climb (control stair without glass is lit)");
  }
  {  // Wall-pixel: Catchor decoders off per-row Pulsor lines; each pixel shows its caught value,
     // lights alone within its row, left to right; rows are isolated; no wire floats.
    for (const int rows : {1, 16}) {
      ChunkMap ww;
      ww.setRegistry(&blockRegistry());
      for (int cz = 0; cz < 5; ++cz)
        for (int cx = 0; cx < 4; ++cx) { auto col = std::make_unique<ChunkColumn>(); col->cx = cx; col->cz = cz; ww.insert(std::move(col)); }
      constexpr int px0 = 24, zw = 44, n = 16, yb = 60;
      buildWallPixelScene(ww, px0, yb, zw, n, rows);
      SignalSim ws;
      ws.scan(ww);
      std::vector<int> first(size_t(n * rows), -1), count(size_t(n * rows), 0);
      bool alone = true, value = true;
      for (int t = 0; t < 2500; ++t) {
        ws.tick(ww, 0, 0, 100, [](int, int, int) {});
        for (int r = 0; r < rows; ++r) {
          int lit = 0;
          for (int i = 0; i < n; ++i) {
            const int v = stValue(ww.getState(px0 + i, yb + 1 + 2 * r, zw));
            if (!v) continue;
            ++lit; ++count[size_t(r * n + i)];
            if (v != i + 1) value = false;
            if (first[size_t(r * n + i)] < 0) first[size_t(r * n + i)] = t;
          }
          if (lit > 1) alone = false;
        }
      }
      bool all = true, ordered = true;
      for (int r = 0; r < rows; ++r)
        for (int i = 0; i < n; ++i) {
          if (count[size_t(r * n + i)] < 2) all = false;
          if (i && first[size_t(r * n + i)] <= first[size_t(r * n + i - 1)]) ordered = false;
        }
      int floating = 0;
      for (int z = 0; z < 80; ++z) for (int x = 0; x < 64; ++x) for (int yy = 1; yy < kWorldHeight - 1; ++yy)
        if (isFlatWire(ww.get(x, yy, z)) && !wireSupported(ww, x, yy, z)) ++floating;
      std::printf("  wall-pixel %d row(s): px0 fired %d, last px fired %d in 2500 ticks; %d floating wires\n",
                  rows, count[0], count[size_t(n * rows - 1)], floating);
      if (rows > 1) { std::printf("   per row (px0/px15):"); for (int r = 0; r < rows; ++r) std::printf(" %d/%d", count[size_t(r * n)], count[size_t(r * n + n - 1)]); std::printf("\n"); }
      check(all && alone && ordered && value && floating == 0,
            rows == 1 ? "wall-pixel 1 row: every pixel fires repeatedly, alone, left to right, showing its caught value"
                      : "wall-pixel 16 rows: per-row drivers + vias, every pixel of every row fires alone in order with its value");
      if (rows == 1) {
        ws.use(ww, px0 + 3, yb + 1, zw + 1, [](int, int, int) {}, true);
        check((stAux(ww.getState(px0 + 3, yb + 1, zw + 1)) & 3) == 3, "catchor: sneak + use (alt-use command) clears the binding for rebind");
      }
    }
  }
  {  // Wire torture: stair / cut / ceiling / via / colour rules, one tiny fixture each.
    constexpr int Y = 60;
    auto fixture = [&](auto&& build, int ticks = 12) {
      ChunkMap m;
      m.setRegistry(&blockRegistry());
      { auto col = std::make_unique<ChunkColumn>(); col->cx = 0; col->cz = 0; m.insert(std::move(col)); }
      for (int x = 0; x < 16; ++x) for (int z = 0; z < 16; ++z) m.set(x, Y - 1, z, kWaferSilicon);
      build(m);
      SignalSim s; s.scan(m);
      for (int t = 0; t < ticks; ++t) s.tick(m, 0, 0, 100, [](int, int, int) {});
      return m;
    };
    auto val = [&](const ChunkMap& m, int x, int y, int z) { return int(stValue(m.getState(x, y, z))); };
    struct StepCase { BlockId b; const char* name; };
    const StepCase steps[] = {{kWaferSilicon, "silicon"}, {kGlass, "glass"}, {kFrostedGlass, "frosted glass"}, {kCeramic, "ceramic"}, {kSiliconSlab, "silicon slab"}};
    int stairBad = 0, cutBad = 0;
    for (const auto& sc : steps) {
      // Up: torch -> A (floor) -> C on top of the step block. Down: torch on the step -> A.
      const ChunkMap up = fixture([&](ChunkMap& m) { m.set(2, Y, 4, kRedTorch); m.set(3, Y, 4, kRedTrace); m.set(4, Y, 4, sc.b); m.set(4, Y + 1, 4, kRedTrace); m.set(5, Y + 1, 4, kRedTrace); });
      const ChunkMap dn = fixture([&](ChunkMap& m) { m.set(4, Y, 4, sc.b); m.set(5, Y, 4, kWaferSilicon); m.set(6, Y + 1, 4, kRedTorch); m.set(5, Y + 1, 4, kRedTrace); m.set(4, Y + 1, 4, kRedTrace); m.set(3, Y, 4, kRedTrace); });
      const bool upOk = val(up, 4, Y + 1, 4) == 63 && val(up, 5, Y + 1, 4) == 62, dnOk = val(dn, 3, Y, 4) == 62;
      // Cut: the same climb with the block placed over the lower wire instead (blocks the stair if it cuts).
      const ChunkMap cut = fixture([&](ChunkMap& m) { m.set(2, Y, 4, kRedTorch); m.set(3, Y, 4, kRedTrace); m.set(3, Y + 1, 4, sc.b); m.set(4, Y, 4, kWaferSilicon); m.set(4, Y + 1, 4, kRedTrace); });
      const bool cuts = blockRegistry().has(sc.b, kCutsWire);
      const bool cutOk = cuts ? val(cut, 4, Y + 1, 4) == 0 : val(cut, 4, Y + 1, 4) == 63;
      std::printf("  stairs over %-13s: up %d/%d down %d | block over the lower wire -> upper %d (%s)\n", sc.name, val(up, 4, Y + 1, 4),
                  val(up, 5, Y + 1, 4), val(dn, 3, Y, 4), val(cut, 4, Y + 1, 4), cuts ? "should cut" : "should pass");
      stairBad += !(upOk && dnOk); cutBad += !cutOk;
    }
    check(stairBad == 0, "wire stairs: climb onto / descend from silicon, glass, frosted glass, ceramic and slab steps (1 per block)");
    check(cutBad == 0, "wire stairs: silicon / glass / frosted glass / ceramic over the lower wire cut the stair; a slab does not");
    // Ceiling wire: hangs under glass, carries signal sideways.
    const ChunkMap ceil = fixture([&](ChunkMap& m) {
      for (int x = 2; x <= 6; ++x) { m.set(x, Y + 3, 6, kGlass); m.set(x, Y + 2, 6, x == 2 ? kRedTorch : kRedTrace); }
    });
    check(val(ceil, 3, Y + 2, 6) == 64 && val(ceil, 6, Y + 2, 6) == 61, "ceiling wire: hangs under glass and carries the signal (64 -> 61 over 4)");
    // Vias: lossless climb, no sideways link, colours isolated; red / blue traces side by side never link.
    const ChunkMap via = fixture([&](ChunkMap& m) {
      m.set(2, Y, 8, kRedTorch); m.set(3, Y, 8, kRedTrace);
      for (int y = Y + 1; y <= Y + 8; ++y) m.set(3, y, 8, kRedVia);
      m.set(3, Y + 9, 8, kRedTrace); m.set(4, Y + 9, 8, kRedTrace); m.set(4, Y + 8, 8, kGlass); m.set(3, Y + 8, 7, kGlass);
      m.set(4, Y + 4, 8, kGlass); m.set(4, Y + 5, 8, kRedTrace);  // beside the via: must stay dark
      m.set(3, Y, 9, kBlueTrace); m.set(4, Y, 9, kBlueTrace);     // blue next to the red trace / torch
    });
    const bool viaOk = val(via, 3, Y + 9, 8) == val(via, 3, Y, 8) && val(via, 4, Y + 9, 8) == val(via, 3, Y, 8) - 1 && val(via, 4, Y + 5, 8) == 0 &&
                       val(via, 3, Y, 9) == 0 && val(via, 4, Y, 9) == 0;
    std::printf("  via: bottom %d top %d after-top %d beside %d | blue beside red %d\n", val(via, 3, Y, 8), val(via, 3, Y + 9, 8), val(via, 4, Y + 9, 8),
                val(via, 4, Y + 5, 8), val(via, 3, Y, 9));
    check(viaOk, "via: 8-high climb is lossless, nothing links to its side, blue never picks up red");
    // Repeater through a block: drives the silicon block in front (powerable) and the wire on top of it.
    const ChunkMap rep = fixture([&](ChunkMap& m) {
      m.set(2, Y, 11, kRedTorch); m.set(3, Y, 11, kRedTrace); m.set(4, Y, 11, kRepeater); m.setState(4, Y, 11, makeState(0, 1, 0));
      m.set(5, Y, 11, kWaferSilicon); m.set(5, Y + 1, 11, kRedTrace); m.set(6, Y, 11, kRedTrace);
    }, 20);
    const ChunkMap repUnder = fixture([&](ChunkMap& m) {  // wire under the block (hanging below it) and a repeater reading it
      m.set(2, Y + 3, 13, kRedTorch); m.set(3, Y + 3, 13, kRedTrace); m.set(4, Y + 3, 13, kRepeater); m.setState(4, Y + 3, 13, makeState(0, 1, 0));
      for (int x = 2; x <= 4; ++x) m.set(x, Y + 2, 13, kGlass);
      m.set(5, Y + 3, 13, kWaferSilicon); m.set(5, Y + 2, 13, kRedTrace); m.set(6, Y + 2, 13, kWaferSilicon);
      m.set(6, Y + 3, 13, kRepeater); m.setState(6, Y + 3, 13, makeState(0, 1, 0)); m.set(7, Y + 3, 13, kRedTrace); m.set(7, Y + 2, 13, kGlass);
    }, 20);
    const ChunkMap insul = fixture([&](ChunkMap& m) {  // the same into glass and ceramic: never powered
      m.set(2, Y, 14, kRedTorch); m.set(3, Y, 14, kRedTrace); m.set(4, Y, 14, kRepeater); m.setState(4, Y, 14, makeState(0, 1, 0));
      m.set(5, Y, 14, kGlass); m.set(5, Y + 1, 14, kRedTrace); m.set(6, Y, 14, kRedTrace);
      m.set(2, Y, 15, kRedTorch); m.set(3, Y, 15, kRedTrace); m.set(4, Y, 15, kRepeater); m.setState(4, Y, 15, makeState(0, 1, 0));
      m.set(5, Y, 15, kCeramic); m.set(5, Y + 1, 15, kRedTrace); m.set(6, Y, 15, kRedTrace);
    }, 20);
    std::printf("  repeater -> silicon: top %d beyond %d | under %d, repeater behind it -> %d | glass top %d beyond %d, ceramic top %d beyond %d\n",
                val(rep, 5, Y + 1, 11), val(rep, 6, Y, 11), val(repUnder, 5, Y + 2, 13), val(repUnder, 7, Y + 3, 13),
                val(insul, 5, Y + 1, 14), val(insul, 6, Y, 14), val(insul, 5, Y + 1, 15), val(insul, 6, Y, 15));
    check(val(rep, 5, Y + 1, 11) > 0 && val(rep, 6, Y, 11) > 0 && val(repUnder, 5, Y + 2, 13) > 0 && val(repUnder, 7, Y + 3, 13) > 0,
          "repeater hard-powers the silicon block it faces: wire on top, beyond, hanging underneath and a repeater behind it all pick it up");
    check(val(insul, 5, Y + 1, 14) == 0 && val(insul, 6, Y, 14) == 0 && val(insul, 5, Y + 1, 15) == 0 && val(insul, 6, Y, 15) == 0,
          "repeater into glass / ceramic: never powered, nothing beyond lights");
    // Perf: a 48 x 48 serpentine of stairs (up / down every block) flipped on / off every tick.
    {
      ChunkMap m;
      m.setRegistry(&blockRegistry());
      for (int cz = 0; cz < 4; ++cz) for (int cx = 0; cx < 4; ++cx) { auto col = std::make_unique<ChunkColumn>(); col->cx = cx; col->cz = cz; m.insert(std::move(col)); }
      int wires = 0;
      for (int z = 2; z < 50; ++z) for (int x = 2; x < 50; ++x) {
        m.set(x, Y - 1, z, kWaferSilicon);
        const bool up = (x + z) & 1;
        if (up) m.set(x, Y, z, kWaferSilicon);
        if ((z % 2 == 0) || x == ((z / 2) % 2 ? 2 : 49)) { m.set(x, up ? Y + 1 : Y, z, kRedTrace); ++wires; }
      }
      SignalSim s; s.scan(m);
      const auto t0 = std::chrono::steady_clock::now();
      int lit = 0;
      for (int t = 0; t < 400; ++t) {
        for (int z = 2; z < 50; z += 2) {  // a torch at every row start: the whole field flips each tick
          const BlockId b = (t & 1) ? kAir : kRedTorch;
          m.set(1, Y - 1, z, kWaferSilicon); m.set(1, Y, z, b); s.onBlockChanged(1, Y, z, b);
        }
        s.tick(m, 0, 0, 100, [](int, int, int) {});
        if (t == 398) for (int z = 2; z < 50; ++z) for (int x = 2; x < 50; ++x) for (int yy = Y; yy <= Y + 1; ++yy) lit += isFlatWire(m.get(x, yy, z)) && stValue(m.getState(x, yy, z)) > 0;
      }
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 400;
      std::printf("  wire torture perf: %d stair wires toggled every tick: %.3f ms/tick (%.0f TPS max), %d lit at peak\n", wires, ms, 1000.0 / ms, lit);
    }
  }
  {  // Component sleep: skipping quiescent components gives exactly the same world every tick.
    struct Case { const char* name; int kind; };
    for (const Case c : {Case{"pattern wall 32", 0}, Case{"wire torture", 1}, Case{"wall-pixel 16", 2}, Case{"sawtooth", 3}}) {
      ChunkMap a, b;
      a.setRegistry(&blockRegistry());
      b.setRegistry(&blockRegistry());
      for (ChunkMap* m : {&a, &b}) {
        for (int cz = 0; cz < 7; ++cz) for (int cx = 0; cx < 7; ++cx) { auto col = std::make_unique<ChunkColumn>(); col->cx = cx; col->cz = cz; m->insert(std::move(col)); }
        if (c.kind == 0) buildPatternWall(*m, 16, 60, 44, 32, "circles");
        else if (c.kind == 1) buildWireTorture(*m, 8, 60, 8);
        else if (c.kind == 2) buildWallPixelScene(*m, 24, 60, 44, 16, 16);
        else buildSawtoothWall(*m, 8, 60, 20, 16, 4, "circles");
      }
      SignalSim sa, sb; sa.setComponentSleep(true); sb.setComponentSleep(false);
      sa.scan(a); sb.scan(b);
      int diffTicks = 0; size_t ticked = 0, total = 0;
      const auto t0 = std::chrono::steady_clock::now(); double msA = 0, msB = 0;
      for (int t = 0; t < 600; ++t) {
        auto ta = std::chrono::steady_clock::now();
        sa.tick(a, 0, 0, 100, [](int, int, int) {});
        auto tb = std::chrono::steady_clock::now();
        sb.tick(b, 0, 0, 100, [](int, int, int) {});
        auto tc = std::chrono::steady_clock::now();
        msA += std::chrono::duration<double, std::milli>(tb - ta).count(); msB += std::chrono::duration<double, std::milli>(tc - tb).count();
        ticked += sa.lastTickedComponents(); total += sb.lastTickedComponents();
        bool same = true;
        if (t % 7 == 0 || t < 50)
          for (int z = 0; z < 112 && same; ++z) for (int x = 0; x < 112 && same; ++x) for (int yy = 40; yy < 200 && same; ++yy)
            if (a.getState(x, yy, z) != b.getState(x, yy, z)) same = false;
        diffTicks += !same;
      }
      (void)t0;
      std::printf("  component sleep, %s: %d differing ticks; ticked %.0f%% of components; %.3f vs %.3f ms/tick\n", c.name, diffTicks,
                  total ? 100.0 * double(ticked) / double(total) : 0.0, msA / 600, msB / 600);
      check(diffTicks == 0, "component sleep: identical world every checked tick (pattern wall, wire torture, wall-pixel, sawtooth)");
    }
  }
  {  // Redirector output is colourless: red Trace in -> blue Trace out and back; a Catchor feeds both split lanes.
    constexpr int Y = 60;
    auto run = [&](auto&& build) {
      auto m = std::make_unique<ChunkMap>();
      m->setRegistry(&blockRegistry());
      { auto col = std::make_unique<ChunkColumn>(); col->cx = 0; col->cz = 0; m->insert(std::move(col)); }
      for (int x = 0; x < 16; ++x) for (int z = 0; z < 16; ++z) m->set(x, Y - 1, z, kWaferSilicon);
      build(*m);
      SignalSim s; s.scan(*m);
      for (int t = 0; t < 12; ++t) s.tick(*m, 0, 0, 100, [](int, int, int) {});
      return m;
    };
    for (const bool redIn : {true, false}) {
      const auto m = run([&](ChunkMap& w) {
        w.set(2, Y, 4, redIn ? kRedTorch : kBlueTorch); w.set(3, Y, 4, redIn ? kRedTrace : kBlueTrace);
        w.set(4, Y, 4, kRedirector); w.setState(4, Y, 4, makeState(0, 1));  // unsplit, out east
        w.set(5, Y, 4, redIn ? kBlueTrace : kRedTrace); w.set(6, Y, 4, redIn ? kBlueTrace : kRedTrace);
      });
      std::printf("  redirector %s -> %s: out trace %d, next %d\n", redIn ? "red" : "blue", redIn ? "blue" : "red", int(stValue(m->getState(5, Y, 4))), int(stValue(m->getState(6, Y, 4))));
      check(stValue(m->getState(5, Y, 4)) > 0 && stValue(m->getState(6, Y, 4)) > 0,
            redIn ? "redirector transfers red Trace in -> blue Trace out (colourless output)" : "redirector transfers blue Trace in -> red Trace out (colourless output)");
    }
    {  // Split lanes: red lane front east into blue Trace (cross-colour), blue lane front north into red Trace.
      const auto m = run([&](ChunkMap& w) {
        w.set(2, Y, 8, kRedTorch); w.set(3, Y, 8, kRedTrace);
        w.set(4, Y, 8, kRedirector); w.setState(4, Y, 8, makeState(0, 1, 4 | 0));  // red lane -> E, blue lane -> N
        w.set(5, Y, 8, kBlueTrace); w.set(4, Y, 7, kRedTrace);
      });
      check(stValue(m->getState(5, Y, 8)) > 0 && stValue(m->getState(4, Y, 7)) == 0, "split redirector: red lane out its front into blue Trace; idle blue lane stays dark");
    }
    {  // Colourless Catchor input feeds both split lanes (documented): both fronts flash.
      int north = 0, south = 0;
      auto m = std::make_unique<ChunkMap>();
      m->setRegistry(&blockRegistry());
      { auto col = std::make_unique<ChunkColumn>(); col->cx = 0; col->cz = 0; m->insert(std::move(col)); }
      for (int x = 0; x < 16; ++x) for (int z = 0; z < 16; ++z) m->set(x, Y - 1, z, kWaferSilicon);
      m->set(2, Y, 12, kRedTorch); m->set(3, Y, 12, kRedTrace);
      m->set(4, Y, 12, kCatchor); m->setState(4, Y, 12, makeState(kMaxSignal, 0, 3));
      m->set(5, Y, 12, kRedirector); m->setState(5, Y, 12, makeState(0, 0, 4 | 2));
      m->set(5, Y, 11, kColorLamp64); m->set(5, Y, 13, kColorLamp64);
      SignalSim s; s.scan(*m);
      for (int t = 0; t < 12; ++t) {
        s.tick(*m, 0, 0, 100, [](int, int, int) {});
        north = std::max(north, int(stValue(m->getState(5, Y, 11)))); south = std::max(south, int(stValue(m->getState(5, Y, 13))));
      }
      check(north > 0 && south > 0, "catchor (colourless) -> split redirector: feeds both lanes, both fronts fire");
    }
  }
  {  // Wire-torture scene: field flips every tick, Catchor chain fires, hard power works, nothing floats.
    ChunkMap m;
    m.setRegistry(&blockRegistry());
    for (int cz = 0; cz < 7; ++cz) for (int cx = 0; cx < 5; ++cx) { auto col = std::make_unique<ChunkColumn>(); col->cx = cx; col->cz = cz; m.insert(std::move(col)); }
    constexpr int X = 8, Yt = 60, Z = 8;
    buildWireTorture(m, X, Yt, Z);
    SignalSim s; s.scan(m);
    int flips = 0, prev = -1, catchFires = 0, hard = 0;
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kT = 300;
    for (int t = 0; t < kT; ++t) {
      s.tick(m, 0, 0, 100, [](int, int, int) {});
      const int v = stValue(m.getState(X + 47, Yt, Z + kTortureField + 10));  // deep in the field
      if (prev >= 0 && (v > 0) != (prev > 0)) ++flips;
      prev = v;
      for (int i = 4; i <= 8; i += 2) catchFires += stValue(m.getState(X + i, Yt + 1, Z + 26)) > 0;
      hard = stValue(m.getState(X + 3, Yt + 1, Z + 15));
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kT;
    int floating = 0, lampGlass = stValue(m.getState(X + 5, Yt, Z + 17)) + stValue(m.getState(X + 5, Yt, Z + 19));
    for (int z = 0; z < 112; ++z) for (int x = 0; x < 80; ++x) for (int yy = Yt; yy < Yt + 14; ++yy)
      if (isFlatWire(m.get(x, yy, z)) && !wireSupported(m, x, yy, z)) ++floating;
    std::printf("  wire-torture scene: field flips %d / %d ticks, catchor lamp-ticks %d, hard-powered top %d, glass/ceramic lamps %d, %d floating, %.3f ms/tick\n",
                flips, kT - 1, catchFires, hard, lampGlass, floating, ms);
    check(flips >= (kT - 1) * 9 / 10 && catchFires > 0 && hard > 0 && lampGlass == 0 && floating == 0,
          "wire-torture scene: stair field flips every tick, Catchor -> Redirector chain fires, hard power only through silicon, no floating wire");
  }
  {  // Sawtooth wall: every pixel's own wire is independent (packed red / blue side by side).
    constexpr int W = 16, Rr = 4, px0 = 8, zw = 20, yb = 60;
    auto make = [&](ChunkMap& m) {
      for (int cz = 0; cz < 3; ++cz) for (int cx = 0; cx < 3; ++cx) { auto col = std::make_unique<ChunkColumn>(); col->cx = cx; col->cz = cz; m.insert(std::move(col)); }
      buildSawtoothWall(m, px0, yb, zw, W, Rr, "circles");
    };
    auto lampAt = [&](const ChunkMap& m, int i, int r) { return stValue(m.getState(px0 + i, yb + 1 + 2 * r, zw)); };
    // Full image
    ChunkMap full;
    full.setRegistry(&blockRegistry());
    make(full); SignalSim fs; fs.scan(full);
    for (int t = 0; t < 40; ++t) fs.tick(full, 0, 0, 100, [](int, int, int) {});
    int wrongFull = 0, crosstalk = 0;
    for (int r = 0; r < Rr; ++r) for (int i = 0; i < W; ++i)
      if (lampAt(full, i, r) != std::clamp(patternColor("circles", W, i, r), 1, kMaxSignal - kSawtoothRun)) ++wrongFull;
    // One pixel at a time: clear every Memor but one; only that lamp may light.
    for (int r = 0; r < Rr; ++r) for (int i = 0; i < W; i += 3) {
      ChunkMap one;
      one.setRegistry(&blockRegistry());
      make(one);
      for (int rr = 0; rr < Rr; ++rr) for (int ii = 0; ii < W; ++ii)
        if (rr != r || ii != i) one.setState(px0 + ii, yb + 1 + 2 * rr, zw + kSawtoothRun + 1, makeState(0, 0));
      SignalSim os; os.scan(one);
      for (int t = 0; t < 40; ++t) os.tick(one, 0, 0, 100, [](int, int, int) {});
      for (int rr = 0; rr < Rr; ++rr) for (int ii = 0; ii < W; ++ii)
        if ((rr == r && ii == i) != (lampAt(one, ii, rr) > 0)) ++crosstalk;
    }
    int floating = 0;
    for (int z = 0; z < 48; ++z) for (int x = 0; x < 48; ++x) for (int yy = 50; yy < 80; ++yy)
      if (isFlatWire(full.get(x, yy, z)) && !wireSupported(full, x, yy, z)) ++floating;
    std::printf("  sawtooth %dx%d: %d wrong colours, %d cross-talk cells, %d floating\n", W, Rr, wrongFull, crosstalk, floating);
    check(wrongFull == 0 && crosstalk == 0 && floating == 0,
          "sawtooth wall: red / blue wires packed side by side, every pixel independent (single-pixel drive lights only it), pattern colours exact");
  }
  {  // Pattern wall: each Catchor matches its pixel's pattern colour; shared drivers sweep the colours
     // up Red / Blue Via spines; every pixel lights with exactly its colour; no wire floats.
    for (const auto& [sz, pat] : {std::pair{32, "circles"}, std::pair{64, "checkers"}}) {
      ChunkMap pw;
      pw.setRegistry(&blockRegistry());
      for (int cz = 0; cz < 5; ++cz)
        for (int cx = 0; cx < 7; ++cx) { auto col = std::make_unique<ChunkColumn>(); col->cx = cx; col->cz = cz; pw.insert(std::move(col)); }
      constexpr int px0 = 16, zw = 44, yb = 60;
      buildPatternWall(pw, px0, yb, zw, sz, pat);
      SignalSim ps;
      ps.scan(pw);
      std::vector<int> count(size_t(sz * sz), 0);
      int wrong = 0;
      const auto t0 = std::chrono::steady_clock::now();
      constexpr int kTicks = 2000;
      for (int t = 0; t < kTicks; ++t) {
        ps.tick(pw, 0, 0, 100, [](int, int, int) {});
        for (int r = 0; r < sz; ++r)
          for (int i = 0; i < sz; ++i) {
            const int v = stValue(pw.getState(px0 + i, yb + 1 + 2 * r, zw));
            if (!v) continue;
            ++count[size_t(r * sz + i)];
            if (v != patternColor(pat, sz, i, r)) ++wrong;
          }
      }
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kTicks;
      const auto t1 = std::chrono::steady_clock::now();
      for (int t = 0; t < 1000; ++t) ps.tick(pw, 0, 0, 100, [](int, int, int) {});
      { const auto p = ps.takePhaseMs(); std::printf("  pattern wall %dx%d phases (ms/tick over 3000): comps %.3f mark %.3f wires %.3f lamps %.3f\n", sz, sz, p.comps / 3000, p.mark / 3000, p.wires / 3000, p.lamps / 3000); }
      std::printf("  pattern wall %dx%d: pure sim %.3f ms/tick (1000 ticks, no probes)\n", sz, sz, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count() / 1000);
      int dark = 0, minC = 1 << 30;
      for (int c : count) { if (c < 2) ++dark; minC = std::min(minC, c); }
      int floating = 0;
      for (int z = 0; z < 80; ++z) for (int x = 0; x < 112; ++x) for (int yy = 1; yy < 200; ++yy)
        if (isFlatWire(pw.get(x, yy, z)) && !wireSupported(pw, x, yy, z)) ++floating;
      std::printf("  pattern wall %dx%d %s: %d dark, %d wrong-colour lights, min %d lights/pixel in %d ticks, %d floating, %.3f ms/tick (incl. readback)\n",
                  sz, sz, pat, dark, wrong, minC, kTicks, floating, ms);
      check(dark == 0 && wrong == 0 && floating == 0,
            sz == 32 ? "pattern wall 32x32 circles: every pixel lights repeatedly with exactly its pattern colour, no floating wire"
                     : "pattern wall 64x64 checkers: every pixel lights repeatedly with exactly its pattern colour, no floating wire");
    }
  }
  {  // Catchor wiring: auto-connects to the first wire touching a side, output centre / left / right
     // relative to that input (never up / down), state persists; a Catchor feeding a Redirector
     // makes it the Redirector's input (centre / left / right cycling only).
    ChunkMap cw;
    cw.setRegistry(&blockRegistry());
    { auto col = std::make_unique<ChunkColumn>(); col->cx = 0; col->cz = 0; cw.insert(std::move(col)); }
    for (int x = 0; x < 12; ++x) for (int z = 0; z < 8; ++z) cw.set(x, y - 1, z, kWaferSilicon);
    SignalSim cs;
    auto setB = [&](int x, int z, BlockId b, CellState st = 0) { cw.set(x, y, z, b); if (st) cw.setState(x, y, z, st); cs.onBlockChanged(x, y, z, b); };
    auto lit = [&](int x, int z) { return stValue(cw.getState(x, y, z)) > 0; };
    cw.set(2, y, 3, kCatchor); cw.setState(2, y, 3, makeState(64, 0, 3));  // unwired, catches 64
    cw.set(3, y, 3, kColorLamp64); cw.set(2, y, 2, kColorLamp64); cw.set(2, y, 4, kColorLamp64);
    cs.scan(cw);
    setB(1, 3, kRedTrace); setB(0, 3, kRedTorch);  // the first wire to touch: west side, red
    std::array<bool, 3> seen{};  // E (centre), N (left), S (right)
    auto runC = [&](int k) { seen = {}; for (int i = 0; i < k; ++i) { cs.tick(cw, 0, 0, 100, [](int, int, int) {}); seen[0] |= lit(3, 3); seen[1] |= lit(2, 2); seen[2] |= lit(2, 4); } };
    auto retrigger = [&] { setB(0, 3, kAir); runC(3); setB(0, 3, kRedTorch); runC(6); };
    runC(6);
    const CellState w0 = cw.getState(2, y, 3);
    const bool autoWired = stFacing(w0) == 1 && (stAux(w0) & 7) == 0 && seen[0] && !seen[1] && !seen[2];
    setB(2, 4, kAir); setB(2, 4, kBlueTrace); runC(3);  // a later blue trace does not rewire it
    const bool kept = cw.getState(2, y, 3) == makeState(64, 1, 0, stOn(cw.getState(2, y, 3)));
    setB(2, 4, kColorLamp64);
    cs.use(cw, 2, y, 3, [](int, int, int) {}); retrigger();
    const bool left = seen[1] && !seen[0] && !seen[2];
    cs.use(cw, 2, y, 3, [](int, int, int) {}); retrigger();
    const bool right = seen[2] && !seen[0] && !seen[1];
    cs.use(cw, 2, y, 3, [](int, int, int) {});
    const bool back = (stAux(cw.getState(2, y, 3)) & 3) == 0;
    cs.use(cw, 2, y, 3, [](int, int, int) {}, true);  // bound -> sneak+use clears the binding
    const bool unbound = (stAux(cw.getState(2, y, 3)) & 3) == 3 && stValue(cw.getState(2, y, 3)) == 64;
    cs.use(cw, 2, y, 3, [](int, int, int) {}, true);  // unbound -> sneak+use steps the match (64 -> 1)
    const bool alt = stValue(cw.getState(2, y, 3)) == 1 && (stAux(cw.getState(2, y, 3)) & 3) == 3;
    cw.setState(2, y, 3, makeState(64, 1, 0));  // rebind directly for the redirector test below
    check(autoWired && kept && left && right && back && unbound && alt,
          "catchor: auto-wires to the first touching wire (west, red), keeps it; output centre -> left -> right; sneak+use clears binding, then steps match");
    // Catchor -> Redirector: replace the east lamp with a Redirector (facing east) + lamps around it.
    setB(3, 3, kRedirector, makeState(0, 1)); setB(4, 3, kColorLamp64); setB(3, 2, kColorLamp64); setB(3, 4, kColorLamp64);
    auto runR = [&](int k) { seen = {}; for (int i = 0; i < k; ++i) { cs.tick(cw, 0, 0, 100, [](int, int, int) {}); seen[0] |= lit(4, 3); seen[1] |= lit(3, 2); seen[2] |= lit(3, 4); } };
    auto again = [&] { setB(0, 3, kAir); runR(3); setB(0, 3, kRedTorch); runR(8); };
    again();
    const bool rc = seen[0] && !seen[1] && !seen[2];
    cs.use(cw, 3, y, 3, [](int, int, int) {}); again();
    const bool rl = seen[1] && !seen[0] && !seen[2] && stFacing(cw.getState(3, y, 3)) == 0;
    cs.use(cw, 3, y, 3, [](int, int, int) {}); again();
    const bool rr = seen[2] && !seen[0] && !seen[1] && stFacing(cw.getState(3, y, 3)) == 2;
    cs.use(cw, 3, y, 3, [](int, int, int) {});
    const bool rBack = stFacing(cw.getState(3, y, 3)) == 1;  // centre again, never west into the Catchor
    check(rc && rl && rr && rBack, "catchor -> redirector: the Catchor is its input; use cycles centre -> left -> right (never back, never up/down)");
  }
  {  // Wire support: placement needs a floor or a ceiling; losing both pops the wire.
    ChunkMap sw;
    sw.setRegistry(&blockRegistry());
    { auto col = std::make_unique<ChunkColumn>(); col->cx = 0; col->cz = 0; sw.insert(std::move(col)); }
    SignalWorldSimulation sim;
    sw.set(2, y - 1, 2, kWaferSilicon); sw.set(4, y + 1, 2, kGlass);
    const bool onFloor = sim.canPlace(sw, 2, y, 2, kRedTrace), underCeiling = sim.canPlace(sw, 4, y, 2, kRedTrace);
    const bool floating = sim.canPlace(sw, 6, y, 2, kRedTrace), viaFree = sim.canPlace(sw, 6, y, 2, kRedVia);
    sw.set(2, y, 2, kRedTrace);
    const bool kept = sim.stillSupported(sw, 2, y, 2);
    sw.set(2, y - 1, 2, kAir);
    check(onFloor && underCeiling && !floating && viaFree && kept && !sim.stillSupported(sw, 2, y, 2),
          "wire support: on a block or under a ceiling only; vias need none; removing the support un-supports it");
  }
  {  // No bench scene leaves a wire floating.
    auto audit = [&](auto&& build) {
      ChunkMap aw;
      aw.setRegistry(&blockRegistry());
      for (int cz = 0; cz < 8; ++cz)
        for (int cx = 0; cx < 8; ++cx) { auto col = std::make_unique<ChunkColumn>(); col->cx = cx; col->cz = cz; aw.insert(std::move(col)); }
      build(aw);
      int bad = 0;
      for (int z = 0; z < 128; ++z) for (int x = 0; x < 128; ++x) for (int yy = 1; yy < kWorldHeight - 1; ++yy)
        if (isFlatWire(aw.get(x, yy, z)) && !wireSupported(aw, x, yy, z)) ++bad;
      return bad;
    };
    const int c = audit([&](ChunkMap& m) { buildCircuitScene(m, 4, y, 4, 64); });
    const int d = audit([&](ChunkMap& m) { buildDisplayScene(m, 4, y, 4, 64); });
    const int f = audit([&](ChunkMap& m) { buildFramebufferScene(m, 4, y, 4, 64); });
    const int v = 0;  // legacy stacked-wire wall retired: --screen-orient vertical now builds wall-pixel
    const int r = audit([&](ChunkMap& m) { buildRedirectorScene(m, 8, y, 8, 8); });
    std::printf("  floating wires: circuits %d, display %d, framebuffer %d, wall %d, redirector %d\n", c, d, f, v, r);
    check(c + d + f + v + r == 0, "bench scenes: no floating wire (every wire on a block or under a ceiling)");

  }
  {  // Wireless: transmittor -> receivor across a gap no wire crosses.
    // TX at (10, y, 30) with a red torch to its west (binds red input); RX at (40, y, 30)
    // drives a lamp. Both on channel 1 (default). 30 blocks apart: within TX power.
    putF(10, y, 30, kSignalTransmittor, 1);  // facing E; back (W) is the input side
    put(9, y, 30, kRedTorch);
    putF(40, y, 30, kSignalReceivor, 1);
    put(41, y, 30, kRedTrace);
    put(42, y, 30, kLamp);
    run(6);
    check(val(42, y, 30) > 0, "wireless: lamp lights through transmittor -> receivor (no wire between)");
    // Channel isolation: sneak+use cycles the receivor to channel 2; the ch-1 TX goes unheard.
    sim.use(w, 40, y, 30, none, true);
    run(6);
    check(val(42, y, 30) == 0, "wireless: channel isolation (ch-2 receivor ignores ch-1 broadcast)");
    // Cycle back to channel 1 (2 -> 3 -> 4 -> 1 with 4 unlocked): lamp relights.
    sim.use(w, 40, y, 30, none, true);
    sim.use(w, 40, y, 30, none, true);
    sim.use(w, 40, y, 30, none, true);
    run(6);
    check(val(42, y, 30) > 0, "wireless: sneak+use channel cycling returns to ch 1");
    // Beacon mode: regular use toggles the transmittor to self-generate; break the torch feed.
    w.set(9, y, 30, kAir); sim.onBlockChanged(9, y, 30, kAir);
    run(3);
    check(val(42, y, 30) == 0, "wireless: input mode goes silent with no input");
    sim.use(w, 10, y, 30, none, false);  // toggle to beacon
    run(6);
    check(val(42, y, 30) > 0, "wireless: beacon mode transmits without wire input");
  }
  {  // Jammer: Overclockor + jammer item blocks all channels except its own.
    // TX at (10, y, 50) torch-fed; RX at (40, y, 50) + lamp; Overclockor at (25, y, 50).
    putF(10, y, 50, kSignalTransmittor, 1);
    put(9, y, 50, kRedTorch);
    putF(40, y, 50, kSignalReceivor, 1);
    put(41, y, 50, kRedTrace);
    put(42, y, 50, kLamp);
    put(25, y, 50, kOverclockor);
    run(6);
    check(val(42, y, 50) > 0, "jammer: no jammer installed, signal passes");
    sim.use(w, 25, y, 50, none, false);  // install the jammer item (v1: use toggles)
    run(6);
    check(val(42, y, 50) > 0, "jammer: jammer on ch 1 allows ch-1 traffic through");
    sim.use(w, 40, y, 50, none, true);  // receivor to ch 2; TX still on ch 1
    sim.use(w, 10, y, 50, none, true);  // TX to ch 2 as well
    run(6);
    check(val(42, y, 50) == 0, "jammer: ch-2 traffic jammed by ch-1 jammer");
    sim.use(w, 25, y, 50, none, true);  // jammer to ch 2: now the allowed channel
    run(6);
    check(val(42, y, 50) > 0, "jammer: ch-2 traffic passes when jammer allows ch 2");
  }
  {  // RadioRegistry unit checks: link budget, strongest-wins, channel filter.
    RadioRegistry r;
    int s = 0, c = 0;
    r.publish(RadioBroadcast{0, 10, 0, 1, 1, 64, 1, 256});
    r.publish(RadioBroadcast{200, 10, 0, 1, 1, 32, 2, 256});
    r.publish(RadioBroadcast{1000, 10, 0, 1, 1, 48, 1, 256});
    r.flip();
    bool heard = r.query(1, 150, 10, 0, 1, 0, s, c);
    check(heard && s == 64 && c == 1, "radio: strongest in-range broadcast wins");
    heard = r.query(1, 500, 10, 0, 1, 0, s, c);  // nearest (B at 200) is 300 > 256: out of range
    check(!heard, "radio: out-of-range broadcast unheard (link budget)");
    heard = r.query(1, 500, 10, 0, 1, 100, s, c);  // +100 sensitivity: 300 <= 356, hears B
    check(heard && s == 32 && c == 2, "radio: receivor sensitivity extends range (the Dish hears better)");
    heard = r.query(2, 200, 10, 0, 1, 0, s, c);
    check(!heard, "radio: channel filter (nothing on ch 2)");
    r.flip();  // no new publishes: registry goes silent
    heard = r.query(1, 200, 10, 0, 1, 0, s, c);
    check(!heard, "radio: broadcasts expire after one tick without refresh");
  }
  {  // Q) Launch-critical Trace guarantees, explicit (JD priority, 2026-10-12): colour isolation
     // in BOTH directions on parallel adjacent runs, exact 1-per-block decay at measured
     // distances, and the torch -> wire -> lamp round trip with source strength exactly 64.
    ChunkMap q;
    q.setRegistry(&blockRegistry());
    for (int cz = 0; cz < 2; ++cz)
      for (int cx = 0; cx < 4; ++cx) { auto col = std::make_unique<ChunkColumn>(); col->cx = cx; col->cz = cz; q.insert(std::move(col)); }
    const int qy = 10;
    for (int z = 0; z < 32; ++z)
      for (int x = 0; x < 64; ++x) q.set(x, qy - 1, z, kWaferSilicon);
    SignalSim qs;
    auto qput = [&](int x, int z, BlockId b) { q.set(x, qy, z, b); qs.onBlockChanged(x, qy, z, b); };
    auto qrun = [&](int n) { for (int i = 0; i < n; ++i) qs.tick(q, 0, 0, 100, [](int, int, int) {}); };
    auto qval = [&](int x, int z) { return stValue(q.getState(x, qy, z)); };
    // A1/A2: parallel adjacent runs, one colour driven at a time, plus a wire of the other
    // colour touching the driving torch itself.
    qput(0, 0, kRedTorch); qput(0, 1, kBlueTrace);
    for (int x = 1; x <= 10; ++x) { qput(x, 0, kRedTrace); qput(x, 1, kBlueTrace); }
    qput(0, 4, kRedTrace); qput(0, 5, kBlueTorch);
    for (int x = 1; x <= 10; ++x) { qput(x, 5, kBlueTrace); qput(x, 4, kRedTrace); }
    qrun(3);
    bool blueDark = qval(0, 1) == 0;
    for (int x = 1; x <= 10; ++x) blueDark &= qval(x, 1) == 0;
    check(qval(1, 0) == 64 && qval(10, 0) == 55 && blueDark,
          "TRACE-A1 isolation: red run at 64..55 beside blue run - every blue cell stays 0 (incl. beside the red torch)");
    bool redDark = qval(0, 4) == 0;
    for (int x = 1; x <= 10; ++x) redDark &= qval(x, 4) == 0;
    check(qval(1, 5) == 64 && qval(10, 5) == 55 && redDark,
          "TRACE-A2 isolation: blue run at 64..55 beside red run - every red cell stays 0 (incl. beside the blue torch)");
    // B: exact decay at measured distances on both colours (strength = 65 - distance in blocks).
    qput(0, 8, kRedTorch);
    for (int x = 1; x <= 40; ++x) qput(x, 8, kRedTrace);
    qput(0, 10, kBlueTorch);
    for (int x = 1; x <= 40; ++x) qput(x, 10, kBlueTrace);
    qrun(3);
    check(qval(1, 8) == 64 && qval(2, 8) == 63 && qval(5, 8) == 60 && qval(10, 8) == 55 && qval(33, 8) == 32 && qval(40, 8) == 25,
          "TRACE-B1 decay: red run is exactly 64@1, 63@2, 60@5, 55@10, 32@33, 25@40 (1 per block, no more, no less)");
    check(qval(1, 10) == 64 && qval(2, 10) == 63 && qval(5, 10) == 60 && qval(33, 10) == 32 && qval(40, 10) == 25,
          "TRACE-B2 decay: blue run decays identically (64@1, 63@2, 60@5, 32@33, 25@40)");
    // C: torch -> wire -> lamp round trip, both colours; source strength is exactly 64, never more.
    qput(0, 12, kRedTorch);
    for (int x = 1; x <= 6; ++x) qput(x, 12, kRedTrace);
    qput(7, 12, kLamp);
    qput(0, 14, kBlueTorch);
    for (int x = 1; x <= 6; ++x) qput(x, 14, kBlueTrace);
    qput(7, 14, kLamp);
    qrun(3);
    int maxRed = 0, maxBlue = 0;
    for (int x = 1; x <= 6; ++x) { maxRed = std::max(maxRed, qval(x, 12)); maxBlue = std::max(maxBlue, qval(x, 14)); }
    check(qval(7, 12) == 1 && qval(1, 12) == 64 && maxRed == 64,
          "TRACE-C1 round trip (red): torch -> 6 trace -> lamp lit; first trace 64 and no cell exceeds 64");
    check(qval(7, 14) == 1 && qval(1, 14) == 64 && maxBlue == 64,
          "TRACE-C2 round trip (blue): torch -> 6 trace -> lamp lit; first trace 64 and no cell exceeds 64");
    q.set(6, qy, 12, kAir); qs.onBlockChanged(6, qy, 12, kAir);  // break the last red trace
    qrun(2);
    check(qval(7, 12) == 0 && qval(7, 14) == 1,
          "TRACE-C3 round trip is live: breaking the red run's last trace turns its lamp off; the blue lamp stays lit");
  }
  return fails;
}

}  // namespace lithokraft
