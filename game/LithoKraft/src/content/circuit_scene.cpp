#include "lithokraft/content/circuit_scene.h"
#include <string_view>
#include <vector>
#include <cmath>
#include <algorithm>
#include "lithokraft/content/blocks.h"

namespace lithokraft {

int buildCircuitScene(ChunkMap& w, int x0, int y, int z0, int size) {
  int placed = 0;
  auto put = [&](int x, int yy, int z, BlockId b, int facing = 0) {
    if (w.set(x, yy, z, b)) { if (facing) w.setState(x, yy, z, makeState(0, facing)); ++placed; }
  };
  for (int z = z0; z < z0 + size; ++z)
    for (int x = x0; x < x0 + size; ++x) put(x, y - 1, z, kWaferSilicon);
  // Clock grid: every 8 x 8 cell holds a pulsor (east-facing) feeding a 5-long red trace into a
  // lamp, and a blue trace cross-mesh between the cells.
  for (int cz = z0 + 1; cz + 7 < z0 + size / 2; cz += 8)
    for (int cx = x0 + 1; cx + 7 < x0 + size; cx += 8) {
      put(cx, y, cz, kPulsor, 1);
      for (int i = 1; i <= 5; ++i) put(cx + i, y, cz, kRedTrace);
      put(cx + 6, y, cz, kLamp);
      for (int i = 1; i <= 6; ++i) put(cx + i, y, cz + 3, kBlueTrace);
      for (int i = 1; i <= 6; ++i) if (i != 3) put(cx + 3, y, cz + i, kBlueTrace);
      put(cx, y, cz + 3, kBlueTorch);
    }
  // Repeater chains: a red torch then alternating repeater / wire runs eastward, one per row.
  for (int z = z0 + size / 2 + 1; z < z0 + size - 1; z += 2) {
    put(x0 + 1, y, z, kRedTorch);
    for (int x = x0 + 2; x < x0 + size - 2; ++x) put(x, y, z, (x - x0) % 4 == 0 ? kRepeater : kRedTrace, 1);
    put(x0 + size - 2, y, z, kLamp);
  }
  return placed;
}

int buildDisplayScene(ChunkMap& w, int x0, int y, int z0, int size) {
  constexpr int N = 0, E = 1, S = 2, W = 3;
  int placed = 0;
  auto put = [&](int x, int yy, int z, BlockId b, int facing = 0, int value = 0, int aux = 0) {
    if (w.set(x, yy, z, b)) { if (facing || value || aux) w.setState(x, yy, z, makeState(value, facing, aux)); ++placed; }
  };
  for (int z = z0; z < z0 + size; ++z)
    for (int x = x0; x < x0 + size; ++x) put(x, y - 1, z, kWaferSilicon);
  // Clock: a 3 x 3 ring of four delay-8 repeaters (edges) joined by single blue traces (corners).
  // A kick Pulsor (red torch data, blue torch clock) fires one pulse into the ring on its first
  // tick; the pulse then laps forever, giving a rising edge every ~32 ticks.
  const int ox = x0 + 4, oz = z0 + 1;
  put(ox - 3, y, oz, kRedTorch); put(ox - 2, y, oz, kRedTrace); put(ox - 1, y, oz, kPulsor, E);
  put(ox - 1, y, oz + 1, kBlueTrace); put(ox - 2, y, oz + 1, kBlueTorch);
  put(ox, y, oz, kBlueTrace);     put(ox + 1, y, oz, kRepeater, E, 0, 7);
  put(ox + 2, y, oz, kBlueTrace); put(ox + 2, y, oz + 1, kRepeater, S, 0, 7);
  put(ox + 2, y, oz + 2, kBlueTrace); put(ox + 1, y, oz + 2, kRepeater, W, 0, 7);
  put(ox, y, oz + 2, kBlueTrace); put(ox, y, oz + 1, kRepeater, N, 0, 7);
  // Clock spine (blue) runs south from the ring past every bank's Pulsor.
  const int px = ox + 2, banks = std::max(1, (size - 8) / 4), width = std::max(4, size - 6 - (px - x0));
  const int zEnd = oz + 5 + 4 * (banks - 1);
  for (int z = oz + 3; z <= zEnd; ++z) put(px, y, z, kBlueTrace);
  for (int i = 0; i < banks; ++i) {
    const int z = oz + 5 + 4 * i;
    put(px + 2, y, z - 1, kMemor, W, 1 + (i * 7) % 63);  // cache line: stored strength
    put(px + 1, y, z - 1, kRedTrace);
    put(px + 1, y, z, kPulsor, S);                          // side (west) = clock spine
    for (int x = px + 1; x <= px + width; ++x) put(x, y, z + 1, kRedTrace);
    for (int x = px + 2; x <= px + width; ++x) put(x, y, z + 2, kColorLamp64);
  }
  return placed;
}
FramebufferLayout buildFramebufferScene(ChunkMap& w, int x0, int y, int z0, int size) {
  constexpr int N = 0, E = 1, S = 2, W = 3;
  constexpr int kFrames = 8, kPixels = 56, kPitch = 64;
  FramebufferLayout L;
  int placed = 0;
  auto put = [&](int x, int yy, int z, BlockId b, int facing = 0, int value = 0, int aux = 0) {
    if (w.set(x, yy, z, b)) { if (facing || value || aux) w.setState(x, yy, z, makeState(value, facing, aux)); ++placed; }
  };
  const int zs = z0 + 12;                          // first screen stage's pulsor row
  const int stages = std::max(2, (size - 16) / 4);
  const int zLast = zs + 4 * (stages - 1), zb = zLast + 3;  // bottom spine row
  const int columns = std::max(1, size / kPitch);
  for (int z = z0; z <= zb + 20; ++z)
    for (int x = x0; x < x0 + std::max(size, kPitch * columns); ++x) {
      for (int yy = y; yy <= y + 3; ++yy) w.set(x, yy, z, kAir);  // clear terrain bumps
      put(x, y - 1, z, kWaferSilicon);
    }
  for (int j = 0; j < columns; ++j) {
    const int px = x0 + 4 + kPitch * j;  // clock spine column
    // ---- screen: shift-register stages (Memor -> Pulsor -> 3-row red data run, lamps on top)
    for (int i = 0; i < stages; ++i) {
      const int z = zs + 4 * i;
      put(px + 2, y, z - 1, kMemor, W, 1 + ((i * 5 + j * 11) % 63));  // initial picture
      put(px + 1, y, z - 1, kRedTrace);
      put(px + 1, y, z, kPulsor, S);
      auto data = [&](int x, int zz) { put(x, y, zz, kRedTrace); put(x, y + 1, zz, kColorLamp64); };
      for (int x = px + 1; x <= px + 4 + kPixels; ++x) data(x, z + 1);
      for (int x = px + 4; x <= px + 4 + kPixels; ++x) data(x, z);
      for (int x = px + 5; x <= px + 4 + kPixels; ++x) data(x, z - 1);
      if (i + 1 < stages) { put(px + 3, y, z + 2, kRedTrace); put(px + 3, y, z + 3, kRedTrace); }  // write the next stage
    }
    // Spine (blue), driven from the bottom up so lower stages fire no later than upper ones.
    for (int z = zs - 1; z <= zb; ++z) {
      const bool rep = z > zs && z < zb - 2 && ((z - zs - 2) % 48) == 0;  // gap rows only
      if (rep) put(px, y, z, kRepeater, N, 0, 0); else put(px, y, z, kBlueTrace);
    }
    // ---- frame cache + controller (north of the screen)
    const int cx = px + 6, zc = zs - 6, zr = zc - 3;
    for (int k = 0; k < kFrames; ++k) {
      const int x = cx + 4 * k;
      put(x, y, zc - 1, kMemor, S, 1 + ((k * 8 + j * 3) % 64 == 0 ? 1 : (k * 8 + j * 3) % 64));  // frame k colour
      put(x, y, zc, kPulsor, S);
      put(x - 1, y, zc, kBlueTrace);                                   // clock tap -> west side
      for (int z = zr + 1; z <= zc; ++z) put(x - 2, y, z, kBlueTrace);
      put(x - 2, y, zr, kBlueTrace);                                   // ring node k
      for (int d = 1; d <= 3; ++d) put(x - 2 + d, y, zr, kRepeater, E, 0, 3);
    }
    const int xEnd = cx + 4 * kFrames - 2;  // final node: return path back to node 0
    put(xEnd, y, zr, kBlueTrace); put(xEnd, y, zr - 1, kBlueTrace);
    for (int x = cx - 2; x <= xEnd; ++x) put(x, y, zr - 2, kBlueTrace);
    put(cx - 2, y, zr - 1, kBlueTrace);
    // Kick pulsor: fires once on the first tick and starts the pulse lapping the ring.
    put(cx - 3, y, zr, kPulsor, E); put(cx - 4, y, zr, kRedTrace); put(cx - 5, y, zr, kRedTorch);
    put(cx - 3, y, zr + 1, kRedTrace); put(cx - 3, y, zr + 2, kRedTorch);
    // Cache bus (red): every cache pulsor fires into it; it writes stage 0 and triggers the spine.
    for (int x = px + 3; x <= cx + 4 * (kFrames - 1); ++x) put(x, y, zc + 1, kRedTrace);
    for (int z = zc + 2; z <= zs - 1; ++z) put(px + 3, y, z, kRedTrace);
    put(px + 2, y, zc + 1, kRepeater, W, 0, 0);                       // bus pulse -> spine trigger
    for (int x = px - 2; x <= px + 1; ++x) put(x, y, zc + 1, kBlueTrace);
    for (int z = zc + 2; z <= zb; ++z) {
      const bool rep = ((z - zc - 2) % 48) == 47;
      if (rep) put(px - 2, y, z, kRepeater, S, 0, 0); else put(px - 2, y, z, kBlueTrace);
    }
    put(px - 1, y, zb, kBlueTrace);
  }
  // Viewing stand: a 9 x 9 platform 14 blocks up, south of the screen, centred.
  const int sx = x0 + std::max(size, kPitch * columns) / 2, sz = zb + 12;
  for (int z = sz - 4; z <= sz + 4; ++z)
    for (int x = sx - 4; x <= sx + 4; ++x) put(x, y + 14, z, kWaferSilicon);
  L = {x0, z0, y, size, zs - 1, zb, zs - 9, columns, placed};
  return L;
}
FramebufferLayout buildFramebufferWall(ChunkMap& w, int x0, int y0, int z0, int size) {
  constexpr int N = 0, E = 1, S = 2, W = 3;
  constexpr int kFrames = 8, kPixels = 52, kPitch = 64;
  size = std::clamp(size, 32, 128);
  int placed = 0;
  auto put = [&](int x, int yy, int z, BlockId b, int facing = 0, int value = 0, int aux = 0) {
    if (w.set(x, yy, z, b)) { if (facing || value || aux) w.setState(x, yy, z, makeState(value, facing, aux)); ++placed; }
  };
  const int zw = z0 + 8, stages = std::max(2, (size - 4) / 4), columns = std::max(1, size / kPitch);
  const int hb0 = y0 + 3, top = hb0 + 4 * stages;
  const int width = kPitch * columns;
  for (int z = z0; z <= zw + 8; ++z)
    for (int x = x0; x < x0 + width + 4; ++x) {
      for (int yy = y0; yy <= top + 4; ++yy) w.set(x, yy, z, kAir);  // terrain would cut the wire stairs
      put(x, y0 - 1, z, kWaferSilicon);
    }
  for (int j = 0; j < columns; ++j) {
    const int sx = x0 + 10 + kPitch * j;  // pulsor column (back face, z = zw + 1)
    const int zb = zw + 1, zs = zw + 2;   // data / link plane, spine / trigger plane
    for (int i = 0; i < stages; ++i) {
      const int hb = hb0 + 4 * i, hp = hb + 1;
      put(sx - 1, hp, zb, kMemor, E, 1 + ((i * 5 + j * 11) % 63));
      put(sx, hp, zb, kPulsor, E);
      auto data = [&](int x, int yy) { put(x, yy, zb, kRedTrace); put(x, yy, zw, kColorLamp64); };
      for (int x = sx + 3; x <= sx + kPixels; ++x) data(x, hb);
      for (int x = sx + 1; x <= sx + kPixels; ++x) data(x, hb + 1);
      for (int x = sx + 2; x <= sx + kPixels; ++x) data(x, hb + 2);
      if (i + 1 < stages) {  // link up-left into the next stage's memory cell back (sx - 2, hb + 5)
        for (int x = sx - 2; x <= sx + 1; ++x) put(x, hb + 3, zb, kRedTrace);
        put(sx - 3, hb + 4, zb, kRedTrace);
        put(sx - 2, hb + 5, zb, kRedTrace);
      }
    }
    // Clock spine: blue zigzag stairs on z = zw + 2, at x = sx on pulsor rows, sx - 1 between.
    const int spineLo = hb0 + 1, spineHi = hb0 + 4 * (stages - 1) + 1;
    for (int yy = spineLo; yy <= spineHi; ++yy) put(((yy - spineLo) & 1) ? sx - 1 : sx, yy, zs, kBlueTrace);
    const int k = (stages - 1) / 2, ym = hb0 + 4 * k + 2;  // feed point (spine at sx - 1 there)
    put(sx - 2, ym, zs, kRepeater, E, 0, 0);
    for (int yy = y0; yy <= ym; ++yy) put(((ym - yy) & 1) ? sx - 4 : sx - 3, yy, zs, kBlueTrace);  // trigger stairs
    // Frame cache + controller on the floor in front of the wall (same machine as the flat scene).
    const int cx = sx + 4, zc = zw, zr = zc - 3;
    for (int f = 0; f < kFrames; ++f) {
      const int x = cx + 4 * f, v = (f * 8 + j * 3) % 64;
      put(x, y0, zc - 1, kMemor, S, v == 0 ? 1 : v);
      put(x, y0, zc, kPulsor, S);
      put(x - 1, y0, zc, kBlueTrace);
      for (int z = zr + 1; z <= zc; ++z) put(x - 2, y0, z, kBlueTrace);
      put(x - 2, y0, zr, kBlueTrace);
      for (int d = 1; d <= 3; ++d) put(x - 2 + d, y0, zr, kRepeater, E, 0, 3);
    }
    const int xEnd = cx + 4 * kFrames - 2;
    put(xEnd, y0, zr, kBlueTrace); put(xEnd, y0, zr - 1, kBlueTrace);
    for (int x = cx - 2; x <= xEnd; ++x) put(x, y0, zr - 2, kBlueTrace);
    put(cx - 2, y0, zr - 1, kBlueTrace);
    put(cx - 3, y0, zr, kPulsor, E); put(cx - 4, y0, zr, kRedTrace); put(cx - 5, y0, zr, kRedTorch);
    put(cx - 3, y0, zr + 1, kRedTrace); put(cx - 3, y0, zr + 2, kRedTorch);
    // Bus (red, z = zw + 1 on the floor): climbs the back to stage 0's memory cell; the west end
    // drives the trigger stairs through a repeater.
    for (int x = sx - 6; x <= cx + 4 * (kFrames - 1); ++x) put(x, y0, zb, kRedTrace);
    put(sx - 5, y0 + 1, zb, kRedTrace); put(sx - 4, y0 + 2, zb, kRedTrace); put(sx - 3, y0 + 3, zb, kRedTrace);
    put(sx - 2, y0 + 4, zb, kRedTrace);
    put(sx - 7, y0, zb, kRepeater, W, 0, 0);
    put(sx - 8, y0, zb, kBlueTrace);
    for (int x = sx - 8; x <= sx - 5; ++x) put(x, y0, zs, kBlueTrace);
  }
  // Viewing stand in front of the wall, at mid height.
  const int sxm = x0 + width / 2, szm = zw - 34, sy = (hb0 + top) / 2;
  for (int z = szm - 4; z <= szm + 4; ++z)
    for (int x = sxm - 4; x <= sxm + 4; ++x) put(x, sy, z, kWaferSilicon);
  FramebufferLayout L;
  L.x0 = x0; L.z0 = z0; L.y = y0; L.size = size; L.screenZ0 = zw; L.screenZ1 = zw + 2; L.cacheZ = zw - 3;
  L.columns = columns; L.blocks = placed; L.vertical = true; L.wallZ = zw; L.topY = top; L.width = width;
  return L;
}
int buildRedirectorScene(ChunkMap& w, int x0, int y, int z0, int n) {
  constexpr int N = 0, E = 1, S = 2;
  int placed = 0;
  auto put = [&](int x, int yy, int z, BlockId b, CellState st = 0) {
    if (w.set(x, yy, z, b)) { if (st) w.setState(x, yy, z, st); ++placed; }
  };
  for (int z = z0 - 2; z <= z0 + 4; ++z)
    for (int x = x0 - 3; x <= x0 + 2 * n + 2; ++x) {
      for (int yy = y; yy <= y + 2; ++yy) w.set(x, yy, z, kAir);
      put(x, y - 1, z, kWaferSilicon);
    }
  put(x0 - 1, y, z0, kRedTorch); put(x0 - 1, y, z0 + 2, kBlueTorch);
  for (int o = 0; o < 2 * n; ++o) {
    const int x = x0 + o, yy = (o & 1) ? y + 1 : y;  // sawtooth: down at Redirectors, up between
    if (o & 1) { put(x, y, z0, kGlass); put(x, y, z0 + 2, kGlass); }  // raised wire rests on glass
    put(x, yy, z0, kRedTrace); put(x, yy, z0 + 2, kBlueTrace);
    if (o & 1) { put(x, y, z0 + 1, kColorLamp64); continue; }
    const bool redPixel = ((o / 2) & 1) == 0;
    // One lane per pixel; the other is off (the output is colourless, so a parked lane would leak).
    put(x, y, z0 + 1, kRedirector, redPixel ? CellState(makeState(0, E, 4 | N) | (1u << 14)) : CellState(makeState(0, S, 4 | E) | (1u << 13)));
  }
  return placed;
}
FramebufferLayout buildWallPixelScene(ChunkMap& w, int x0, int yf, int zw, int width, int rows) {
  constexpr int N = 0, E = 1, S = 2, W = 3;
  const int n = std::clamp(width / 2, 1, 8), Wd = 2 * n;  // pixels per ring side
  rows = std::clamp(rows, 1, 16);
  const int B = zw + 4, Rn = zw, Rs = zw + 8;              // driver rows (underground, under the wall)
  const int X = x0 + n - 15;                               // first driver tap: drivers centred under the screen
  const int lineA = X + 1, lineB = std::max(x0 + Wd, X + 4 * (std::min(rows, 8) - 1) + 1);
  const int yTop = yf + 1 + 2 * (rows - 1), yLow = yf - 3 - 2 * (rows - 1) - 2;
  int placed = 0;
  auto put = [&](int x, int yy, int z, BlockId b, int facing = 0, int value = 0, int aux = 0) {
    if (w.set(x, yy, z, b)) { if (facing || value || aux) w.setState(x, yy, z, makeState(value, facing, aux)); ++placed; }
  };
  const int xa = std::min(X - 7, x0 - 2), xb = std::max(X + 4 * n + 2, lineB + 2);
  for (int z = zw - 40; z <= Rs + 2; ++z)
    for (int x = xa; x <= xb; ++x) {
      for (int yy = (z < zw ? yf : yLow); yy <= yTop + 4; ++yy) w.set(x, yy, z, kAir);
      put(x, yf - 1, z, z < zw - 1 ? kWaferSilicon : kGlass);  // glass floor: the drivers show through
    }
  for (int r = 0; r < rows; ++r) {
    const int Y = yf + 1 + 2 * r, H = yf - 3 - 2 * r;
    // Row r: lamp | Catchor (match i+1) | shared red line, on glass; glass separates the rows.
    for (int x = lineA; x <= lineB; ++x)
      for (int z = zw; z <= zw + 2; ++z) { put(x, Y - 1, z, kGlass); put(x, Y + 1, z, kGlass); }
    for (int i = 0; i < Wd; ++i) { put(x0 + i, Y, zw, kColorLamp64); put(x0 + i, Y, zw + 1, kCatchor, N, i + 1); }
    for (int x = lineA; x <= lineB; ++x) put(x, Y, zw + 2, kRedTrace);
    // Driver r: its own shelf, bus, clock ring, Pulsors and Memor addresses (as one row).
    for (int z = Rn; z <= Rs; ++z) for (int x = X - 6; x <= X + 4 * n + 1; ++x) put(x, H - 1, z, kGlass);
    for (int x = X - 1; x <= X + 4 * (n - 1) + 1; ++x) put(x, H, B, kRedTrace);  // bus (reaches every via stub)
    for (int k = 0; k < n; ++k) {
      const int px = X + 4 * k;
      put(px, H, B - 3, kPulsor, W); put(px + 1, H, B - 3, kMemor, W, k + 1);
      put(px - 1, H, B - 3, kRedTrace); put(px - 1, H, B - 2, kRedTrace); put(px - 1, H, B - 1, kRedTrace);
      put(px, H, Rn, kBlueTrace); put(px + 1, H, Rn, kRepeater, E, 0, 7); put(px + 2, H, Rn, kBlueTrace); put(px + 3, H, Rn, kBlueTrace);
      put(px, H, B + 3, kPulsor, W); put(px + 1, H, B + 3, kMemor, W, 2 * n - k);
      put(px - 1, H, B + 3, kRedTrace); put(px - 1, H, B + 2, kRedTrace); put(px - 1, H, B + 1, kRedTrace);
      put(px, H, Rs, kBlueTrace); put(px + 1, H, Rs, kRepeater, W, 0, 7); put(px + 2, H, Rs, kBlueTrace); put(px + 3, H, Rs, kBlueTrace);
    }
    put(X - 2, H, Rn, kBlueTrace); put(X - 1, H, Rn, kBlueTrace);
    put(X - 2, H, Rs, kBlueTrace); put(X - 1, H, Rs, kBlueTrace);
    for (int z = Rn; z <= Rs; ++z) {
      put(X - 3, H, z, z == B ? kRepeater : kBlueTrace, z == B ? N : 0, 0, z == B ? 7 : 0);
      put(X + 4 * n, H, z, z == B ? kRepeater : kBlueTrace, z == B ? S : 0, 0, z == B ? 7 : 0);
    }
    put(X - 4, H, Rn + 1, kPulsor, E); put(X - 5, H, Rn + 1, kRedTrace);
    put(X - 4, H, Rn + 2, kRedTrace); put(X - 5, H, Rn + 2, kRedTorch);
  }
  // Feeds: a stub off each bus, a Red Via straight up through the shallower drivers' empty cells
  // to its row, then onto the line (rows 8+ use the second via plane and a two-block connector).
  for (int r = 0; r < rows; ++r) {
    const int Y = yf + 1 + 2 * r, H = yf - 3 - 2 * r, xv = X + 4 * (r % 8) + 1, zv = r < 8 ? B - 1 : B + 1;
    put(xv, H, zv, kRedTrace);
    for (int yy = H + 1; yy < Y; ++yy) put(xv, yy, zv, kRedVia);
    put(xv, Y, zv, kRedTrace);
    if (r >= 8) {
      put(xv, Y - 1, zw + 4, kGlass); put(xv, Y, zw + 4, kRedTrace);
      put(xv, Y - 1, zw + 3, kGlass); put(xv, Y, zw + 3, kRedTrace);
    }
  }
  FramebufferLayout L;
  L.x0 = x0; L.y = yf; L.z0 = zw; L.size = Wd; L.width = Wd; L.vertical = true; L.wallZ = zw; L.topY = yTop;
  L.columns = 1; L.blocks = placed; L.screenZ0 = zw; L.screenZ1 = zw; L.cacheZ = B;
  return L;
}
int patternColor(std::string_view pattern, int size, int i, int r) {
  if (pattern == "circles") {
    const double c = (size - 1) / 2.0, d = std::sqrt((i - c) * (i - c) + (r - c) * (r - c));
    const int ring = std::min(15, int(d / std::max(1.0, size / 16.0)));
    static constexpr int kRing[16] = {60, 52, 44, 36, 28, 20, 12, 4, 58, 50, 42, 34, 26, 18, 10, 2};
    return kRing[ring];
  }
  return ((i / 8) + (r / 8)) & 1 ? 9 : 57;  // checkers
}

FramebufferLayout buildPatternWall(ChunkMap& w, int x0, int yf, int zw, int size, std::string_view pattern) {
  constexpr int N = 0, E = 1, S = 2, W = 3, n = 8;
  size = std::clamp(size & ~1, 16, 64);
  const int rows = size;
  const int B = zw + 4, Rn = zw, Rs = zw + 8, zs = zw + 3;
  const int X = x0 + size / 2 - 16;                 // driver centred: bus <= 15 from the spines
  const int xr = X + 13, xbl = X + 14;              // Red / Blue Via spines (clear of the Pulsor legs)
  const int Hr = yf - 3, Hb = yf - 5;               // red driver, blue driver below it
  const int yTop = yf + 1 + 2 * (rows - 1);
  int placed = 0;
  auto put = [&](int x, int yy, int z, BlockId b, int facing = 0, int value = 0, int aux = 0) {
    if (w.set(x, yy, z, b)) { if (facing || value || aux) w.setState(x, yy, z, makeState(value, facing, aux)); ++placed; }
  };
  // The colours present (<= 16), repeated to fill the 16 driver slots.
  std::vector<int> cols;
  for (int r = 0; r < rows; ++r) for (int i = 0; i < size; ++i) {
    const int c = patternColor(pattern, size, i, r);
    if (std::find(cols.begin(), cols.end(), c) == cols.end()) cols.push_back(c);
  }
  std::sort(cols.begin(), cols.end());
  const int xa = std::min(X - 7, x0 - 2), xb = std::max(X + 4 * n + 2, x0 + size + 1);
  for (int z = zw - 40; z <= Rs + 2; ++z)
    for (int x = xa; x <= xb; ++x) {
      for (int yy = (z < zw ? yf : Hb - 1); yy <= yTop + 4; ++yy) w.set(x, yy, z, kAir);
      put(x, yf - 1, z, z < zw - 1 ? kWaferSilicon : kGlass);
    }
  // A driver: lapping clock pulse on a repeater ring fires 16 Pulsors, each reading one colour from
  // its Memor onto the bus (the ring is the other wire colour, so the two never touch).
  auto driver = [&](int H, BlockId bus, BlockId ring, BlockId torch) {
    for (int z = Rn; z <= Rs; ++z) for (int x = X - 6; x <= X + 4 * n + 1; ++x) put(x, H - 1, z, kGlass);
    for (int x = X - 1; x <= X + 4 * (n - 1) + 1; ++x) put(x, H, B, bus);
    for (int k = 0; k < n; ++k) {
      const int px = X + 4 * k;
      put(px, H, B - 3, kPulsor, W); put(px + 1, H, B - 3, kMemor, W, cols[size_t(k) % cols.size()]);
      put(px - 1, H, B - 3, bus); put(px - 1, H, B - 2, bus); put(px - 1, H, B - 1, bus);
      put(px, H, Rn, ring); put(px + 1, H, Rn, kRepeater, E, 0, 7); put(px + 2, H, Rn, ring); put(px + 3, H, Rn, ring);
      put(px, H, B + 3, kPulsor, W); put(px + 1, H, B + 3, kMemor, W, cols[size_t(n + k) % cols.size()]);
      put(px - 1, H, B + 3, bus); put(px - 1, H, B + 2, bus); put(px - 1, H, B + 1, bus);
      put(px, H, Rs, ring); put(px + 1, H, Rs, kRepeater, W, 0, 7); put(px + 2, H, Rs, ring); put(px + 3, H, Rs, ring);
    }
    put(X - 2, H, Rn, ring); put(X - 1, H, Rn, ring); put(X - 2, H, Rs, ring); put(X - 1, H, Rs, ring);
    for (int z = Rn; z <= Rs; ++z) {
      put(X - 3, H, z, z == B ? kRepeater : ring, z == B ? N : 0, 0, z == B ? 7 : 0);
      put(X + 4 * n, H, z, z == B ? kRepeater : ring, z == B ? S : 0, 0, z == B ? 7 : 0);
    }
    put(X - 4, H, Rn + 1, kPulsor, E); put(X - 5, H, Rn + 1, bus); put(X - 4, H, Rn + 2, bus); put(X - 5, H, Rn + 2, torch);
  };
  driver(Hr, kRedTrace, kBlueTrace, kRedTorch);
  driver(Hb, kBlueTrace, kRedTrace, kBlueTorch);
  for (int r = 0; r < rows; ++r) {
    const int Y = yf + 1 + 2 * r;
    const bool blue = r & 1;
    for (int x = x0; x < x0 + size; ++x)
      for (int z = zw; z <= zw + 2; ++z) { put(x, Y - 1, z, kGlass); put(x, Y + 1, z, kGlass); }
    for (int i = 0; i < size; ++i) {
      put(x0 + i, Y, zw, kColorLamp64);
      put(x0 + i, Y, zw + 1, kCatchor, N, patternColor(pattern, size, i, r), blue ? 4 : 0);
      put(x0 + i, Y, zw + 2, blue ? kBlueTrace : kRedTrace);
    }
  }
  // Spines: a stub off each bus, then via straight up with a wire tap at each of its rows.
  put(xr, Hr, zs, kRedTrace);
  for (int yy = Hr + 1; yy <= yTop - ((rows - 1) & 1 ? 2 : 0); ++yy)
    put(xr, yy, zs, (yy - yf - 1) >= 0 && (yy - yf - 1) % 4 == 0 ? kRedTrace : kRedVia);
  put(xbl, Hb, zs, kBlueTrace);
  for (int yy = Hb + 1; yy <= yf + 1 + 2 * (rows - 1 - ((rows - 1) & 1 ? 0 : 1)); ++yy)
    put(xbl, yy, zs, (yy - yf - 3) >= 0 && (yy - yf - 3) % 4 == 0 ? kBlueTrace : kBlueVia);
  FramebufferLayout L;
  L.x0 = x0; L.y = yf; L.z0 = zw; L.size = size; L.width = size; L.vertical = true; L.wallZ = zw; L.topY = yTop;
  L.columns = 1; L.blocks = placed; L.screenZ0 = zw; L.screenZ1 = zw; L.cacheZ = B;
  return L;
}

FramebufferLayout buildSawtoothWall(ChunkMap& w, int x0, int yf, int z0, int width, int rows, std::string_view pattern) {
  constexpr int N = 0, L = kSawtoothRun;
  width = std::clamp(width, 2, 64); rows = std::clamp(rows, 1, 64);
  int placed = 0;
  auto put = [&](int x, int yy, int z, BlockId b, CellState st = 0) {
    if (w.set(x, yy, z, b)) { if (st) w.setState(x, yy, z, st); ++placed; }
  };
  const int yTop = yf + 1 + 2 * (rows - 1);
  for (int z = z0 - 30; z <= z0 + L + 3; ++z)
    for (int x = x0 - 2; x <= x0 + width + 1; ++x) {
      for (int yy = yf; yy <= yTop + 3; ++yy) w.set(x, yy, z, kAir);
      put(x, yf - 1, z, z < z0 ? kWaferSilicon : kGlass);
    }
  for (int r = 0; r < rows; ++r) {
    const int Y = yf + 1 + 2 * r;
    for (int x = x0; x < x0 + width; ++x)
      for (int z = z0; z <= z0 + L + 1; ++z) { put(x, Y - 1, z, kGlass); put(x, Y + 1, z, kGlass); }
    for (int i = 0; i < width; ++i) {
      const int x = x0 + i;
      const bool red = ((i + r) & 1) == 0;
      const BlockId wire = red ? kRedTrace : kBlueTrace;
      put(x, Y, z0, kColorLamp64);
      // Split Redirector: red front into the lamp; the blue lane (fed by the blue runs either side)
      // points back into its own red run, where a blue output cannot go: no cross-talk.
      if (red) put(x, Y, z0 + 1, kRedirector, CellState(makeState(0, N, 4 | 2) | (1u << 14)));  // blue lane off
      else put(x, Y, z0 + 1, wire);
      for (int z = z0 + 2; z <= z0 + L; ++z) put(x, Y, z, wire);
      // Memor value so the lamp shows the pattern colour after the run's decay.
      const int col = std::clamp(patternColor(pattern, std::max(width, rows), i, r), 1, kMaxSignal - L);
      const int v = col + (red ? L - 2 : L - 1);  // red: one wire fewer (the Redirector passes it on)
      put(x, Y, z0 + L + 1, kMemor, makeState(v, N, 0, true));
    }
  }
  FramebufferLayout L2;
  L2.x0 = x0; L2.y = yf; L2.z0 = z0; L2.size = width; L2.width = width; L2.vertical = true; L2.wallZ = z0; L2.topY = yTop;
  L2.columns = 1; L2.blocks = placed; L2.screenZ0 = z0; L2.screenZ1 = z0; L2.cacheZ = z0 + L + 1;
  return L2;
}

FramebufferLayout buildWireTorture(ChunkMap& w, int X, int Y, int Z) {
  constexpr int S = 2;
  const int F = Z + kTortureField;
  int placed = 0;
  auto put = [&](int x, int yy, int z, BlockId b, CellState st = 0) {
    if (w.set(x, yy, z, b)) { if (st) w.setState(x, yy, z, st); ++placed; }
  };
  for (int z = Z - 4; z <= F + 50; ++z)
    for (int x = X - 4; x <= X + 54; ++x) {
      for (int yy = Y; yy <= Y + 14; ++yy) w.set(x, yy, z, kAir);
      put(x, Y - 1, z, kWaferSilicon);
    }
  // Torch inverter: torch on block s, its wire comes back beside s, so s powers off the torch the
  // next tick: the network flips every tick. Output continues east at y + 1 from (x + 1).
  auto osc = [&](int x, int y, int z) {
    put(x, y, z, kWaferSilicon); put(x, y + 1, z, kRedTorch);
    put(x + 1, y, z, kWaferSilicon); put(x + 1, y + 1, z, kRedTrace);
    put(x + 1, y, z + 1, kRedTrace); put(x, y, z + 1, kRedTrace);
  };
  // Stairs over each step block (up then down) and the same block over the lower wire (cut).
  const BlockId steps[5] = {kWaferSilicon, kGlass, kFrostedGlass, kCeramic, kSiliconSlab};
  for (int k = 0; k < 5; ++k) {
    const int z = Z + 2 * k;
    put(X, Y, z, kRedTorch); put(X + 1, Y, z, kRedTrace); put(X + 2, Y, z, steps[k]); put(X + 3, Y, z, steps[k]);
    put(X + 2, Y + 1, z, kRedTrace); put(X + 3, Y + 1, z, kRedTrace); put(X + 4, Y, z, kRedTrace); put(X + 5, Y, z, kRedTrace);
    put(X + 8, Y, z, kRedTorch); put(X + 9, Y, z, kRedTrace); put(X + 9, Y + 1, z, steps[k]); put(X + 10, Y, z, kWaferSilicon);
    put(X + 10, Y + 1, z, kRedTrace); put(X + 11, Y + 1, z, kRedTrace); put(X + 11, Y, z, kWaferSilicon);
  }
  // Ceiling wire under glass.
  for (int x = X; x <= X + 6; ++x) { put(x, Y + 3, Z + 10, kGlass); put(x, Y + 2, Z + 10, x == X ? kRedTorch : kRedTrace); }
  // Via tower (8 high), a wire beside it that must stay dark, blue beside red at the foot.
  put(X, Y, Z + 12, kRedTorch); put(X + 1, Y, Z + 12, kRedTrace);
  for (int yy = Y + 1; yy <= Y + 8; ++yy) put(X + 1, yy, Z + 12, kRedVia);
  put(X + 1, Y + 9, Z + 12, kRedTrace); put(X + 2, Y + 9, Z + 12, kRedTrace); put(X + 2, Y + 8, Z + 12, kGlass); put(X + 1, Y + 8, Z + 11, kGlass);
  put(X + 2, Y + 4, Z + 12, kGlass); put(X + 2, Y + 5, Z + 12, kRedTrace);
  put(X + 1, Y, Z + 13, kBlueTrace); put(X + 2, Y, Z + 13, kBlueTrace);
  // Repeater hard power: into silicon (lights top / beyond), into glass and ceramic (nothing).
  const BlockId targets[3] = {kWaferSilicon, kGlass, kCeramic};
  for (int k = 0; k < 3; ++k) {
    const int z = Z + 15 + 2 * k;
    put(X, Y, z, kRedTorch); put(X + 1, Y, z, kRedTrace); put(X + 2, Y, z, kRepeater, makeState(0, 1, 0));
    put(X + 3, Y, z, targets[k]); put(X + 3, Y + 1, z, kRedTrace); put(X + 4, Y, z, kRedTrace); put(X + 5, Y, z, kColorLamp64);
  }
  // Catchor chain on an oscillating line: catchors (match = the line strength there) fire on each
  // rising edge into Redirectors (output south) and lamps.
  osc(X, Y, Z + 23);
  for (int x = X + 2; x <= X + 10; ++x) { put(x, Y, Z + 23, kWaferSilicon); put(x, Y + 1, Z + 23, kRedTrace); }
  for (int i = 4; i <= 8; i += 2) {
    const int x = X + i;
    for (int z = Z + 24; z <= Z + 26; ++z) put(x, Y, z, kWaferSilicon);
    put(x, Y + 1, Z + 24, kCatchor, makeState(kMaxSignal - (i - 1), S, 0));
    put(x, Y + 1, Z + 25, kRedirector, makeState(0, S));  // fed by the Catchor (colourless): unsplit
    put(x, Y + 1, Z + 26, kColorLamp64);
  }
  // Every-wire-flips field: 24 rows x 48 stairs, an oscillator at each row start.
  for (int r = 0; r < 24; ++r) {
    const int z = F + 2 * r;
    osc(X, Y, z);
    for (int x = X + 2; x < X + 50; ++x) {
      const bool up = ((x - X) & 1) == 0;
      if (up) { put(x, Y, z, kWaferSilicon); put(x, Y + 1, z, kRedTrace); }
      else put(x, Y, z, kRedTrace);
    }
  }
  FramebufferLayout L;
  L.x0 = X; L.y = Y; L.z0 = Z; L.size = 50; L.width = 50; L.columns = 1; L.blocks = placed; L.vertical = false;
  L.screenZ0 = Z; L.screenZ1 = Z + 27; L.cacheZ = F + 24;
  return L;
}
}  // namespace lithokraft
