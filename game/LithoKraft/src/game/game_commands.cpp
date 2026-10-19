// GameCommands: Decision-9 first-party commands. LF line endings.
#include "lithokraft/game/game_commands.h"

#include <cstdint>
#include <string>

#include "litholattice/console/concmd.h"
#include "lithokraft/content/items.h"  // isItemId (give validation, G-L5)
#include "lithokraft/game/map_render.h"

namespace lithokraft {
namespace {

using litholattice::console::CommandFlags;
using litholattice::console::ConsoleArgs;
using litholattice::console::ConsoleRegistry;

uint32_t parseU32(const std::string& s, bool& ok) {
  try {
    // Reject negatives explicitly: stoul("-1") wraps to ULONG_MAX instead of failing.
    if (!s.empty() && s[0] == '-') { ok = false; return 0; }
    size_t n = 0;
    unsigned long v = std::stoul(s, &n);
    ok = n == s.size() && v <= UINT32_MAX;
    return ok ? static_cast<uint32_t>(v) : 0;
  } catch (...) {
    ok = false;
    return 0;
  }
}

int parseInt(const std::string& s, bool& ok) {
  try {
    size_t n = 0;
    long v = std::stol(s, &n);
    ok = n == s.size();
    return int(v);
  } catch (...) {
    ok = false;
    return 0;
  }
}

}  // namespace

void registerGameCommands(ConsoleRegistry& r, GameCommandBindings b) {
  CommandFlags cheat;
  cheat.cheat = true;
  // Session tier (P1b, 2026-10-13, Appendix F M6): quit/save/map are not
  // cheats (the local player uses them in ordinary play), but they are
  // session/destructive — restricted callers (MCP agents, mods) must not
  // reach them. The full-authority local player keeps access, cheats or not.
  CommandFlags session;
  session.session = true;
  std::string err;

  // give <itemId> [count] — cheat.
  r.registerCommand(
      "give", "give <itemId> [count] - add items to the player inventory (cheat)",
      [b](ConsoleRegistry& r, const ConsoleArgs& a) {
        if (a.size() < 2 || a.size() > 3) {
          r.print("usage: give <itemId> [count]");
          return;
        }
        bool ok = false;
        const uint32_t id = parseU32(a[1], ok);
        uint32_t count = 1;
        if (a.size() == 3) count = parseU32(a[2], ok);
        // Item id 0 is the empty sentinel: giving it silently eats the items.
        // Unknown ids are rejected too (P2b-2, review G-L5): a stack with no
        // def, icon or name must never enter the inventory.
        if (!ok || id == 0 || count == 0 || count > 65535 || !isItemId(id)) {
          r.print("usage: give <itemId> [count]  (id > 0, count 1-65535)");
          return;
        }
        r.print(b.give(id, static_cast<uint16_t>(count)));
      },
      cheat, &err);

  // kill — cheat; respawn until the health system lands.
  r.registerCommand(
      "kill", "kill - respawn the player at spawn (cheat; provisional)",
      [b](ConsoleRegistry& r, const ConsoleArgs&) { r.print(b.kill()); }, cheat,
      &err);

  // fly [on|off] — cheat; toggles the player's locomotion Fly mode. The
  // free camera (`camera free`) is a camera, not the player, and is not
  // touched by this.
  r.registerCommand(
      "fly", "fly [on|off] - toggle player flight (locomotion Fly mode; cheat)",
      [b](ConsoleRegistry& r, const ConsoleArgs& a) {
        if (a.size() > 2) {
          r.print("usage: fly [on|off]");
          return;
        }
        r.print(b.flyToggle(a.size() == 2 ? a[1] : ""));
      },
      cheat, &err);

  // etch <name...> — the EtchOR flow (wiring pass B6): consume one blank
  // wafer, etch a ChipOR bearing the name-field value. Not a cheat (it
  // transforms owned items), but session-tier: it mutates the inventory,
  // so restricted callers cannot run it. The name is every argument after
  // the verb, joined by single spaces (a name field is one line).
  r.registerCommand(
      "etch", "etch <name...> - etch a ChipOR from a blank wafer, bearing the name field",
      [b](ConsoleRegistry& r, const ConsoleArgs& a) {
        if (a.size() < 2) {
          r.print("usage: etch <name...>");
          return;
        }
        std::string name = a[1];
        for (size_t i = 2; i < a.size(); ++i) name += " " + a[i];
        r.print(b.etchChip(name));
      },
      session, &err);

  // sign <certificate> — certificate-table signing (wiring pass B7):
  // spend static levels, apply the certificate to the selected hotbar
  // stack. Session-tier: it spends a player resource.
  r.registerCommand(
      "sign", "sign <certificate> - sign the selected hotbar item (spends static levels; see: certificates)",
      [b](ConsoleRegistry& r, const ConsoleArgs& a) {
        if (a.size() != 2) {
          r.print("usage: sign <certificate>  (see: certificates)");
          return;
        }
        r.print(b.signCertificate(a[1]));
      },
      session, &err);

  // deceitor <spawn|interact|attack> — Deceitor mob test paths
  // (2026-10-14). spawn conjures one in front of the player (needs
  // ai.enabled 1: the mob lives in the AiWorld); interact/attack drive
  // the nearest one — today's stand-ins for the click-picking and
  // mob-combat hooks those entries will use. Cheat: it conjures and
  // provokes mobs from the console.
  r.registerCommand(
      "deceitor", "deceitor <spawn|interact|attack> - Deceitor mob test paths (cheat; needs ai.enabled)",
      [b](ConsoleRegistry& r, const ConsoleArgs& a) {
        if (a.size() != 2) {
          r.print("usage: deceitor <spawn|interact|attack>");
          return;
        }
        if (a[1] == "spawn") r.print(b.spawnDeceitor());
        else if (a[1] == "interact") r.print(b.deceitorInteract());
        else if (a[1] == "attack") r.print(b.deceitorAttack());
        else r.print("usage: deceitor <spawn|interact|attack>");
      },
      cheat, &err);

  // certificates — the registry + the player's standing. Read-only.
  r.registerCommand(
      "certificates", "certificates - list signable certificates and your static level",
      [b](ConsoleRegistry& r, const ConsoleArgs&) { r.print(b.listCertificates()); },
      CommandFlags{}, &err);

  // map [dimension] — not a cheat (navigation, like HL1), but session-tier:
  // switching dimension is a session action restricted callers cannot run.
  r.registerCommand(
      "map", "map [dimension] - switch dimension, or list dimensions",
      [b](ConsoleRegistry& r, const ConsoleArgs& a) {
        if (a.size() > 2) {
          r.print("usage: map [dimension]");
          return;
        }
        if (a.size() == 1) {
          std::string out = "dimensions:";
          for (const auto& id : b.dimensionIds()) out += " " + id;
          r.print(out);
          return;
        }
        r.print(b.map(a[1]));
      },
      session, &err);

  // save — not a cheat, but session-tier (writes the world to disk).
  r.registerCommand("save", "save - save the world now",
                    [b](ConsoleRegistry& r, const ConsoleArgs&) {
                      r.print(b.save());
                    },
                    session, &err);

  // pause — not a cheat.
  r.registerCommand("pause", "pause - toggle pause",
                    [b](ConsoleRegistry& r, const ConsoleArgs&) {
                      r.print(b.pauseToggle());
                    },
                    &err);

  // quit — not a cheat, but session-tier (ends the session).
  r.registerCommand("quit", "quit - quit to desktop",
                    [b](ConsoleRegistry&, const ConsoleArgs&) { b.quit(); }, session, &err);

  // screenshot — not a cheat. Captures the current framebuffer to PNG; the
  // capture lands on the next rendered frame (the command runs during
  // update, before the frame is drawn), so the binding only arms it.
  //
  //   screenshot                        auto-name into screenshots/
  //   screenshot lowres                 auto-name, agent-vision fidelity
  //   screenshot <path> [lowres]        capture to a path
  //   screenshot fragment [lowres] [caption...]
  //                                     capture into the Fragment's
  //                                     screenshots gallery (lore page 5)
  r.registerCommand(
      "screenshot",
      "screenshot [path] [lowres] | screenshot fragment [lowres] [caption...] - "
      "capture the framebuffer to PNG (lowres = mcp.vision_width x mcp.vision_height)",
      [b](ConsoleRegistry& r, const ConsoleArgs& a) {
        if (!b.screenshot) {
          r.print("screenshot: unavailable");
          return;
        }
        if (a.size() < 1 || a.size() > 3) {
          // Fragment mode takes a free-form caption; file mode takes at most
          // a path and the lowres flag.
          const bool fragmentMode = a.size() >= 2 && a[1] == "fragment";
          if (!fragmentMode || a.size() > 18) {
            r.print("usage: screenshot [path] [lowres]");
            r.print("       screenshot fragment [lowres] [caption...]");
            return;
          }
        }
        ScreenshotRequest req;
        if (a.size() >= 2 && a[1] == "fragment") {
          // Fragment gallery mode: caption is the remaining words.
          req.toFragment = true;
          size_t cap = 2;
          if (a.size() >= 3 && a[2] == "lowres") {
            req.lowres = true;
            cap = 3;
          }
          for (size_t i = cap; i < a.size(); ++i) {
            if (i > cap) req.caption += " ";
            req.caption += a[i];
          }
          r.print(b.screenshot(req));
          return;
        }
        if (a.size() == 2) {
          if (a[1] == "lowres")
            req.lowres = true;
          else
            req.path = a[1];
        } else if (a.size() == 3) {
          if (a[2] != "lowres") {
            r.print("usage: screenshot [path] [lowres]");
            return;
          }
          req.path = a[1];
          req.lowres = true;
        }
        r.print(b.screenshot(req));
      },
      &err);

  // locatebiome <biome> — cheat (reveals ungenerated land). Spiral search from
  // the player for the nearest column whose dominant biome matches; the game
  // resolves the name against the active dimension's biome list.
  r.registerCommand(
      "locatebiome", "locatebiome <biome> - find the nearest <biome> column from the player",
      [b](ConsoleRegistry& r, const ConsoleArgs& a) {
        if (!b.locateBiome) {
          r.print("locatebiome: unavailable");
          return;
        }
        if (a.size() != 2) {
          r.print("usage: locatebiome <biome>");
          return;
        }
        r.print(b.locateBiome(a[1]));
      },
      cheat, &err);

  // genmap <path> <center> <zoom> <granularity> — cheat (reveals ungenerated
  // land). Renders a top-down biome/structure overview map to PNG.
  //
  //   genmap out.png player 2 16     1024 blocks around the player, 64x64 px
  //   genmap out.png 1024,-256 6 64  16K blocks around (1024,-256), 256x256 px
  //
  // center is "player" or "x,z"; zoom 0-6 doubles the region from 256 to 16384
  // blocks across; granularity is the required blocks-per-pixel (8/16/32/64).
  r.registerCommand(
      "genmap", "genmap <path> <center> <zoom> <granularity> - render a top-down biome/structure overview map to PNG",
      [b](ConsoleRegistry& r, const ConsoleArgs& a) {
        if (!b.genmap) {
          r.print("genmap: unavailable");
          return;
        }
        if (a.size() != 5) {
          r.print("usage: genmap <path> <center> <zoom> <granularity>");
          r.print("       center: \"player\" or \"x,z\"; zoom: 0-6; granularity: 8, 16, 32, 64 blocks/px");
          return;
        }
        GenMapRequest req;
        req.path = a[1];
        req.center = a[2];
        bool ok = false;
        req.zoom = parseInt(a[3], ok);
        if (!ok || req.zoom < kMapZoomMin || req.zoom > kMapZoomMax) {
          r.print("genmap: zoom must be 0-6");
          return;
        }
        req.granularity = parseInt(a[4], ok);
        if (!ok || !validMapGranularity(req.granularity)) {
          r.print("genmap: granularity must be one of 8, 16, 32, 64 (blocks per pixel)");
          return;
        }
        if (req.center.empty()) {
          r.print("genmap: center must be \"player\" or \"x,z\"");
          return;
        }
        r.print(b.genmap(req));
      },
      cheat, &err);

  // tick freeze — pause the tick engine (creative-mode contraption debugger).
  r.registerCommand(
      "tick_freeze", "tick_freeze - freeze the tick engine (pause all ticks)",
      [b](ConsoleRegistry& r, const ConsoleArgs&) {
        if (!b.tickFreeze) { r.print("tick_freeze: unavailable"); return; }
        r.print(b.tickFreeze());
      },
      cheat, &err);

  // tick unfreeze — resume the tick engine.
  r.registerCommand(
      "tick_unfreeze", "tick_unfreeze - unfreeze the tick engine (resume ticks)",
      [b](ConsoleRegistry& r, const ConsoleArgs&) {
        if (!b.tickUnfreeze) { r.print("tick_unfreeze: unavailable"); return; }
        r.print(b.tickUnfreeze());
      },
      cheat, &err);

  // tick step <n> — advance n ticks while frozen (tick-by-tick debugger).
  r.registerCommand(
      "tick_step", "tick_step <n> - advance <n> ticks while frozen (default 1)",
      [b](ConsoleRegistry& r, const ConsoleArgs& a) {
        if (!b.tickStep) { r.print("tick_step: unavailable"); return; }
        uint64_t n = 1;
        if (a.size() >= 2) {
          bool ok = false;
          unsigned long v = 0;
          try { size_t p = 0; v = std::stoul(a[1], &p); ok = p == a[1].size(); } catch (...) {}
          if (!ok || v == 0) { r.print("usage: tick_step <n>  (n >= 1)"); return; }
          n = v;
        }
        r.print(b.tickStep(n));
      },
      cheat, &err);

#ifndef NDEBUG
  // hotswap_refresh — debug builds only (not registered in release). Applies
  // staged block-definition updates (ContentRegistry::refreshStaged) between
  // sim ticks via the binding. Cheat: mutates content at runtime.
  r.registerCommand(
      "hotswap_refresh", "hotswap_refresh - apply staged block-definition updates (debug only; cheat)",
      [b](ConsoleRegistry& r, const ConsoleArgs&) {
        if (!b.hotswapRefresh) { r.print("hotswap_refresh: unavailable"); return; }
        r.print(b.hotswapRefresh());
      },
      cheat, &err);
#endif
}

}  // namespace lithokraft
