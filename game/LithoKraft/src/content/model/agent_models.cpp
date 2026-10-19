// Agent model authoring: see agent_models.h for the design notes.
//
// The Elfie figure is built procedurally with box/set ops on a 32^3 grid,
// last-write-wins — the same construction as the design prototype, kept in
// lockstep by agent_models_selftest (cell count + FNV-1a hash over the
// sorted cell list, so any drift between design and code fails loudly).
#include "agent_models.h"

#include <array>
#include <utility>

#include "litholattice/anim/riggen.h"

namespace lithokraft::model::agents {
namespace {

constexpr int kGrid = 32;

/// Scratch build grid: palette byte per cell, 0 = empty. Writes are
/// last-write-wins, mirroring the design prototype exactly.
struct Builder {
  std::array<uint8_t, kGrid * kGrid * kGrid> v{};
  static constexpr size_t idx(int x, int y, int z) {
    return size_t(x) + kGrid * (size_t(z) + kGrid * size_t(y));
  }
  void set(int x, int y, int z, uint8_t m) { v[idx(x, y, z)] = m; }
  void box(int x0, int y0, int z0, int x1, int y1, int z1, uint8_t m) {
    for (int y = y0; y <= y1; ++y)
      for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) set(x, y, z, m);
  }
  litholattice::procgen::LithoStampModel finish() const {
    litholattice::procgen::LithoStampModel out;
    out.size = kGrid;
    // Emit in (x, y, z) lexicographic order: the canonical cell order.
    for (int x = 0; x < kGrid; ++x)
      for (int y = 0; y < kGrid; ++y)
        for (int z = 0; z < kGrid; ++z) {
          const uint8_t m = v[idx(x, y, z)];
          if (m) out.cells.push_back({x, y, z, m});
        }
    return out;
  }
};

}  // namespace

namespace palette {

const char* name(uint8_t material) {
  switch (material) {
    case kSuitMidnight: return "suit_midnight";
    case kSuitPanel: return "suit_panel";
    case kGlowPurple: return "glow_purple";
    case kGlowOrange: return "glow_orange";
    case kHairMidnight: return "hair_midnight";
    case kSkin: return "skin";
    case kGlowCore: return "glow_core";
    case kTrimDark: return "trim_dark";
    default: return "unknown";
  }
}

Rgb color(uint8_t material) {
  switch (material) {
    case kSuitMidnight: return {13, 19, 48};
    case kSuitPanel: return {27, 38, 83};
    case kGlowPurple: return {178, 107, 255};
    case kGlowOrange: return {255, 140, 26};
    case kHairMidnight: return {19, 28, 64};
    case kSkin: return {236, 201, 163};
    case kGlowCore: return {233, 220, 255};
    case kTrimDark: return {5, 7, 15};
    default: return {255, 0, 255};  // magenta: unbound palette id (authoring error)
  }
}

}  // namespace palette

litholattice::procgen::LithoStampModel authorElfie() {
  using namespace palette;
  Builder b;

  // Legs + boots (y 0..10): left leg x12-14, right leg x17-19.
  for (int legX : {12, 17}) {
    b.box(legX, 3, 14, legX + 2, 10, 16, kSuitMidnight);  // leg
    b.box(legX, 0, 14, legX + 2, 2, 16, kTrimDark);       // boot
    b.box(legX, 1, 17, legX + 2, 1, 17, kGlowOrange);     // boot front glow stripe
  }
  // Outer leg glow seams (orange): left outer face x=12, right outer x=19.
  for (int y = 4; y <= 9; ++y) {
    b.set(12, y, 15, kGlowOrange);
    b.set(19, y, 15, kGlowOrange);
  }

  // Pelvis (y 11..12) + belt glow line.
  b.box(12, 11, 14, 19, 12, 17, kSuitPanel);
  b.box(12, 12, 17, 19, 12, 17, kGlowOrange);

  // Torso (y 13..21).
  b.box(11, 13, 13, 20, 18, 17, kSuitMidnight);
  b.box(11, 19, 13, 20, 21, 17, kSuitPanel);  // chest / shoulders
  b.box(10, 20, 14, 10, 21, 16, kSuitPanel);  // shoulder caps
  b.box(21, 20, 14, 21, 21, 16, kSuitPanel);
  // Chest circuit (purple glow) on the front face z=17: center stem, top
  // bar, branch drops, white-violet core node.
  for (int y = 14; y <= 19; ++y) b.set(16, y, 17, kGlowPurple);
  b.box(14, 19, 17, 18, 19, 17, kGlowPurple);
  b.set(14, 18, 17, kGlowPurple);
  b.set(18, 18, 17, kGlowPurple);
  b.set(16, 16, 17, kGlowCore);
  // Back panel (disc mounting area).
  b.box(13, 15, 13, 19, 21, 13, kSuitPanel);

  // Arms (y 11..21), hanging at the sides: left x8-10, right x21-23.
  for (auto [armX, outerX] : {std::pair{8, 8}, std::pair{21, 23}}) {
    b.box(armX, 12, 14, armX + 2, 21, 16, kSuitMidnight);
    b.box(armX, 20, 14, armX + 2, 21, 16, kSuitPanel);
    b.box(armX, 11, 14, armX + 2, 11, 16, kTrimDark);  // glove
    for (int y = 13; y <= 19; ++y) b.set(outerX, y, 15, kGlowOrange);
  }

  // Neck + head (y 22..29).
  b.box(15, 22, 15, 16, 22, 16, kSkin);
  b.box(13, 23, 13, 19, 28, 19, kSkin);
  // Eyes: glow purple on the front face z=19.
  b.set(14, 26, 19, kGlowPurple);
  b.set(18, 26, 19, kGlowPurple);
  // Pointed elf ears.
  b.box(11, 26, 15, 12, 26, 16, kSkin);
  b.set(11, 27, 15, kSkin);
  b.box(20, 26, 15, 21, 26, 16, kSkin);
  b.set(21, 27, 15, kSkin);

  // Hair: long, straight, midnight. Top cap, back panel, long back columns
  // falling either side of where the disc mounts, front side locks, fringe.
  b.box(13, 29, 13, 19, 29, 19, kHairMidnight);
  b.box(13, 23, 13, 19, 29, 13, kHairMidnight);
  b.box(13, 14, 12, 14, 22, 13, kHairMidnight);
  b.box(18, 14, 12, 19, 22, 13, kHairMidnight);
  b.box(12, 23, 17, 12, 27, 18, kHairMidnight);
  b.box(20, 23, 17, 20, 27, 18, kHairMidnight);
  b.box(13, 28, 19, 19, 28, 19, kHairMidnight);

  // Identity disc (back, z=12), drawn last so it mounts OVER the hair:
  // purple glow ring centered (16,19), white-violet core.
  for (int dx = -3; dx <= 3; ++dx)
    for (int dy = -3; dy <= 3; ++dy) {
      const int d2 = dx * dx + dy * dy;
      if (d2 >= 2 && d2 <= 5) b.set(16 + dx, 19 + dy, 12, kGlowPurple);
    }
  b.set(16, 19, 12, kGlowCore);

  return b.finish();
}

const std::vector<AgentModelDef>& allAgentModels() {
  static const std::vector<AgentModelDef> kAll = {
      {"elfie", "Elfie", authorElfie},
  };
  return kAll;
}

const AgentModelDef* findAgentModel(const std::string& name) {
  for (const auto& d : allAgentModels())
    if (name == d.name) return &d;
  return nullptr;
}

litholattice::anim::VoxelGrid toVoxelGrid(const litholattice::procgen::LithoStampModel& m) {
  litholattice::anim::VoxelGrid g;
  g.size = m.size;
  g.cells.reserve(m.cells.size());
  for (const auto& c : m.cells) g.cells.push_back({c.x, c.y, c.z});
  return g;
}

}  // namespace lithokraft::model::agents
