# Versioned receipts and rematch

## 1. Credits
Designed by: Master Chief.
Built by: Cortana.

The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these test suites on their own, without input from JD; JD then verified them and approved them for posting (and asked for longer rewind runs). JD human-reviews and verifies all AI-authored work. Design: `../designs/masterchief-03-versioned-receipt-rematch-testbed.md`.

## 2. Goal
Same seed and same brain version must give the same receipt at every worker count; a v1 rematch after v2 exists must match the v1 original; every receipt names its brain version; the rolling receipt counter must be gap-free.
It runs on the PCH reference boss (`pch/harness/boss.py`), a deliberately minimal, documented stub, not the LithoKraft boss.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_boss_fixture.py versioned_rematch

Or through CMake: `ctest --preset testbeds-only -R boss.versioned_rematch` (after `cmake --preset testbeds-only`).
Exit 0 means every check passed.

## 4. Status
**passed** (2026-10-23, on the reference boss stub): 5 seeds x brain v1/v2 x 1, 2, 4 and all workers give identical decisions; v1 rematches are hash-identical to the originals; the counter check is gap-free and catches a planted gap and repeat.

## 5. Safety
Imports `pch/harness/boss.py` and `stepstream.py`; builds receipts in memory and writes no files.

## 6. Further experiments and design notes
Next: run the same checks against the LithoKraft boss once it emits PCH receipts (see `../DECISIONS.md`).
