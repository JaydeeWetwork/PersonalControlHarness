# Telegraph fairness

## 1. Credits
Designed by: Master Chief and Herominde.
Built by: Cortana.

The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these test suites on their own, without input from JD; JD then verified them and approved them for posting (and asked for longer rewind runs). JD human-reviews and verifies all AI-authored work. Design: `../designs/masterchief-02-telegraph-fairness-testbed.md`.

## 2. Goal
Every attack shows a wind-up and a HUD energy drop early enough to react: lead time minus the reaction window (250 ms + input lag: hard 15 ticks, easy 21 ticks) must be >= 0, no attack may lack a cue, and wind-up length may vary by at most 1 tick across forked branches.
It runs on the PCH reference boss (`pch/harness/boss.py`), a deliberately minimal, documented stub, not the LithoKraft boss.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_boss_fixture.py telegraph_fairness

Or through CMake: `ctest --preset testbeds-only -R boss.telegraph_fairness` (after `cmake --preset testbeds-only`).
Exit 0 means every check passed.

## 4. Status
**passed** (2026-10-23, on the reference boss stub): 524 attacks over 5 scenarios x 5 seeds; minimum fairness margin 7 ticks (hard tier) and 1 tick (easy tier); 0 unreadable attacks; strike 22 and heavy 32 ticks in every forked branch; negative control: a 3-tick heavy wind-up is caught.

## 5. Safety
Imports `pch/harness/boss.py` and `stepstream.py`; builds receipts in memory and writes no files.

## 6. Further experiments and design notes
Next: run the same checks against the LithoKraft boss once it emits PCH receipts (see `../DECISIONS.md`).
