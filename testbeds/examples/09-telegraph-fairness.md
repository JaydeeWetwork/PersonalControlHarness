# Telegraph fairness

## 1. Designed by
Master Chief and Herominde. The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these experiments independently, with no direction from JD; JD reviewed them (and asked for longer rewind runs). Design: `../designs/masterchief-02-telegraph-fairness-testbed.md`.

## 2. Goal
See the design file for the full goal and pass/fail rules. This one needs boss attacks with wind-up and HUD timing.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_boss_pending.py telegraph_fairness

Or through CMake: `ctest --preset testbeds-only -R boss.telegraph_fairness` (after `cmake --preset testbeds-only`).
Today it prints SKIPPED with the missing feature. Once that feature exists, the design's pass/fail rules apply.

## 4. Status
**skipped**: the public PCH has no boss attacks with wind-up and HUD timing yet. Nothing is faked.

## 5. Safety
Read-only use of the harness: it imports `pch/harness/stepstream.py` / `harness.py` and reads the shipped `.jsonl` streams, and writes nothing into `pch/`. PCH core is not modified.

## 6. Further experiments and design notes
A good first contribution: build the missing piece, then replace the skip with the real check.
