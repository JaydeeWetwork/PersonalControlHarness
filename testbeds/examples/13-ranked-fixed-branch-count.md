# Fixed branch count (ranked)

## 1. Credits
Designed by: Herominde.
Built by: Cortana.

The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these test suites on their own, without input from JD; JD then verified them and approved them for posting (and asked for longer rewind runs). JD human-reviews and verifies all AI-authored work. Design: `../designs/herominde-fairplay-testbeds.md`.

## 2. Goal
In ranked mode the branch count is locked, so a "phone" budget (1 worker) and a "PC" budget (8 workers) must give identical decisions and an identical receipt hash.
It runs on the PCH reference boss (`pch/harness/boss.py`), a deliberately minimal, documented stub, not the LithoKraft boss.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_boss_fixture.py ranked_fixed_branch_count

Or through CMake: `ctest --preset testbeds-only -R boss.ranked_fixed_branch_count` (after `cmake --preset testbeds-only`).
Exit 0 means every check passed.

## 4. Status
**passed** (2026-10-23, on the reference boss stub): 5 seeds x brain v1/v2: ranked phone and PC receipts are hash-identical with the branch count locked at 4 and recorded; control: unranked phone (2 branches) vs PC (6 branches) budgets change decisions.

## 5. Safety
Imports `pch/harness/boss.py` and `stepstream.py`; builds receipts in memory and writes no files.

## 6. Further experiments and design notes
Next: run the same checks against the LithoKraft boss once it emits PCH receipts (see `../DECISIONS.md`).
