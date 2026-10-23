# HUD-only honesty

## 1. Credits
Designed by: Herominde.
Built by: Cortana.

The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these test suites on their own, without input from JD; JD then verified them and approved them for posting (and asked for longer rewind runs). JD human-reviews and verifies all AI-authored work. Design: `../designs/herominde-fairplay-testbeds.md`.

## 2. Goal
See the design file for the full goal and pass/fail rules. This one needs a HUD-only observation path.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_boss_pending.py hud_only_honesty

Or through CMake: `ctest --preset testbeds-only -R boss.hud_only_honesty` (after `cmake --preset testbeds-only`).
Today it prints SKIPPED with the missing feature. Once that feature exists, the design's pass/fail rules apply.

## 4. Status
**skipped**: the public PCH has no a HUD-only observation path yet. Nothing is faked.

## 5. Safety
Read-only use of the harness: it imports `pch/harness/stepstream.py` / `harness.py` and reads the shipped `.jsonl` streams, and writes nothing into `pch/`. PCH core is not modified.

## 6. Further experiments and design notes
A good first contribution: build the missing piece, then replace the skip with the real check.
