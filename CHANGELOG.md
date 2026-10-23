# Changelog

Versions use a four-place rolling counter (`a.b.c.d`).

## 0.0.0.3-alpha (2026-10-23)

**Alpha.** Credits release; no code or test changes.

- Includes both credit updates: PCH originally designed by Elfie Muse AI,
  with credit to the Muse team (the team that created the system Elfie is
  built on); and "Timing handshake, LithoLattice engine and LithoKraft game:
  original algorithms designed by JaydeeWetwork (open source under MIT)."
  The second line was committed after the 0.0.0.2-alpha tag, so this is the
  first tag carrying both.

## 0.0.0.2-alpha (2026-10-23)

**Alpha.** Credits correction only; no code or test changes.

- PCH originally designed by Elfie Muse AI; credit added for Elfie Muse AI
  and the Muse team (the team that created the system Elfie is built on)
  in README, CREDITS.md and CITATION.cff.
- Credit line added in README and CREDITS.md: "Timing handshake, LithoLattice engine and LithoKraft game: original algorithms designed by JaydeeWetwork (open source under MIT)."

## 0.0.0.1-alpha (2026-10-23)

**Alpha.** First tagged pre-release. Interfaces, file formats and results may
change without notice; not for production use.

- **Test beds:** fourteen PCH test beds (`testbeds/`), each designed
  independently by a squad member (Cortana, CloudAtlas, Herominde,
  Master Chief) and built by Cortana. 01-07 ran and passed (06 only at
  N = 1 and 64; larger N exceeds the toy model's 248-step context).
  08-14 target boss features that are not built yet and are not run.
- **Build:** CMake presets for Visual Studio 2026 (`vs2026`, `vs2026-release`),
  usable from VS Code, Visual Studio or the command line with free tools.
- **Tests:** full `ctest` run: 65/65 with 0 failed (56 passed, 9 skipped).
  Skips: `engine_storage` and `engine_assets` need symlink rights
  (Windows Developer Mode); the seven `pch.boss.*` tests await unbuilt
  features.
- **docs/media:** real screen captures of the test beds in a terminal and
  of LithoKraft (Debug build) as MP4 and GIF.
- **Docs and credits:** landing-page README, per-test-bed credits
  ("Designed by" / "Built by: Cortana"), CREDITS.md and the DeepSeek-V3
  acknowledgement. (Credit for Elfie Muse AI as the PCH's original
  designer, and for the Muse team, was added in 0.0.0.2-alpha.)

Known issues: the core receipt trailer-hash gap is documented and not yet
closed; the LithoKraft clip shows a Debug build because the Release build
refuses to boot with the retired test key.
