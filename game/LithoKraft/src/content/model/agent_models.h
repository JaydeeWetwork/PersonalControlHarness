// Agent models: voxel figures for players / agents / NPC pilots, authored in
// code the same way the armory models are — but at figure scale (a 32^3
// stamp grid) instead of single-block .lthm micro-cells. An agent model is
// the body everything else hangs off: riggen (engine anim) extracts a
// skeleton + skinning from the shape, the palette byte carries the paint,
// and the game designates one as the default spawn model.
//
// First agent model: "elfie" — the spawn model. Tron-istic attire (JD,
// 2026-10-12): the robes are gone; a midnight-blue circuit suit with glowing
// purple + orange traces, long straight midnight hair, pointed ears, and an
// identity disc mounted on the back over the hair. Figure stands 30 voxels
// tall in the 32 grid (about 2 blocks at the gallery's 16-vox/block res).
//
// Materials are palette bytes, same convention as the authored .lthm
// models: the byte is a stable id, the color binding lives here (authoring
// reference RGB) and with the renderer. Never renumber palette ids.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "litholattice/procgen/generator_loader.h"  // LithoStampModel

namespace litholattice::anim {
struct VoxelGrid;
}  // namespace litholattice::anim

namespace lithokraft::model::agents {

namespace palette {
// Stable agent-palette ids (see file header). Never renumber.
inline constexpr uint8_t kSuitMidnight = 1;  ///< circuit suit base: near-black midnight blue
inline constexpr uint8_t kSuitPanel = 2;     ///< suit panels: lighter navy
inline constexpr uint8_t kGlowPurple = 3;    ///< circuit glow: Laysor purple
inline constexpr uint8_t kGlowOrange = 4;    ///< circuit glow: SelectOR orange
inline constexpr uint8_t kHairMidnight = 5;  ///< long hair: midnight blue
inline constexpr uint8_t kSkin = 6;          ///< pale elf skin
inline constexpr uint8_t kGlowCore = 7;      ///< white-violet glow core (disc center, nodes)
inline constexpr uint8_t kTrimDark = 8;      ///< boots / gloves: darkest trim

const char* name(uint8_t material);  ///< human-readable palette name

struct Rgb {
  uint8_t r, g, b;
};
Rgb color(uint8_t material);  ///< authoring-reference color for palette ids
}  // namespace palette

/// The designated default agent/spawn model name (JD, 2026-10-12).
inline constexpr const char* kDefaultAgentModel = "elfie";

/// Elfie: the first agent model (see file header for the design).
litholattice::procgen::LithoStampModel authorElfie();

struct AgentModelDef {
  const char* name;        ///< lookup name ("elfie")
  const char* displayName; ///< display name ("Elfie")
  litholattice::procgen::LithoStampModel (*author)();
};

/// Every registered agent model, in registration order.
const std::vector<AgentModelDef>& allAgentModels();

/// Look up an agent model by name; nullptr when unknown.
const AgentModelDef* findAgentModel(const std::string& name);

/// Shape-only view of a stamp model for rig extraction (palette ignored,
/// matching riggen's contract).
litholattice::anim::VoxelGrid toVoxelGrid(const litholattice::procgen::LithoStampModel& m);

}  // namespace lithokraft::model::agents
