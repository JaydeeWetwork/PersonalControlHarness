// GameCommands: first-party lithoscript commands for LithoKraft (Decision 9,
// JD-locked 2026-10-11). The console module owns the language; these are the
// provisional first-party population for JD to review (language/population
// split, locked). Names are plain verbs; cheat-gating follows Decision 8.
//
//   give <itemId> [count]  — add items to the player inventory (cheat)
//   kill                   — respawn the player at spawn (cheat; provisional
//                            until the health/damage system lands)
//   fly [on|off]           — toggle the player's locomotion Fly mode
//                            (cheat, like kill; the free CAMERA is a
//                            different thing and is not affected)
//   map [dimension]        — switch dimension, or list dimensions
//                            (session tier, P1b: not a cheat, but restricted
//                            callers — MCP agents, mods — cannot run it)
//   save                   — save the world now (session tier, P1b)
//   pause                  — toggle pause
//   quit                   — quit to desktop (session tier, P1b)
//   screenshot [path] [lowres]
//                          — capture the framebuffer to PNG (auto-names into
//                            screenshots/ when no path is given); lowres
//                            downscales to mcp.vision_width x mcp.vision_height.
//                            Paths are confined to the screenshots directory
//                            by Game::requestScreenshot (P1b): absolute and
//                            ".." paths are refused and fall back to the
//                            confined auto-name
//   screenshot fragment [lowres] [caption]
//                          — capture into the Fragment's screenshots gallery
//                            (Fragment lore page 5) instead of a file
//   locatebiome <biome>    — spiral-search from the player for the nearest
//                            column of the biome (cheat: reveals ungenerated land)
//   genmap <path> <center> <zoom> <granularity>
//                          — render a top-down biome/structure overview map to
//                            PNG (cheat); center is "player" or "x,z", zoom 0-6
//                            (256 to 16384 blocks across), granularity one of
//                            8, 16, 32, 64 blocks per pixel
//   hotswap_refresh        — apply staged block-definition updates via the
//                            hot-swap system (debug builds only; cheat)
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "lithokraft/game/screenshot.h"  // ScreenshotRequest

namespace litholattice::console {
class ConsoleRegistry;
}  // namespace litholattice::console

namespace lithokraft {

/// What a `genmap` lithoscript call asked for (parsed by the command shim in
/// game_commands.cpp; executed by the game). `center` is "player" or "x,z".
struct GenMapRequest {
  std::string path;
  std::string center = "player";
  int zoom = 2;
  int granularity = 16;
};

/// Bindings the game fills in; the commands stay thin shims over them.
struct GameCommandBindings {
  /// Add `count` of item `itemId` to the player inventory. Returns a message.
  std::function<std::string(uint32_t itemId, uint16_t count)> give;
  /// Respawn the player at spawn. Returns a message.
  std::function<std::string()> kill;
  /// Toggle the player's locomotion mode (Walk <-> Fly), or set it when
  /// `arg` is "on"/"off". Returns a message naming the new mode.
  std::function<std::string(const std::string& arg)> flyToggle;
  /// Switch to dimension `id` (empty = list). Returns a message.
  std::function<std::string(const std::string& id)> map;
  /// All known dimension ids, for `map` with no args.
  std::function<std::vector<std::string>()> dimensionIds;
  /// Save the world now. Returns a message.
  std::function<std::string()> save;
  /// Toggle pause. Returns the new state message.
  std::function<std::string()> pauseToggle;
  /// Freeze the tick engine (pause). Returns a message.
  std::function<std::string()> tickFreeze;
  /// Unfreeze the tick engine. Returns a message.
  std::function<std::string()> tickUnfreeze;
  /// Step N ticks while frozen (tick debugger). Returns a message.
  std::function<std::string(uint64_t n)> tickStep;
  /// Quit the game.
  std::function<void()> quit;
  /// Take a screenshot per `req`. Returns a message for the console. The
  /// capture itself lands on the next rendered frame (see
  /// Game::capturePendingScreenshot); the message says where it is going.
  std::function<std::string(const ScreenshotRequest& req)> screenshot;
  /// Find the nearest column of biome `biomeName` (a name from the active
  /// dimension's biome list). Returns the report message.
  std::function<std::string(const std::string& biomeName)> locateBiome;
  /// Render an overview map per `req` and write the PNG. Returns the message.
  std::function<std::string(const GenMapRequest& req)> genmap;
  /// Apply staged block-definition updates via SimThread::runBetweenTicks.
  /// Debug-only (the command registers #ifndef NDEBUG). Returns a message.
  std::function<std::string()> hotswapRefresh;
  /// Etch one ChipOR from a blank wafer, bearing `name` as its name field
  /// (wiring pass B6; the EtchOR's flow, game/etching.cpp). Returns a message.
  std::function<std::string(const std::string& name)> etchChip;
  /// Sign the selected hotbar stack with certificate `certId`, spending
  /// static levels (wiring pass B7; game/signing.cpp). Returns a message.
  std::function<std::string(const std::string& certId)> signCertificate;
  /// List the certificate registry with the player's level/balance (B7).
  std::function<std::string()> listCertificates;
  /// Spawn a Deceitor in front of the player (Deceitor work, 2026-10-14;
  /// needs ai.enabled — the mob lives in the AiWorld). Cheat: it
  /// conjures a mob. Returns a message.
  std::function<std::string()> spawnDeceitor;
  /// Interact with the nearest Deceitor: opens NO trade interface (the
  /// tell) and starts its failed-click grace window. Cheat-tier test
  /// path for the future click-picking hook. Returns a message.
  std::function<std::string()> deceitorInteract;
  /// Attack the nearest Deceitor: pack retaliation (nearby Deceitors
  /// turn hostile together). Cheat-tier test path for the future
  /// mob-combat hook. Returns a message.
  std::function<std::string()> deceitorAttack;
};

/// Registers the Decision-9 commands on `r`. give/kill are cheat-flagged;
/// hotswap_refresh is debug-only (#ifndef NDEBUG) and cheat-flagged.
void registerGameCommands(litholattice::console::ConsoleRegistry& r,
                          GameCommandBindings b);

}  // namespace lithokraft
