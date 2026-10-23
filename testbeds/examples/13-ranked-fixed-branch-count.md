# Fixed branch count (ranked)

## 1. Credits
Directed by: JaydeeWetwork (direction and dictation). Built by: Cortana.

Designed by: Herominde. The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these experiments independently, with no direction from JD; JD reviewed them (and asked for longer rewind runs). Design: `../designs/herominde-fairplay-testbeds.md`.

## 2. Goal
See the design file for the full goal and pass/fail rules. This one needs a configurable branch-count budget.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_boss_pending.py ranked_fixed_branch_count

Or through CMake: `ctest --preset testbeds-only -R boss.ranked_fixed_branch_count` (after `cmake --preset testbeds-only`).
Today it prints SKIPPED with the missing feature. Once that feature exists, the design's pass/fail rules apply.

## 4. Status
**skipped**: the public PCH has no a configurable branch-count budget yet. Nothing is faked.

## 5. Safety
Read-only use of the harness: it imports `pch/harness/stepstream.py` / `harness.py` and reads the shipped `.jsonl` streams, and writes nothing into `pch/`. PCH core is not modified.

## 6. Further experiments and design notes
A good first contribution: build the missing piece, then replace the skip with the real check.
