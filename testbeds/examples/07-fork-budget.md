# Lookahead fork budget

## 1. Credits
Directed by: JaydeeWetwork (direction and dictation). Built by: Cortana.

Designed by: Cortana. The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these experiments independently, with no direction from JD; JD reviewed them (and asked for longer rewind runs). Design: `../designs/cortana-testbeds.md`.

## 2. Goal
Fork a sealed prefix into 1, 2, 4, 8 and 16 branches over a fixed 32-step horizon. Record wall time, process peak RSS and best-branch score, to see where more branches stop helping on modest hardware.

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

Without torch and the DeepSeek-V3 code it prints SKIPPED and why.

## 5. Safety
Read-only use of the harness: it imports `pch/harness/stepstream.py` / `harness.py` and reads the shipped `.jsonl` streams, and writes nothing into `pch/`. PCH core is not modified.

## 6. Further experiments and design notes
Install `psutil` (free) for the process peak RSS numbers (whole Python process, not just the forks); without it memory shows 0. Compare a 2-core laptop against a bigger desktop.
