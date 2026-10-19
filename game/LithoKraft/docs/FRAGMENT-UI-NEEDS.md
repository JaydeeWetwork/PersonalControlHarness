# Fragment UI — needs list (G-M6, documented 2026-10-13, review Appendix G M6)

Status: **the Fragment has no player-facing UI.** This document is the
complete remainder, written for JD's decision — nothing here was half-built
in the P3-4 fix pack, per the pack brief.

## What already exists and works (verified, selftest-covered)

- The Fragment item: registered, granted on day one in hotbar slot 0
  (`grantStartingItems`, idempotent), persisted with the inventory
  (player.dat v4), rendered in the HUD with its own icon and name.
- `FragmentState` (game/fragment_state.h): tier (starts 0, +1 per newly
  applied upgrade), markdown-unlock flag, `upgradeFragment()` as the single
  upgrade entry point, LFS1 serialize/deserialize (versioned, fail-closed).
- The markdown renderer (`fragment_markdown.*`): takes a plain
  `bool markdownEnabled` and renders, covered by its selftest.
- `FragmentGallery` (game/fragment_gallery.h): screenshot capture into an
  in-memory gallery (wired once, in Game's screenshot path).

## What is missing — the needs list

1. **A Fragment book dialog in GameUi.** No dialog/page exists in
   game_ui.cpp/.h. Needs: an open/close flow (which key/action opens it —
   using the Fragment item from the hotbar is the natural verb, but no
   per-item "use" dispatch exists yet, see item 4), pages for the journal /
   unlock log / recipes / chip archive the header lore describes, and the
   tier display ("displayed on the Fragment" per the design).
2. **FragmentState has zero production callers.** Nothing in Game owns an
   instance. Needs: Game (or Player) owns one, `upgradeFragment()` gets a
   real caller, and the markdown renderer's `markdownEnabled` is fed from
   `markdown_unlocked()`.
3. **The Spark does not exist in code.** The design's upgrade trigger (find
   the Spark, an early-game character/event) is "not yet in code" per the
   header itself. Needs: the Spark encounter/event, or an interim trigger
   JD accepts (console command / certificate-table interaction are the
   cheap candidates) that calls `upgradeFragment()`.
4. **No per-item use dispatch.** Items are registry data + models + icons;
   there is no production handler for "use the selected item" (Appendix G
   H2b: per-tool world-interaction handlers are likewise absent, SeedOR's
   Till included). The Fragment dialog's open verb depends on this seam, or
   on a dedicated key binding that bypasses it.
5. **FragmentState is not persisted.** It has serialize/deserialize but no
   save slot: player.dat carries inventory/stats/vitals, not the Fragment
   blob. Needs: a player.dat block (next version bump) or a field in the
   stats/vitals-style named blocks, plus load-side wiring. Without it, an
   unlocked Fragment re-locks on relaunch.
6. **FragmentGallery has no viewer and no persistence.** Screenshots land
   in an in-memory gallery nothing displays. Needs: a gallery page in the
   book dialog (item 1) and a decision on whether captures persist.
7. **Threading note.** FragmentState is a plain data object with no
   synchronization; its (future) callers must be game-thread only, like the
   rest of the Game-side state. This is now stated in fragment_state.h.

## Decisions JD needs to make

- Open verb: dedicated key, "use selected item", or both?
- Interim upgrade trigger until the Spark exists — or wait for the Spark?
- Persist FragmentState in player.dat (format bump) at the same time as
  the dialog, or dialog first with session-only state?
- Gallery: in-book viewer in v1, or capture-only until later?

Until those land, every doc/README claim should read: the Fragment is the
starting item and its state model + markdown renderer exist and are
selftest-covered; the book cannot yet be opened in game.
