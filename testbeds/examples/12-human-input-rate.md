# Human input rate

## 1. Credits
Designed by: Herominde.
Built by: Cortana.

The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these test suites on their own, without input from JD; JD then verified them and approved them for posting (and asked for longer rewind runs). JD human-reviews and verifies all AI-authored work. Design: `../designs/herominde-fairplay-testbeds.md`.

## 2. Goal
No burst of boss actions may exceed a human rate cap: at most 10 non-wait actions in any 60-tick (1 s) window. The cap is recorded in the hashed receipt header. Inventory-swap timing (also in Herominde #4) is not modeled by the stub.
It runs on the PCH reference boss (`pch/harness/boss.py`), a deliberately minimal, documented stub, not the LithoKraft boss.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_boss_fixture.py human_input_rate

Or through CMake: `ctest --preset testbeds-only -R boss.human_input_rate` (after `cmake --preset testbeds-only`).
Exit 0 means every check passed.

## 4. Status
**passed** (2026-10-23, on the reference boss stub): for 5 seeds x brain v1/v2 the largest burst is <= 10 actions per 60 ticks; the negative control (cap off) bursts 58 actions, so the check has teeth; capped actions are recorded in the receipt.

## 5. Safety
Imports `pch/harness/boss.py` and `stepstream.py`; builds receipts in memory and writes no files.

## 6. Further experiments and design notes
Next: run the same checks against the LithoKraft boss once it emits PCH receipts (see `../DECISIONS.md`).
