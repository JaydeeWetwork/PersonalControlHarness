# HUD-only honesty

## 1. Credits
Designed by: Herominde.
Built by: Cortana.

The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these test suites on their own, without input from JD; JD then verified them and approved them for posting (and asked for longer rewind runs). JD human-reviews and verifies all AI-authored work. Design: `../designs/herominde-fairplay-testbeds.md`.

## 2. Goal
Blank the energy bar on the HUD while the real value stays full. The boss must act as though energy is unknown: no x-ray of the hidden value.
It runs on the PCH reference boss (`pch/harness/boss.py`), a deliberately minimal, documented stub, not the LithoKraft boss.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_boss_fixture.py hud_only_honesty

Or through CMake: `ctest --preset testbeds-only -R boss.hud_only_honesty` (after `cmake --preset testbeds-only`).
Exit 0 means every check passed.

## 4. Status
**passed** (2026-10-23, on the reference boss stub): 5 seeds: with the bar blanked, real energy 100 and 0 give identical action sequences and no energy-spending moves; control: with the bar shown the boss attacks; 0 leaks for the honest brain, while an x-ray brain is caught reading the blanked value.

## 5. Safety
Imports `pch/harness/boss.py` and `stepstream.py`; builds receipts in memory and writes no files.

## 6. Further experiments and design notes
Next: run the same checks against the LithoKraft boss once it emits PCH receipts (see `../DECISIONS.md`).
