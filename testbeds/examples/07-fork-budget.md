# Lookahead fork budget

## 1. Designed by
Cortana. The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these experiments independently, with no direction from JD; JD reviewed them (and asked for longer rewind runs). Design: `../designs/cortana-testbeds.md`.

## 2. Goal
Fork a sealed prefix into 1, 2, 4, 8 and 16 branches over a fixed 32-step horizon. Record wall time, peak memory and best-branch score, to see where more branches stop helping on modest hardware.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_model_testbeds.py fork_budget

Or through CMake: `ctest --preset testbeds-only -R fork_budget` (after `cmake --preset testbeds-only`).
It's a measurement. Pass means every branch kept the sealed prefix, and the table is the result. Fail: a branch altered the prefix.

## 4. Status
**skipped** on the reference workstation: needs CPU PyTorch and the DeepSeek-V3 reference code (free; see `pch/README.md`), which are not installed there. Without them it prints SKIPPED and why.

## 5. Safety
Read-only use of the harness: it imports `pch/harness/stepstream.py` / `harness.py` and reads the shipped `.jsonl` streams, and writes nothing into `pch/`. PCH core is not modified.

## 6. Further experiments and design notes
Install `psutil` (free) for peak-memory numbers; without it memory shows 0. Compare a 2-core laptop against a bigger desktop.
