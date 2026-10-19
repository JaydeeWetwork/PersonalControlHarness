#include "lithokraft/game/structures.h"
#include <algorithm>
#include "lithokraft/content/blocks.h"

namespace lithokraft {

Spawn buildDemoCircuit(ChunkMap& w, int ox, int oz) {
  // Flat silicon platform 50 x 33 at level P.
  int P = 0;
  for (int z = -2; z < 31; ++z)
    for (int x = -2; x < 48; ++x) P = std::max(P, w.surfaceY(ox + x, oz + z));
  P += 1;
  for (int z = -2; z < 31; ++z)
    for (int x = -2; x < 48; ++x)
      for (int y = w.surfaceY(ox + x, oz + z) + 1; y <= P; ++y) w.set(ox + x, y, oz + z, kWaferSilicon);
  const int y = P + 1;
  auto put = [&](int x, int yy, int z, BlockId b, CellState s = 0) {
    w.set(ox + x, yy, oz + z, b);
    if (s) w.setState(ox + x, yy, oz + z, s);
  };
  constexpr int N = 0, E = 1, S = 2;

  // Row z=0: Red Torch -> 10 red trace -> Lamp (lit). Row z=1: unpowered blue trace right beside
  // the red run: it stays dark because red and blue never connect.
  put(0, y, 0, kRedTorch);
  for (int x = 1; x <= 10; ++x) { put(x, y, 0, kRedTrace); put(x, y, 1, kBlueTrace); }
  put(11, y, 0, kLamp);

  // Row z=4: inverter. Source torch -> red trace run 1 (with a GAP at x=3) -> silicon block B.
  // Torch T1 sits on B; it is lit while B is unpowered and drives run 2 -> Lamp 2.
  // Place red trace into the gap: B becomes powered, T1 turns off, Lamp 2 goes dark.
  put(0, y, 4, kRedTorch);
  for (int x = 1; x <= 5; ++x) if (x != 3) put(x, y, 4, kRedTrace);
  put(6, y, 4, kWaferSilicon);          // block B
  put(6, y + 1, 4, kRedTorch);          // T1 (inverter)
  for (int x = 7; x <= 13; ++x) put(x, y, 4, kWaferSilicon);  // ledge
  for (int x = 7; x <= 12; ++x) put(x, y + 1, 4, kRedTrace);
  put(13, y + 1, 4, kLamp);

  // Row z=8: Blue Torch -> blue trace staircase (climbs one block) -> Lamp.
  put(0, y, 8, kBlueTorch);
  for (int x = 1; x <= 5; ++x) put(x, y, 8, kBlueTrace);
  for (int x = 6; x <= 11; ++x) { put(x, y, 8, kWaferSilicon); put(x, y + 1, 8, kBlueTrace); }
  put(12, y + 1, 8, kLamp);

  // Row z=10: torch powers the block above it (Minecraft-style). Red Torch -> silicon block on top
  // -> that block emits colourless power into BLUE wire on the ledge -> Lamp.
  put(0, y, 10, kRedTorch);
  put(0, y + 1, 10, kWaferSilicon);
  for (int x = 1; x <= 4; ++x) put(x, y, 10, kWaferSilicon);
  for (int x = 1; x <= 3; ++x) put(x, y + 1, 10, kBlueTrace);
  put(4, y + 1, 10, kLamp);

  // Row z=13: Repeater (delay 4) re-boosts a red run to 64 into blue trace -> Lamp.
  put(0, y, 13, kRedTorch);
  for (int x = 1; x <= 3; ++x) put(x, y, 13, kRedTrace);
  put(4, y, 13, kRepeater, makeState(0, E, 3));
  for (int x = 5; x <= 9; ++x) put(x, y, 13, kBlueTrace);
  put(10, y, 13, kLamp);

  // Row z=17: window comparator. Switch (on) -> 19 red trace (46 at the end) -> Thresholdor.
  // Memory cells preloaded with 40 (LOW, left) and 50 (HIGH, right) face into its sides.
  // 40 <= 46 <= 50, so the output (46) lights the lamp. Toggle the switch to see it go dark.
  put(1, y, 17, kSwitch, makeState(0, 0, 0, true));
  for (int x = 2; x <= 20; ++x) put(x, y, 17, kRedTrace);
  put(21, y, 17, kThresholdor, makeState(0, E));
  put(21, y, 16, kMemor, makeState(40, S, 0, true));   // LOW ref
  put(21, y, 18, kMemor, makeState(50, N, 0, true));   // HIGH ref
  for (int x = 22; x <= 24; ++x) put(x, y, 17, kRedTrace);
  put(25, y, 17, kLamp);

  // Row z=21: Pulsor -> 24 wires (no decay) -> Memor -> Color Lamp 64. Data = a memory
  // cell preloaded with 23 behind the Pulsor; press the Button (clock, Pulsor's left) to fire.
  put(5, y, 21, kMemor, makeState(23, E, 0, true));
  put(6, y, 21, kPulsor, makeState(0, E));
  put(6, y, 20, kButton);
  for (int x = 7; x <= 30; ++x) put(x, y, 21, kRedTrace);
  put(31, y, 21, kMemor, makeState(0, E));
  put(32, y, 21, kRedTrace);
  put(33, y, 21, kColorLamp64);

  // Rows z=23..25: lamp wall, 16 x 2 Color Lamp 16. Bottom row fed by a red run (64..49), top row
  // by a blue run on a ledge behind (56..41): WRAP mapping gives every lamp a different colour.
  put(9, y, 23, kRedTorch);
  for (int x = 10; x <= 25; ++x) {
    put(x, y, 23, kRedTrace);
    put(x, y, 24, kColorLamp16); put(x, y + 1, 24, kColorLamp16);
  }
  for (int x = 1; x <= 25; ++x) put(x, y, 25, kWaferSilicon);
  put(1, y + 1, 25, kBlueTorch);
  for (int x = 2; x <= 25; ++x) put(x, y + 1, 25, kBlueTrace);

  // Row z=28: Color Lamp 32 + 64, slabs, Keyboard, Fast Latch, Redirector.
  put(0, y, 28, kSwitch, makeState(0, 0, 0, true));
  for (int x = 1; x <= 3; ++x) put(x, y, 28, kRedTrace);
  put(4, y, 28, kColorLamp32);
  put(6, y, 28, kSiliconSlab); put(7, y, 28, kStoneSlab); put(8, y, 28, kDirtSlab);
  // Keyboard (right-click to type): chars -> wire -> Memor -> Color Lamp 64; Enter blinks the lamp.
  put(12, y, 28, kKeyboard, makeState(0, E));
  for (int x = 13; x <= 15; ++x) put(x, y, 28, kRedTrace);
  put(16, y, 28, kMemor, makeState(0, E));
  put(17, y, 28, kRedTrace);
  put(18, y, 28, kColorLamp64);
  put(12, y, 29, kBlueTrace); put(12, y, 30, kLamp);              // control port (right = south)
  // Fast Latch: press the Button -> toggles -> Lamp.
  put(22, y, 28, kButton);
  put(23, y, 28, kFastLatch, makeState(0, E));
  put(24, y, 28, kRedTrace); put(25, y, 28, kLamp);
  // Redirector: a Switch on its left side, output out the front -> Lamp.
  put(30, y, 27, kSwitch, makeState(0, 0, 0, true));
  put(30, y, 28, kRedirector, makeState(0, E));

  // Row z=30: Tone Block. Switch (off) -> 28 red traces (64..37) -> Tone Block plays s=37 = C5.
  put(14, y, 30, kSwitch);
  for (int x = 15; x <= 42; ++x) put(x, y, 30, kRedTrace);
  put(43, y, 30, kToneBlock, makeState(0, E));
  put(31, y, 28, kRedTrace); put(32, y, 28, kLamp);

  // Spawn (P2b-2, review G-M1): feet ON the platform top (P+1 — the platform
  // is filled up to P), not 11 blocks above it. The old point also sat at
  // z+40, OUTSIDE the platform (z rel -2..30), so the player fell to the
  // natural terrain instead. z rel 2 is a clear row (no circuit occupies
  // it), and looking -Z from there faces the circuit rows at z rel 0..1
  // and beyond. `kill` respawn inherits this point via spawnPoint_.
  return {float(ox) + 18.f, float(P) + 1.f, float(oz) + 2.f, -1.5708f, -0.42f};  // looking -Z over the rows
}

}  // namespace lithokraft
