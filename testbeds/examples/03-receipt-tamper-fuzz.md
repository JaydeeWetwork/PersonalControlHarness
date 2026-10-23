# Receipt fuzz (10,000 mutations)

## 1. Credits
Designed by: CloudAtlas.
Built by: Cortana.

The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these test suites on their own, without input from JD; JD then verified them and approved them for posting (and asked for longer rewind runs). JD human-reviews and verifies all AI-authored work. Design: `../designs/cloudatlas-receipt-tamper-check.md`.

## 2. Goal
Throw 10,000 random edits (value changes, added/removed fields, swapped/dropped records, byte damage) at a sealed receipt and require zero false accepts and zero crashes. Since 0.0.0.4 the test-bed verifier (`testbeds/receipt_check.py`) runs the PCH core check (`stepstream.check_sealed`) plus size caps.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_receipt_tamper.py --fuzz 10000

Or through CMake: `ctest --preset testbeds-only -R fuzz` (after `cmake --preset testbeds-only`).
Pass: `0 false accepts`, `0 crashes` (mutations that change nothing are counted separately as no-ops). Fail: prints the counts.

## 4. Status
**verified** (seed 20261023: 0 false accepts, 0 crashes; no-op mutations are counted and printed).

## 5. Safety
Read-only use of the harness: it imports `pch/harness/stepstream.py` / `harness.py` and reads the shipped `.jsonl` streams, and writes nothing into `pch/`. PCH core is not modified.

## 6. Further experiments and design notes
**Fixed 2026-10-23 (S1.3):** early fuzzing found the trailer itself was not hashed. Schema-2 trailers are now part of the chain (`tb_trailer_chain.py`); the shipped streams keep their legacy trailer and are labeled `legacy-unchained`. Signatures are fuzzed by bed 15.
