// The game's dimensions: FOUR are registered — Wafer (silicon flats +
// demo circuits), Organic, Cosmic and Underwafer (the last two added
// B5 2026-10-13 with crust-level content only; see below). Their
// generator files live in assets/worldgen/ (the game pack, so resource
// packs can replace them) and are also embedded in the exe as the
// built-in fallback.
//
// DIMENSION LEDGER (P2c 2026-10-13, review G-M5; updated B5 2026-10-13):
// the locked design has five core dimensions. Cosmic and Underwafer now
// have worldgen JSONs, generators, registration and spawns, generated
// honestly from the existing stage vocabulary (heightmap terrain + the
// worm-cave carver) -- but only at crust level. Still missing, per
// dimension, before either is its designed self:
//  - cosmic: the Empty as a named medium (worldgen JSON has no medium
//    field; air stands in above the crust, which stays inside the
//    design's lower-256 solid band of the fixed 768 height), space
//    traversal (magnetic boots / free descent into Organic), satellite
//    content (Recons, Lattice Bridge), progression gating behind the
//    Underwafer — none of which exists as stages or game systems.
//  - underwafer: the Sear, hypervisor ore depth progression, a ceiling
//    structure, and its portal link from the Wafer — no stages or
//    content for any of it exist; today's JSON is a deep-stone floor
//    with worm caves.
//  - void: STILL NOT REGISTERED. Infinite extent (the registry/engine
//    dimension model is a fixed 768-tall ChunkMap with game-side bounds
//    only by convention), infohazard / Hypervisor mechanics, and no
//    terrain content at all — an "empty air" registration would be a
//    falling-forever trap, not the Void. See the packPos analysis below.
// The MCP pilot numbering in
// mcp_player_control.cpp (underwafer=2, cosmic=4, void=5, ether=6,
// undefined=7) is forward-looking addressing only; those dimensions
// cannot be entered until they exist here.
//
// DIMENSION BOUNDS vs packPos (P3-2 B-L2): packPos (world/chunk.h) packs
// a cell into 21 bits per horizontal axis with a +2^20 bias, so every
// registered dimension MUST declare bounds inside [-2^20, 2^20-1] blocks
// on x/z (and y in [0, 1023]); outside that range packPos keys alias and
// unrelated cells silently share signal/light/dirty state. The locked
// bounds all fit with an order of magnitude to spare (Underwafer
// +/-131072, Organic +/-196608, Cosmic +/-262144 blocks). THE VOID IS
// THE AT-RISK DESIGN: its locked extent is INFINITE, which no packPos
// key can represent — before the Void is registered it must be given a
// finite bound inside the range above, or a windowed/virtualised
// coordinate scheme that never feeds unbounded coordinates to packPos.
// Enforcement today: ChunkStreamer refuses out-of-range request centres
// loudly at setup/stream time (chunk_streamer.cpp, B-L2).
#pragma once
#include "lithokraft/game/engine.h"
#include <string_view>
#include "litholattice/world/column_generator.h"
#include "litholattice/world/dimension.h"

namespace lithokraft {

/// "wafer" + "organic" ("overworld" is an alias) + "underwafer" +
/// "cosmic", generated from `settings` (pack JSON, else the built-in
/// generators). The Void is deliberately absent — see the header ledger.
DimensionRegistry createDimensions(const WorldgenSettings& settings = {});

/// Built-in generator JSON for "wafer" / "organic" (assets/worldgen/<id>.json), or nullptr.
const char* builtinWorldgenSource(std::string_view dimensionId);

}  // namespace lithokraft
