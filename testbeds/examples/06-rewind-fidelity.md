# Rewind fidelity

## 1. Designed by
Cortana. The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these experiments independently, with no direction from JD; JD reviewed them (and asked for longer rewind runs). Design: `../designs/cortana-testbeds.md`.

## 2. Goal
Snapshot, run N steps, rewind (fresh model + prefix replay), re-run. The snapshot digest and the re-run must match, with a replay check on each N. N = 1, 64, 1024, 16384, 65536.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_model_testbeds.py rewind_fidelity

Or through CMake: `ctest --preset testbeds-only -R rewind_fidelity` (after `cmake --preset testbeds-only`).
Pass: digest, re-run tokens and verify_replay all match for each N. Fail: names N and the check.

## 4. Status
**skipped** on the reference workstation: needs CPU PyTorch and the DeepSeek-V3 reference code (free; see `pch/README.md`), which are not installed there. Without them it prints SKIPPED and why. Even with them, N = 1024, 16384 and 65536 are **not run**: the toy model holds only 256 tokens of context.

## 5. Safety
Read-only use of the harness: it imports `pch/harness/stepstream.py` / `harness.py` and reads the shipped `.jsonl` streams, and writes nothing into `pch/`. PCH core is not modified.

## 6. Further experiments and design notes
Long-N runs need a longer-context fixture or the engine's tick simulation; that is a follow-up.
