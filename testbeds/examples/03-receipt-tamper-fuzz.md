# Receipt fuzz (10,000 mutations)

## 1. Credits
Designed by: CloudAtlas.
Built by: Cortana.

The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these experiments independently, with no direction from JD; JD reviewed them (and asked for longer rewind runs). Design: `../designs/cloudatlas-receipt-tamper-check.md`.

## 2. Goal
Throw 10,000 random edits (value changes, added/removed fields, swapped/dropped records, byte damage) at a sealed receipt and require zero false accepts and zero crashes. The result is measured against the test-bed verifier (`testbeds/receipt_check.py`), not the PCH core verifier.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_receipt_tamper.py --fuzz 10000

Or through CMake: `ctest --preset testbeds-only -R fuzz` (after `cmake --preset testbeds-only`).
Pass: `0 false accepts`, `0 crashes` (mutations that change nothing are counted separately as no-ops). Fail: prints the counts.

## 4. Status
**verified against the test-bed verifier** (seed 20261023: 0 false accepts, 0 crashes, 92 no-op mutations). The test-bed verifier closes the trailer gap below with a stopgap; the PCH core verifier does not yet.

## 5. Safety
Read-only use of the harness: it imports `pch/harness/stepstream.py` / `harness.py` and reads the shipped `.jsonl` streams, and writes nothing into `pch/`. PCH core is not modified.

## 6. Further experiments and design notes
**Known issue (PCH core):** early fuzzing found the trailer itself is not hashed, so the test-bed checker requires exactly its three fields as a stopgap. Making the trailer part of the chain is a PCH core change and is pending JD's go.
