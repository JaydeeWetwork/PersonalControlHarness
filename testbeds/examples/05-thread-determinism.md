# Thread-count determinism

## 1. Designed by
Herominde. The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these experiments independently, with no direction from JD; JD reviewed them (and asked for longer rewind runs). Design: `../designs/herominde-fairplay-testbeds.md`.

## 2. Goal
Run the same sealed run with 1, 2, 4 and all CPU threads. The final receipt hash must be identical.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_model_testbeds.py thread_determinism

Or through CMake: `ctest --preset testbeds-only -R thread_determinism` (after `cmake --preset testbeds-only`).
Pass: every thread count equals the 1-thread hash. Fail: names the thread count that differed.

## 4. Status
**skipped** on the reference workstation: needs CPU PyTorch and the DeepSeek-V3 reference code (free; see `pch/README.md`), which are not installed there. Without them it prints SKIPPED and why.

## 5. Safety
Read-only use of the harness: it imports `pch/harness/stepstream.py` / `harness.py` and reads the shipped `.jsonl` streams, and writes nothing into `pch/`. PCH core is not modified.

## 6. Further experiments and design notes
The harness pins 1 thread on purpose (reduction order). A failure here would be expected, and useful evidence, rather than a bug. Master Chief's versioned-rematch design reuses this check.
