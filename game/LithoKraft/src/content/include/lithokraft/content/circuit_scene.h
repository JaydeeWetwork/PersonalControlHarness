// Stress circuits for --bench-scene circuits and --perf-headless: a grid of small clocks
// (pulsors) driving wire meshes into lamps, plus long repeater chains fed by torches. Built on a
// silicon floor at height y; returns the number of blocks placed.
#pragma once
#include <string_view>
#include "lithokraft/engine.h"
#include "litholattice/world/chunk_map.h"

namespace lithokraft {
int buildCircuitScene(ChunkMap& world, int x0, int y, int z0, int size);
/// --bench-scene display: a ring-oscillator clock drives a column of Pulsors, each reading a
/// preloaded Memor (the instruction / data cache bank), so every clock edge fires the stored
/// strength down a wire into a row of 64-color lamps: an animated screen. size x size footprint;
/// (size - 8) / 4 banks of size - 6 lamps. Returns the number of blocks placed.
int buildDisplayScene(ChunkMap& world, int x0, int y, int z0, int size);

/// Where the framebuffer scene put things (for the flyover camera and the viewing stand).
struct FramebufferLayout { int x0 = 0, z0 = 0, y = 0, size = 0, screenZ0 = 0, screenZ1 = 0, cacheZ = 0, columns = 0, blocks = 0;
  bool vertical = false; int wallZ = 0, topY = 0, width = 0; };
/// --bench-scene framebuffer: a flat 64-color lamp screen (size x size footprint, 56-pixel
/// columns every 64 blocks) driven as a shift register: each 4-row band is a Memor + Pulsor
/// stage, every clock edge passes each band's colour to the band below. Each column's top is fed by
/// a frame cache of 8 Memors read in turn by 8 Pulsors, sequenced by a repeater-ring
/// controller (instruction-cache style), so the screen scrolls a looping colour animation.
/// A viewing stand sits south of the screen.
FramebufferLayout buildFramebufferScene(ChunkMap& world, int x0, int y, int z0, int size);
/// The same machine standing upright: lamps form a wall in the x/y plane at z = z0 + 8, the data
/// runs, links and clock spine climb the back face as one-block wire stairs, and the frame cache +
/// controller sit on the floor in front. Shifts upward. size <= 128 (spine fed from mid-height).
FramebufferLayout buildFramebufferWall(ChunkMap& world, int x0, int y, int z0, int size);

/// --bench-scene redirector: a red and a blue sawtooth wire (dipping to the Redirector row every
/// other block, climbing a step in between) with a split-mode Redirector every other block. Even
/// Redirectors route red east, odd ones blue east, each into its own 64-color lamp; no lamp touches
/// a wire. n Redirectors from x0 along +x, rows z0 (red), z0 + 1 (Redirectors / lamps), z0 + 2
/// (blue). Torches at x0 - 1 feed both lines. Returns blocks placed.
int buildRedirectorScene(ChunkMap& world, int x0, int y, int z0, int n);
/// Wall-pixel screen (decoder design): rows x width (even, <= 16) colour lamps standing in the
/// plane z = zw from y + 1 up, two blocks per row with glass between. Each pixel has a Catchor
/// (match i + 1) on its row's red line, so it shows the caught value as its colour. Each row has its
/// own driver shelf underground (lapping clock pulse on a blue repeater ring firing one Pulsor per
/// pixel with that pixel's Memor address); a Red Via carries the feed straight up to the row.
FramebufferLayout buildWallPixelScene(ChunkMap& world, int x0, int y, int zw, int width, int rows);

/// Static pattern colour (palette index 1-64) of screen pixel (i, r), r = 0 the bottom row.
/// "checkers" (8 x 8 squares, 2 colours) or "circles" (concentric rings, <= 16 colours).
int patternColor(std::string_view pattern, int size, int i, int r);
/// Pattern wall: size x size (even, 16-64) lamps standing in z = zw, rows two blocks apart with glass
/// between. Every Catchor's match value is its pixel's pattern colour; one shared driver per wire
/// colour sweeps the (<= 16) colours present; a Red and a Blue Via spine climb the back with a wire
/// tap at each row (even rows red, odd rows blue), so every pixel catches its own colour once a sweep.
/// Sawtooth wall (JD's compact layout): rows of colour lamps in the plane z = z0, one row per layer
/// (two blocks apart, glass between). Every pixel has its own straight wire running back to its own
/// Memor (front = steady value); neighbouring wires alternate red / blue (and flip per layer),
/// so wires packed side by side never touch. Red runs end in a Redirector into the lamp, blue runs
/// reach the lamp directly. kSawtoothRun = wire length; the image is the pattern's colours.
/// Wire torture: one fixture per wire rule (stairs over silicon / glass / frosted glass / ceramic /
/// slab, cut stairs, ceiling wire, via tower, red / blue side by side, Repeater hard power into
/// silicon vs glass / ceramic, a Catchor -> Redirector chain on an oscillating line) plus a
/// 24 x 48 stair field whose every wire flips each tick (torch-inverter oscillator per row).
/// Layout: fixtures at z0 .. z0 + 27, field rows at kTortureField + 2 r.
constexpr int kTortureField = 32;
FramebufferLayout buildWireTorture(ChunkMap& world, int x0, int y, int z0);
constexpr int kSawtoothRun = 8;
FramebufferLayout buildSawtoothWall(ChunkMap& world, int x0, int y, int z0, int width, int rows, std::string_view pattern);
FramebufferLayout buildPatternWall(ChunkMap& world, int x0, int y, int zw, int size, std::string_view pattern);
}  // namespace lithokraft
