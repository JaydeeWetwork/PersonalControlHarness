// First authored models for LithoKraft's sub-block subdivision pipeline.
// Workstream 5 (2026-10-11): the armory (6 tools), the trace tree, TurtlOR.
// Authored in code through lithokraft::model::ModelBuilder (workstream 3's
// format), saved as versioned .lthm files under assets/models/.
//
// Materials are opaque uint8 palette indices in the .lthm format ("mesher
// passthrough"); the palette below is workstream 5's authoring palette and
// the color binding lives with the renderer. Ids are stable: never renumber.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "model.h"

namespace lithokraft::model::authored {

namespace palette {
// Stable authoring-palette ids (see file header).
inline constexpr uint8_t kHandleWood = 1;  // tool handles: dark warm brown
inline constexpr uint8_t kHeadMetal = 2;   // tool heads: steel gray
inline constexpr uint8_t kBladeEdge = 3;   // cutting edges: bright silver
inline constexpr uint8_t kCopper = 4;      // via-trunk copper
inline constexpr uint8_t kTraceRed = 5;    // glowing red trace
inline constexpr uint8_t kTraceBlue = 6;   // glowing blue trace
inline constexpr uint8_t kShellDark = 7;   // TurtlOR shell plating / substrate
inline constexpr uint8_t kShellPlate = 8;  // plastron / lighter shell panels
inline constexpr uint8_t kSeamRed = 9;     // shell seam glow: red
inline constexpr uint8_t kSeamBlue = 10;   // shell seam glow: blue
inline constexpr uint8_t kCrystalBlue = 11;  // dopant crystal-tree: blue facet
inline constexpr uint8_t kCrystalAmber = 12;  // dopant crystal-tree: amber facet
inline constexpr uint8_t kCrystalCore = 13;  // dopant crystal-tree: white-hot apex

const char* name(uint8_t material);  // human-readable palette name
}  // namespace palette

// The six armory tools (JD-locked tool identities, 2026-10-11).
Model authorBashor();  // hammer: chunky head, bash-prompt trace inlay
Model authorPickor();  // pickaxe: twin points, blue-hot tips
Model authorChopor();  // axe: broad flared blade, zigzag inlay
Model authorHeapor();  // shovel: cupped scoop, glowing rim
Model authorSeedor();  // hoe: angled tines, floating procgen seeds
Model authorSlicor();  // sword: long blade, glowing fuller, gem pommel
// The rest of the lore item list (items workstream, 2026-10-12): the LOCKED
// weapons/special items plus the Fragment starting book, Sniffor and Atlas.
Model authorFirewall();  // shield: broad plate, trace-cross inlay
Model authorPing();      // bow: curved limbs, packet-notch string
Model authorCallback();  // thrown: trident head, return ring on the shaft
Model authorDebuggor();  // debug stick: wand with a rotation ring + dial
Model authorFragment();  // the Fragment: bound book, trace glyph on the cover
Model authorSniffor();   // handheld signal receiver: dish + antenna + screen
Model authorAtlas();     // atlas: framed map tablet, route-line inlay
// The hero organic: via-trunk silicon tree, forking trace branches,
// red/blue glowing tips (matches the trace-forest concept art).
Model authorTraceTree();
// The turtle: domed faceted Faraday-cage shell, head, four legs,
// and the smaller turtle riding on its back (matches concept art).
Model authorTurtlor();

struct AuthoredModel {
  const char* fileStem;  // e.g. "bashor" -> assets/models/bashor.lthm
  const char* displayName;
  Model (*author)();
};

// All authored models, in a fixed order (append-only).
const std::vector<AuthoredModel>& allAuthoredModels();

}  // namespace lithokraft::model::authored
