// The first eight models, authored cell-by-cell at level 6 (the full 64^3
// micro-detail volume of one block) through ModelBuilder. All authoring is
// deterministic: fixed call order and a seeded PRNG, so re-authoring yields
// byte-identical .lthm files (a tested property).
//
// Coordinate convention: x right, y up, z toward the viewer (+z = front).
// The builder clamps fillBox and ignores out-of-range setCell, so branchy
// organic code can overshoot safely.

#include "models.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lithokraft::model::authored {
namespace {

constexpr int LV = 6;  // author at full 64^3 micro-detail
constexpr const char* kAuthor = "LithoKraft workstream 5";

using namespace palette;

void box(ModelBuilder& b, int x0, int y0, int z0, int x1, int y1, int z1,
         uint8_t m) {
  b.fillBox(LV, x0, y0, z0, x1, y1, z1, m);
}
void carve(ModelBuilder& b, int x0, int y0, int z0, int x1, int y1, int z1) {
  b.fillBox(LV, x0, y0, z0, x1, y1, z1, 0);
}
void px(ModelBuilder& b, int x, int y, int z, uint8_t m) {
  b.setCell(LV, x, y, z, m);
}

// 3D Bresenham line. r > 0 stamps a (2r+1)^3 cube per step (builder-clamped).
void line(ModelBuilder& b, int x0, int y0, int z0, int x1, int y1, int z1,
          uint8_t m, int r = 0) {
  int x = x0, y = y0, z = z0;
  const int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0),
            dz = std::abs(z1 - z0);
  const int xs = x0 < x1 ? 1 : -1, ys = y0 < y1 ? 1 : -1,
            zs = z0 < z1 ? 1 : -1;
  auto stamp = [&] {
    if (r <= 0)
      px(b, x, y, z, m);
    else
      box(b, x - r, y - r, z - r, x + r, y + r, z + r, m);
  };
  if (dx >= dy && dx >= dz) {
    int ey = 2 * dy - dx, ez = 2 * dz - dx;
    for (int i = 0; i <= dx; ++i) {
      stamp();
      if (ey > 0) {
        y += ys;
        ey -= 2 * dx;
      }
      if (ez > 0) {
        z += zs;
        ez -= 2 * dx;
      }
      ey += 2 * dy;
      ez += 2 * dz;
      x += xs;
    }
  } else if (dy >= dx && dy >= dz) {
    int ex = 2 * dx - dy, ez = 2 * dz - dy;
    for (int i = 0; i <= dy; ++i) {
      stamp();
      if (ex > 0) {
        x += xs;
        ex -= 2 * dy;
      }
      if (ez > 0) {
        z += zs;
        ez -= 2 * dy;
      }
      ex += 2 * dx;
      ez += 2 * dz;
      y += ys;
    }
  } else {
    int ex = 2 * dx - dz, ey = 2 * dy - dz;
    for (int i = 0; i <= dz; ++i) {
      stamp();
      if (ex > 0) {
        x += xs;
        ex -= 2 * dz;
      }
      if (ey > 0) {
        y += ys;
        ey -= 2 * dz;
      }
      ex += 2 * dx;
      ey += 2 * dy;
      z += zs;
    }
  }
}

// Deterministic PRNG (splitmix64). Fixed seeds => byte-identical re-author.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  int below(int n) { return static_cast<int>(next() % static_cast<uint64_t>(n)); }
  int range(int lo, int hi) { return lo + below(hi - lo + 1); }  // inclusive
};

// Shared tool-handle: wooden shaft with copper grip bands.
void toolHandle(ModelBuilder& b, int cx, int cz, int y0, int y1) {
  box(b, cx - 1, y0, cz - 1, cx + 2, y1, cz + 2, kHandleWood);
  const int b1 = y0 + 6, b2 = y0 + 14;
  box(b, cx - 2, b1, cz - 2, cx + 3, b1 + 2, cz + 3, kCopper);
  box(b, cx - 2, b2, cz - 2, cx + 3, b2 + 2, cz + 3, kCopper);
}

}  // namespace

const char* palette::name(uint8_t m) {
  switch (m) {
    case kHandleWood:
      return "HandleWood";
    case kHeadMetal:
      return "HeadMetal";
    case kBladeEdge:
      return "BladeEdge";
    case kCopper:
      return "Copper";
    case kTraceRed:
      return "TraceRed";
    case kTraceBlue:
      return "TraceBlue";
    case kShellDark:
      return "ShellDark";
    case kShellPlate:
      return "ShellPlate";
    case kSeamRed:
      return "SeamRed";
    case kSeamBlue:
      return "SeamBlue";
    case kCrystalBlue:
      return "CrystalBlue";
    case kCrystalAmber:
      return "CrystalAmber";
    case kCrystalCore:
      return "CrystalCore";
    default:
      return "?";
  }
}

// ---------------------------------------------------------------- armory ---

Model authorBashor() {
  // BashOR: hammer. Chunky head, steel striking face, stepped peen, and a
  // bash-prompt ">_" etched in glowing trace on the head's face.
  ModelBuilder b("BashOR", kAuthor);
  toolHandle(b, 31, 31, 4, 38);
  box(b, 29, 0, 29, 34, 3, 34, kCopper);            // pommel cap
  box(b, 28, 37, 28, 35, 40, 35, kCopper);          // head collar
  box(b, 18, 39, 28, 45, 50, 35, kHeadMetal);       // chunky head
  box(b, 14, 41, 29, 17, 48, 34, kHeadMetal);       // stepped peen
  box(b, 12, 43, 30, 13, 46, 33, kHeadMetal);       // peen tip step
  box(b, 44, 39, 28, 46, 50, 35, kBladeEdge);       // striking face plate
  // Trace inlay on the head's front face (z = 35): a signal line plus ">".
  for (int x = 20; x <= 42; ++x) px(b, x, 44, 35, kTraceBlue);
  px(b, 24, 47, 35, kTraceRed);
  px(b, 26, 44, 35, kTraceRed);
  px(b, 24, 41, 35, kTraceRed);
  px(b, 28, 44, 35, kTraceBlue);  // the prompt's block cursor
  return b.build();
}

Model authorPickor() {
  // PickOR: pickaxe. Long head bar with twin downward-curving points,
  // blue-hot tips, red trace inlay along the bar.
  ModelBuilder b("PickOR", kAuthor);
  toolHandle(b, 31, 30, 4, 36);
  box(b, 29, 0, 28, 34, 3, 33, kCopper);
  box(b, 28, 35, 28, 35, 44, 33, kCopper);          // center mount collar
  box(b, 14, 37, 29, 49, 42, 32, kHeadMetal);       // head bar
  // Twin points, curving down and out (stepped).
  box(b, 10, 34, 29, 15, 39, 32, kHeadMetal);
  box(b, 7, 30, 29, 11, 35, 32, kHeadMetal);
  box(b, 5, 26, 29, 8, 31, 32, kHeadMetal);
  box(b, 5, 24, 29, 6, 26, 32, kTraceBlue);         // hot tip (left)
  box(b, 48, 34, 29, 53, 39, 32, kHeadMetal);
  box(b, 52, 30, 29, 56, 35, 32, kHeadMetal);
  box(b, 55, 26, 29, 58, 31, 32, kHeadMetal);
  box(b, 57, 24, 29, 58, 26, 32, kTraceBlue);       // hot tip (right)
  for (int x = 16; x <= 47; ++x) px(b, x, 39, 32, kTraceRed);  // inlay
  return b.build();
}

Model authorChopor() {
  // ChopOR: axe. Broad flared blade with a bright edge, hammer poll on the
  // back, and a zigzag "chops code" trace inlay on the blade.
  ModelBuilder b("ChopOR", kAuthor);
  toolHandle(b, 30, 30, 4, 40);
  box(b, 28, 0, 28, 33, 3, 33, kCopper);
  box(b, 27, 36, 27, 34, 41, 34, kCopper);          // collar
  box(b, 33, 38, 28, 44, 50, 33, kHeadMetal);       // blade body
  box(b, 44, 36, 28, 47, 52, 33, kHeadMetal);       // flared bit
  box(b, 47, 36, 28, 49, 52, 33, kBladeEdge);       // cutting edge
  box(b, 24, 40, 29, 28, 48, 32, kHeadMetal);       // poll (back)
  // Zigzag inlay on the blade's face.
  const int zx[8] = {36, 38, 40, 42, 40, 38, 36, 38};
  const int zy[8] = {46, 44, 46, 44, 42, 44, 42, 44};
  for (int i = 0; i < 8; ++i) px(b, zx[i], zy[i], 33, kTraceBlue);
  return b.build();
}

Model authorHeapor() {
  // HeapOR: shovel. T-grip, long shaft, cupped scoop with a bright digging
  // lip and a glowing blue rim.
  ModelBuilder b("HeapOR", kAuthor);
  box(b, 26, 54, 29, 37, 56, 32, kHandleWood);      // T-grip
  box(b, 30, 30, 30, 33, 54, 33, kHandleWood);       // shaft
  box(b, 29, 22, 29, 34, 30, 34, kHeadMetal);       // socket
  box(b, 29, 28, 29, 34, 31, 34, kCopper);          // socket band
  box(b, 25, 8, 28, 38, 24, 35, kHeadMetal);        // scoop shell
  carve(b, 27, 11, 30, 36, 24, 33);                 // cup the scoop
  box(b, 25, 8, 34, 38, 10, 35, kBladeEdge);        // digging lip
  for (int x = 25; x <= 38; ++x) {                  // glowing rim
    px(b, x, 24, 28, kTraceBlue);
    px(b, x, 24, 35, kTraceBlue);
  }
  for (int z = 29; z <= 34; ++z) {
    px(b, 25, 24, z, kTraceBlue);
    px(b, 38, 24, z, kTraceBlue);
  }
  return b.build();
}

Model authorSeedor() {
  // SeedOR: hoe. Angled copper tines with blue-hot points, and a scatter
  // of floating procgen seeds above the bar.
  ModelBuilder b("SeedOR", kAuthor);
  toolHandle(b, 31, 30, 14, 46);
  box(b, 29, 46, 28, 34, 49, 33, kHandleWood);      // grip cap
  box(b, 28, 8, 28, 35, 16, 33, kCopper);           // collar
  box(b, 16, 10, 29, 47, 15, 32, kHeadMetal);       // head bar
  const int tx[4] = {20, 28, 36, 44};
  for (int i = 0; i < 4; ++i) {                     // angled tines
    box(b, tx[i], 4, 30, tx[i] + 2, 10, 32, kCopper);
    box(b, tx[i], 2, 32, tx[i] + 2, 4, 35, kCopper);  // forward angle
    box(b, tx[i], 2, 34, tx[i] + 2, 3, 35, kTraceBlue);  // hot point
  }
  // Procgen seeds drifting above the bar.
  const int sx[5] = {20, 26, 32, 38, 44};
  const int sy[5] = {17, 18, 17, 18, 17};
  for (int i = 0; i < 5; ++i) px(b, sx[i], sy[i], 30, kTraceBlue);
  return b.build();
}

Model authorSlicor() {
  // SlicOR: sword. Long bright blade with a glowing fuller, copper guard,
  // banded grip, and a red gem set in the pommel.
  ModelBuilder b("SlicOR", kAuthor);
  box(b, 30, 18, 30, 33, 52, 33, kBladeEdge);       // blade
  for (int y = 22; y <= 48; ++y) {                  // glowing fuller
    px(b, 30, y, 31, kTraceBlue);
    px(b, 30, y, 32, kTraceBlue);
    px(b, 33, y, 31, kTraceBlue);
    px(b, 33, y, 32, kTraceBlue);
  }
  box(b, 30, 53, 30, 33, 55, 33, kBladeEdge);       // tip taper
  box(b, 31, 56, 31, 32, 58, 32, kBladeEdge);
  px(b, 31, 59, 31, kBladeEdge);
  px(b, 32, 59, 32, kBladeEdge);
  box(b, 26, 14, 28, 37, 17, 35, kCopper);          // guard
  box(b, 30, 6, 30, 33, 13, 33, kHandleWood);       // grip
  box(b, 29, 8, 29, 34, 9, 34, kCopper);            // grip bands
  box(b, 29, 11, 29, 34, 12, 34, kCopper);
  box(b, 29, 2, 29, 34, 5, 34, kCopper);            // pommel
  px(b, 31, 3, 34, kTraceRed);                      // pommel gem
  px(b, 32, 4, 34, kTraceRed);
  px(b, 31, 5, 34, kTraceRed);
  return b.build();
}

// ------------------------------------------------- lore items (2026-10-12) --
// Models for the rest of the item registry (content/items.h): the LOCKED
// weapons/special items that had no .lthm yet, the Fragment starting book,
// Sniffor and the Atlas. Same authoring palette and conventions as the armory.

Model authorFirewall() {
  // Firewall: shield. Broad steel plate with a copper rim, a central boss,
  // and a red/blue trace cross inlay on the front face -- the lingo bows.
  ModelBuilder b("Firewall", kAuthor);
  box(b, 20, 10, 29, 43, 53, 32, kHeadMetal);       // plate
  box(b, 18, 8, 28, 45, 55, 33, kCopper);           // rim (behind plate edge)
  box(b, 20, 10, 29, 43, 53, 32, kHeadMetal);       // plate face over rim core
  box(b, 28, 28, 33, 35, 35, 36, kCopper);          // boss
  px(b, 31, 31, 36, kTraceBlue);                    // boss core
  px(b, 32, 32, 36, kTraceBlue);
  for (int y = 14; y <= 49; ++y) px(b, 31, y, 33, kTraceRed);    // vertical inlay
  for (int x = 24; x <= 39; ++x) px(b, x, 31, 33, kTraceBlue);   // horizontal inlay
  box(b, 29, 24, 24, 34, 39, 28, kHandleWood);      // grip behind the plate
  return b.build();
}

Model authorPing() {
  // Ping: bow. Stepped curved limbs of handle wood with copper tips, a taut
  // trace-blue string, and a notched packet (arrow) resting at the grip.
  ModelBuilder b("Ping", kAuthor);
  box(b, 30, 28, 30, 33, 36, 33, kHandleWood);      // grip
  box(b, 29, 30, 29, 34, 34, 34, kCopper);          // grip wrap
  // Upper limb, curving forward (+z) as it rises.
  box(b, 30, 36, 30, 33, 42, 33, kHandleWood);
  box(b, 30, 42, 31, 33, 48, 34, kHandleWood);
  box(b, 30, 48, 32, 33, 54, 35, kHandleWood);
  box(b, 30, 54, 33, 33, 58, 36, kCopper);          // upper tip
  // Lower limb, mirrored.
  box(b, 30, 22, 30, 33, 28, 33, kHandleWood);
  box(b, 30, 16, 31, 33, 22, 34, kHandleWood);
  box(b, 30, 10, 32, 33, 16, 35, kHandleWood);
  box(b, 30, 6, 33, 33, 10, 36, kCopper);           // lower tip
  // String: straight line between the tips, behind the limbs (z = 27).
  line(b, 31, 57, 27, 31, 7, 27, kTraceBlue);
  // The packet: a slim arrow nocked at the grip, pointing +z.
  box(b, 31, 31, 34, 32, 32, 52, kBladeEdge);
  px(b, 31, 31, 53, kTraceRed);                     // request head (awaiting response)
  px(b, 32, 32, 53, kTraceRed);
  return b.build();
}

Model authorCallback() {
  // Callback: the returning thrown weapon (trident role). Long shaft, three
  // prongs with blue-hot tips, and the copper return ring around the shaft --
  // thrown, and it always comes back.
  ModelBuilder b("Callback", kAuthor);
  box(b, 30, 4, 30, 33, 40, 33, kHandleWood);       // shaft
  box(b, 29, 10, 29, 34, 12, 34, kCopper);          // shaft bands
  box(b, 29, 20, 29, 34, 22, 34, kCopper);
  // Return ring: a copper square loop around the shaft at y 28..34.
  box(b, 26, 28, 30, 37, 29, 33, kCopper);
  box(b, 26, 33, 30, 37, 34, 33, kCopper);
  box(b, 26, 29, 30, 27, 33, 33, kCopper);
  box(b, 36, 29, 30, 37, 33, 33, kCopper);
  box(b, 28, 40, 28, 35, 43, 35, kCopper);          // head collar
  // Three prongs.
  box(b, 24, 43, 30, 26, 56, 33, kBladeEdge);       // left prong
  box(b, 30, 43, 30, 33, 58, 33, kBladeEdge);       // centre prong
  box(b, 37, 43, 30, 39, 56, 33, kBladeEdge);       // right prong
  px(b, 25, 57, 31, kTraceBlue);                    // hot tips
  px(b, 31, 59, 31, kTraceBlue);
  px(b, 38, 57, 31, kTraceBlue);
  box(b, 29, 0, 29, 34, 3, 34, kCopper);            // butt cap
  return b.build();
}

Model authorDebuggor() {
  // DebuggOR: the debug stick. A wand whose head is a free rotation ring
  // around a dial core -- it rotates blocks after placement.
  ModelBuilder b("DebuggOR", kAuthor);
  box(b, 30, 4, 30, 33, 34, 33, kHandleWood);       // wand shaft
  box(b, 29, 8, 29, 34, 10, 34, kCopper);           // grip bands
  box(b, 29, 16, 29, 34, 18, 34, kCopper);
  px(b, 31, 24, 33, kTraceRed);                     // setting marks up the shaft
  px(b, 31, 27, 33, kTraceBlue);
  px(b, 31, 30, 33, kTraceRed);
  // Rotation ring at the head (square loop in the x/y plane, z 30..33).
  box(b, 24, 44, 30, 39, 46, 33, kHeadMetal);       // ring top
  box(b, 24, 56, 30, 39, 58, 33, kHeadMetal);       // ring bottom (head end)
  box(b, 24, 46, 30, 26, 56, 33, kHeadMetal);       // ring left
  box(b, 37, 46, 30, 39, 56, 33, kHeadMetal);       // ring right
  box(b, 29, 34, 30, 34, 44, 33, kCopper);          // neck into the ring
  box(b, 30, 49, 30, 33, 53, 33, kBladeEdge);       // dial core
  px(b, 31, 51, 33, kTraceBlue);                    // dial pointer
  px(b, 31, 57, 31, kTraceRed);                     // ring detents
  px(b, 25, 51, 31, kTraceRed);
  px(b, 38, 51, 31, kTraceRed);
  return b.build();
}

Model authorFragment() {
  // The Fragment: the starting book. Dark bound cover, pale page block, a
  // copper clasp, and the fragment glyph -- a broken trace square -- inlaid
  // in red/blue on the front cover.
  ModelBuilder b("Fragment", kAuthor);
  box(b, 18, 8, 26, 45, 55, 30, kShellDark);        // back cover + spine mass
  box(b, 20, 10, 30, 45, 53, 36, kShellPlate);      // page block
  box(b, 18, 8, 36, 45, 55, 38, kShellDark);        // front cover
  box(b, 18, 8, 26, 21, 55, 38, kCopper);           // spine band
  box(b, 43, 28, 30, 46, 35, 38, kCopper);          // clasp
  // Broken-square glyph on the front cover (z = 38): three sides + a gap.
  for (int x = 27; x <= 36; ++x) px(b, x, 42, 38, kTraceRed);
  for (int y = 33; y <= 42; ++y) px(b, 27, y, 38, kTraceRed);
  for (int x = 27; x <= 32; ++x) px(b, x, 33, 38, kTraceBlue);
  for (int y = 33; y <= 38; ++y) px(b, 36, y, 38, kTraceBlue);
  px(b, 31, 37, 38, kTraceBlue);                    // the missing piece, offset
  px(b, 32, 38, 38, kTraceBlue);
  return b.build();
}

Model authorSniffor() {
  // Sniffor: handheld early signal receiver. Body with a glowing screen, a
  // small dish on top and a whip antenna -- it transcodes ambient Signal.
  ModelBuilder b("Sniffor", kAuthor);
  box(b, 24, 10, 28, 39, 38, 35, kShellDark);       // body
  box(b, 26, 24, 35, 37, 36, 36, kTraceBlue);       // screen (lit face)
  for (int x = 28; x <= 35; ++x) px(b, x, 30, 36, kTraceRed);  // signal trace on screen
  px(b, 28, 14, 35, kTraceRed);                     // buttons
  px(b, 31, 14, 35, kTraceBlue);
  px(b, 34, 14, 35, kTraceRed);
  box(b, 28, 38, 29, 35, 41, 34, kCopper);          // dish mount
  box(b, 24, 41, 26, 39, 43, 37, kHeadMetal);       // dish (stepped)
  box(b, 27, 43, 28, 36, 45, 35, kHeadMetal);
  px(b, 31, 45, 31, kTraceBlue);                    // dish feed
  line(b, 37, 38, 30, 41, 58, 30, kHeadMetal);      // whip antenna
  px(b, 41, 59, 30, kTraceRed);                     // antenna tip (Daemon channel blip)
  box(b, 27, 4, 29, 36, 10, 34, kHandleWood);       // grip
  return b.build();
}

Model authorAtlas() {
  // Atlas: the in-game map. A framed tablet: copper frame, pale chart face,
  // and a red/blue route line with grid ticks inlaid on the chart.
  ModelBuilder b("Atlas", kAuthor);
  box(b, 12, 14, 29, 51, 49, 32, kCopper);          // frame
  box(b, 15, 17, 32, 48, 46, 34, kShellPlate);      // chart face
  for (int x = 19; x <= 44; x += 5)                   // chart grid (vertical ticks)
    for (int y = 20; y <= 43; ++y) px(b, x, y, 34, kCopper);
  for (int y = 20; y <= 43; y += 5)                   // chart grid (horizontal ticks)
    for (int x = 19; x <= 44; ++x) px(b, x, y, 34, kCopper);
  // Route line: red in, blue out, meeting at the marked site.
  line(b, 18, 20, 34, 30, 30, 34, kTraceRed);
  line(b, 30, 30, 34, 44, 42, 34, kTraceBlue);
  px(b, 30, 30, 34, kTraceRed);
  px(b, 31, 31, 34, kTraceRed);
  box(b, 28, 2, 30, 35, 14, 33, kHandleWood);       // handle below the frame
  return b.build();
}

// ------------------------------------------------------------ trace tree ---

namespace {
// One forked trace branch: 2-3 segments per depth level, copper tube,
// forking in two at each joint, ending in a glowing red/blue tip.
void treeBranch(ModelBuilder& b, Rng& rng, int x, int y, int z, int dx, int dy,
                int dz, int depth, int& tipFlip) {
  const int segs = depth == 2 ? 1 : 2;
  const int r = depth == 0 ? 1 : 0;
  for (int s = 0; s < segs; ++s) {
    const int segLen =
        depth == 0 ? rng.range(4, 6) : (depth == 1 ? rng.range(3, 5) : rng.range(2, 4));
    // Steer back inside the volume near the top edge.
    int ndx = dx, ndy = dy, ndz = dz;
    if (y > 54) {
      ndy = 1;
      if (x > 44) ndx = -1;
      if (x < 20) ndx = 1;
      if (z > 44) ndz = -1;
      if (z < 20) ndz = 1;
    }
    const int nx = x + ndx * segLen + rng.range(-1, 1);
    const int ny = y + ndy * segLen;
    const int nz = z + ndz * segLen + rng.range(-1, 1);
    line(b, x, y, z, nx, ny, nz, kCopper, r);
    x = nx;
    y = ny;
    z = nz;
    dx = std::clamp(dx + rng.range(-1, 1), -1, 1);
    dz = std::clamp(dz + rng.range(-1, 1), -1, 1);
    dy = rng.below(3) == 0 ? 1 : 2;
    if (dx == 0 && dz == 0) dx = rng.below(2) ? 1 : -1;
  }
  if (depth >= 2) {
    // Glowing tip: small cross, alternating red/blue like the concept art.
    const uint8_t tipMat =
        (tipFlip++ % 2 == 0) ? kTraceRed : kTraceBlue;
    px(b, x, y, z, tipMat);
    px(b, x + 1, y, z, tipMat);
    px(b, x - 1, y, z, tipMat);
    px(b, x, y + 1, z, tipMat);
    px(b, x, y, z + 1, tipMat);
    px(b, x, y, z - 1, tipMat);
    return;
  }
  for (int c = 0; c < 2; ++c) {
    int cdx = std::clamp(dx + (c == 0 ? 1 : -1) * rng.range(0, 1), -1, 1);
    int cdz = std::clamp(dz + (c == 0 ? -1 : 1) * rng.range(0, 1), -1, 1);
    if (cdx == 0 && cdz == 0) cdx = c == 0 ? 1 : -1;
    treeBranch(b, rng, x, y, z, cdx, rng.range(1, 2), cdz, depth + 1, tipFlip);
  }
}

// One surface root: two-segment meander with a red-hot tip.
void treeRoot(ModelBuilder& b, Rng& rng, int dx, int dz) {
  const int len = rng.range(9, 12);
  const int mx = 32 + dx * (len / 2) + rng.range(-2, 2);
  const int mz = 32 + dz * (len / 2) + rng.range(-2, 2);
  const int ex = 32 + dx * len, ez = 32 + dz * len;
  line(b, 32, 2, 32, mx, 2, mz, kCopper);
  line(b, mx, 2, mz, ex, 2, ez, kCopper);
  px(b, ex, 2, ez, kTraceRed);
  px(b, ex, 3, ez, kTraceRed);
}
}  // namespace

Model authorTraceTree() {
  // Trace tree: the hero organic. Via-trunk (stepped copper column with via
  // collars and glowing via rings), root flare on a substrate disc, and
  // forking trace branches tipped in alternating red/blue glow.
  ModelBuilder b("Trace Tree", kAuthor);
  Rng rng(0x7A11CE);

  // Substrate disc (dark PCB the tree grows from).
  for (int dx = -13; dx <= 13; ++dx)
    for (int dz = -13; dz <= 13; ++dz)
      if (dx * dx + dz * dz <= 169)
        box(b, 32 + dx, 0, 32 + dz, 32 + dx, 1, 32 + dz, kShellDark);
  // Surface trace-runes between the roots.
  line(b, 32 + 5, 2, 32 + 2, 32 + 9, 2, 32 + 5, kTraceBlue);
  line(b, 32 - 5, 2, 32 - 3, 32 - 9, 2, 32 - 6, kTraceRed);
  line(b, 32 + 2, 2, 32 - 6, 32 + 4, 2, 32 - 10, kTraceRed);
  line(b, 32 - 3, 2, 32 + 6, 32 - 6, 2, 32 + 10, kTraceBlue);

  // Via-trunk: stepped copper column with wider via collars.
  box(b, 29, 2, 29, 35, 14, 35, kCopper);
  box(b, 27, 14, 27, 37, 16, 37, kCopper);  // via collar 1
  box(b, 30, 17, 30, 34, 28, 34, kCopper);
  box(b, 28, 28, 28, 36, 30, 36, kCopper);  // via collar 2
  box(b, 30, 31, 30, 34, 42, 34, kCopper);
  box(b, 29, 42, 29, 35, 45, 35, kCopper);  // crown
  // Glowing via rings on the collar edges.
  for (int x = 27; x <= 37; ++x) {
    px(b, x, 16, 27, kTraceBlue);
    px(b, x, 16, 37, kTraceBlue);
  }
  for (int z = 28; z <= 36; ++z) {
    px(b, 27, 16, z, kTraceBlue);
    px(b, 37, 16, z, kTraceBlue);
  }
  for (int x = 28; x <= 36; ++x) {
    px(b, x, 30, 28, kTraceBlue);
    px(b, x, 30, 36, kTraceBlue);
  }
  for (int z = 29; z <= 35; ++z) {
    px(b, 28, 30, z, kTraceBlue);
    px(b, 36, 30, z, kTraceBlue);
  }

  // Root flare: 8 roots.
  const int rdx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
  const int rdz[8] = {0, 1, 1, 1, 0, -1, -1, -1};
  for (int i = 0; i < 8; ++i) treeRoot(b, rng, rdx[i], rdz[i]);

  // Crown branches: 6 primaries fanning out and up.
  const int bdx[6] = {1, -1, 0, 0, 1, -1};
  const int bdz[6] = {0, 0, 1, -1, 1, -1};
  int tipFlip = 0;
  for (int i = 0; i < 6; ++i)
    treeBranch(b, rng, 32, 45, 32, bdx[i], 2, bdz[i], 0, tipFlip);

  return b.build();
}

// ---------------------------------------------------------------- TurtlOR ---

namespace {
// Domed shell: half-ellipsoid surface with panel seams. Seam color follows
// the panel index (red/blue alternating, like the concept art's Faraday
// cage panels).
void domeShell(ModelBuilder& b, int cx, int cy, int cz, int rx, int ry, int rz,
               int panels, uint8_t plateMat, uint8_t seamA, uint8_t seamB) {
  constexpr double kPi = 3.14159265358979323846;
  for (int x = cx - rx; x <= cx + rx; ++x)
    for (int y = cy - 1; y <= cy + ry; ++y)
      for (int z = cz - rz; z <= cz + rz; ++z) {
        const double nx = static_cast<double>(x - cx) / rx;
        const double ny = static_cast<double>(y - cy) / ry;
        const double nz = static_cast<double>(z - cz) / rz;
        const double d = nx * nx + ny * ny + nz * nz;
        if (d > 1.0 || d <= 0.72 || y < cy - 1) continue;
        uint8_t mat = plateMat;
        // Longitudinal seams: panel boundaries around the dome.
        const double ang = std::atan2(nz, nx);  // -pi..pi
        const double s = (ang + kPi) / (2.0 * kPi) * panels;
        const double frac = s - std::floor(s);
        // Latitudinal seam: one ring around the dome.
        const double t = (ny * 2.0 + 1.0) - std::floor(ny * 2.0 + 1.0);
        const int panel = static_cast<int>(std::floor(s)) % panels;
        if (frac < 0.10 || t < 0.14)
          mat = (panel % 2 == 0) ? seamA : seamB;
        px(b, x, y, z, mat);
      }
}
}  // namespace

Model authorTurtlor() {
  // TurtlOR: the turtle. Domed faceted shell with red/blue glowing Faraday
  // panel seams, plastron belly, blue-eyed head with antennae, four chunky
  // trace-striped legs — and a smaller turtle riding on its back.
  ModelBuilder b("TurtlOR", kAuthor);

  // Shell: half-ellipsoid, 10 Faraday panels.
  domeShell(b, 32, 12, 30, 16, 12, 20, 10, kShellDark, kSeamRed, kSeamBlue);
  // Shell base rim.
  for (int x = 17; x <= 47; ++x) {
    px(b, x, 9, 11, kShellPlate);
    px(b, x, 9, 49, kShellPlate);
  }
  for (int z = 12; z <= 48; ++z) {
    px(b, 17, 9, z, kShellPlate);
    px(b, 47, 9, z, kShellPlate);
  }

  box(b, 24, 6, 18, 40, 7, 42, kShellPlate);  // plastron (belly)

  // Head: neck + head + snout, blue eyes, antennae with red tips.
  box(b, 28, 8, 46, 36, 12, 52, kShellPlate);   // neck
  box(b, 27, 9, 52, 37, 15, 58, kShellDark);   // head
  box(b, 28, 10, 58, 36, 14, 60, kShellDark);  // snout
  box(b, 28, 12, 60, 29, 13, 60, kTraceBlue);  // eyes
  box(b, 35, 12, 60, 36, 13, 60, kTraceBlue);
  line(b, 30, 15, 55, 30, 20, 55, kShellDark);
  px(b, 30, 21, 55, kTraceRed);
  line(b, 34, 15, 55, 34, 20, 55, kShellDark);
  px(b, 34, 21, 55, kTraceRed);

  // Four legs: chunky, flared feet, red trace stripe.
  const int lx[4] = {14, 44, 14, 44};
  const int lz[4] = {36, 36, 18, 18};
  for (int i = 0; i < 4; ++i) {
    box(b, lx[i], 0, lz[i], lx[i] + 6, 8, lz[i] + 6, kShellDark);
    box(b, lx[i] - 1, 0, lz[i] - 1, lx[i] + 7, 2, lz[i] + 7, kShellDark);
    box(b, lx[i] + 2, 1, lz[i] + 2, lx[i] + 4, 7, lz[i] + 4, kTraceRed);
  }

  // Tail.
  box(b, 30, 7, 8, 34, 9, 14, kShellDark);
  box(b, 31, 7, 6, 33, 8, 8, kShellDark);

  // The little rider: baby turtle on the shell's back (blue seams).
  domeShell(b, 32, 23, 28, 6, 4, 7, 8, kShellDark, kSeamBlue, kSeamBlue);
  box(b, 30, 22, 34, 34, 25, 38, kShellPlate);  // baby head
  px(b, 31, 24, 38, kTraceBlue);                // baby eyes
  px(b, 33, 24, 38, kTraceBlue);
  box(b, 25, 19, 22, 27, 21, 24, kShellDark);   // baby legs
  box(b, 37, 19, 22, 39, 21, 24, kShellDark);
  box(b, 25, 19, 32, 27, 21, 34, kShellDark);
  box(b, 37, 19, 32, 39, 21, 34, kShellDark);

  return b.build();
}

const std::vector<AuthoredModel>& allAuthoredModels() {
  static const std::vector<AuthoredModel> kAll = {
      {"bashor", "BashOR", authorBashor},
      {"pickor", "PickOR", authorPickor},
      {"chopor", "ChopOR", authorChopor},
      {"heapor", "HeapOR", authorHeapor},
      {"seedor", "SeedOR", authorSeedor},
      {"slicor", "SlicOR", authorSlicor},
      {"firewall", "Firewall", authorFirewall},
      {"ping", "Ping", authorPing},
      {"callback", "Callback", authorCallback},
      {"debuggor", "DebuggOR", authorDebuggor},
      {"fragment", "Fragment", authorFragment},
      {"sniffor", "Sniffor", authorSniffor},
      {"atlas", "Atlas", authorAtlas},
      {"trace_tree", "Trace Tree", authorTraceTree},
      {"turtlor", "TurtlOR", authorTurtlor},
  };
  return kAll;
}

}  // namespace lithokraft::model::authored
