# Replay determinism

## 1. Credits
Designed by: Cortana (folded into Herominde's fair-play set).
Built by: Cortana.

The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these test suites on their own, without input from JD; JD then verified them and approved them for posting (and asked for longer rewind runs). JD human-reviews and verifies all AI-authored work. Design: `../designs/cortana-testbeds.md`.

## 2. Goal
Run the same seeded run 16 times and compare the receipt hash every 64 steps. Any mismatch reports the first checkpoint where runs diverge.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_model_testbeds.py replay_determinism

Or through CMake: `ctest --preset testbeds-only -R replay_determinism` (after `cmake --preset testbeds-only`).
Pass: all 16 runs match run 1. Fail: `first divergence at checkpoint N`.

## 4. Status
**verified** 2026-10-23 (Windows 11 workstation, Python 3.14.8, torch 2.14.1+cpu, DeepSeek-V3 `9b4e978`): all 16 runs matched run 1 (CTest 70-73 s). Without torch and the DeepSeek-V3 code it prints SKIPPED and why.

## 5. Safety
Read-only use of the harness: it imports `pch/harness/stepstream.py` / `harness.py` and reads the shipped `.jsonl` streams, and writes nothing into `pch/`. PCH core is not modified.

## 6. Further experiments and design notes
The PCH's own `pch_more_tests.py` and `disc_test_llm.py` also run under CTest (`pch.battery2_rewind_witness`, `pch.disc_test`) and skip the same way without torch. On 2026-10-23 both passed (disc_test 5/5, battery #2 7/7). `disc_test_llm.py` writes its fresh streams to a temp folder (or `PCH_OUT_DIR`), never over the committed ones (core fix approved by JD 2026-10-23).
