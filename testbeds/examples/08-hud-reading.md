# HUD reading

## 1. Credits
Designed by: Master Chief.
Built by: Cortana.

The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these test suites on their own, without input from JD; JD then verified them and approved them for posting (and asked for longer rewind runs). JD human-reviews and verifies all AI-authored work. Design: `../designs/masterchief-01-hud-reading-testbed.md`.

## 2. Goal
The boss brain receives only a HUD snapshot (quantized health/energy bars, potion count, distance, its own wind-up) and acts only through on-screen controls. Same-HUD hidden-state variants must never change a decision; a blind/x-ray control brain must be caught.
It runs on the PCH reference boss (`pch/harness/boss.py`), a deliberately minimal, documented stub, not the LithoKraft boss.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_boss_fixture.py hud_reading

Or through CMake: `ctest --preset testbeds-only -R boss.hud_reading` (after `cmake --preset testbeds-only`).
Exit 0 means every check passed.

## 4. Status
**passed** (2026-10-23, on the reference boss stub): 5 scenarios (low health, empty/full energy, item/empty inventory): decision match rate 100% (>= 90% required), 100% of actions through on-screen controls, 0 hidden-state leaks over same-HUD variants; negative control: an x-ray brain reading exact hidden energy is caught leaking. Note: the brain and the match rule share the same simple policy, so the match rate mainly guards against regressions; the leak test is the real check.

## 5. Safety
Imports `pch/harness/boss.py` and `stepstream.py`; builds receipts in memory and writes no files.

## 6. Further experiments and design notes
Next: run the same checks against the LithoKraft boss once it emits PCH receipts (see `../DECISIONS.md`).
