# Lookahead fork budget

## 1. Credits
Designed by: Cortana.
Built by: Cortana.

The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these test suites on their own, without input from JD; JD then verified them and approved them for posting (and asked for longer rewind runs). JD human-reviews and verifies all AI-authored work. Design: `../designs/cortana-testbeds.md`.

## 2. Goal
Fork a sealed prefix into 1, 2, 4, 8, 16 and 32 branches over a 32-step horizon, then 4 and 16 branches over the 240-step horizon (the toy fixture's ceiling). Record wall time, process peak RSS and best-branch score, to see where more branches stop helping on modest hardware.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_model_testbeds.py fork_budget

Or through CMake: `ctest --preset testbeds-only -R fork_budget` (after `cmake --preset testbeds-only`).
It's a measurement. Pass means every branch kept the sealed prefix, and the table is the result. Fail: a branch altered the prefix.

## 4. Status
**verified** 2026-10-23 (Windows 11 workstation, Python 3.14.8, torch 2.14.1+cpu, DeepSeek-V3 `9b4e978`, psutil 7.2.2): every branch count kept the sealed prefix (CTest 10-12 s).

| Branches | Wall | Peak RSS | Best score |
|---|---|---|---|
| 1 | 0.12 s | 204.7 MiB | -6.3094 |
| 2 | 0.23 s | 206.9 MiB | -6.0950 |
| 4 | 0.45 s | 207.0 MiB | -5.9272 |
| 8 | 0.97 s | 207.7 MiB | -5.7986 |
| 16 | 1.83 s | 207.9 MiB | -5.7986 |

Round 2 (2026-10-23, same workstation), at the fixture ceiling:

| Branches | Horizon | Wall | Peak RSS | Best score |
|---|---|---|---|---|
| 32 | 32 | 3.07 s | 206.9 MiB | -5.7986 |
| 4 | 240 | 2.42 s | 205.4 MiB | -5.9455 |
| 16 | 240 | 9.50 s | 206.1 MiB | -5.9104 |

**Ceiling:** the horizon cannot exceed 240 steps (toy `max_seq_len` 256, 8-token prompt, 8 sealed steps). Longer horizons need a longer-context fixture; the real-model run waits for the specimen.

Without torch and the DeepSeek-V3 code it prints SKIPPED and why.

## 5. Safety
Read-only use of the harness: it imports `pch/harness/stepstream.py` / `harness.py` and reads the shipped `.jsonl` streams, and writes nothing into `pch/`. PCH core is not modified.

## 6. Further experiments and design notes
Install `psutil` (free) for the process peak RSS numbers (whole Python process, not just the forks); without it memory shows 0. Compare a 2-core laptop against a bigger desktop.
