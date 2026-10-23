# Replay determinism

## 1. Designed by
Cortana (folded into Herominde's fair-play set). The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these experiments independently, with no direction from JD; JD reviewed them (and asked for longer rewind runs). Design: `../designs/cortana-testbeds.md`.

## 2. Goal
Run the same seeded run 16 times and compare the receipt hash every 64 steps. Any mismatch reports the first checkpoint where runs diverge.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_model_testbeds.py replay_determinism

Or through CMake: `ctest --preset testbeds-only -R replay_determinism` (after `cmake --preset testbeds-only`).
Pass: all 16 runs match run 1. Fail: `first divergence at checkpoint N`.

## 4. Status
**skipped** on the reference workstation: needs CPU PyTorch and the DeepSeek-V3 reference code (free; see `pch/README.md`), which are not installed there. Without them it prints SKIPPED and why.

## 5. Safety
Read-only use of the harness: it imports `pch/harness/stepstream.py` / `harness.py` and reads the shipped `.jsonl` streams, and writes nothing into `pch/`. PCH core is not modified.

## 6. Further experiments and design notes
The PCH's own `pch_more_tests.py` and `disc_test_llm.py` also run under CTest (`pch.battery2_rewind_witness`, `pch.disc_test`) and skip the same way.
