# Receipt tamper and replay check (8 cases)

## 1. Designed by
CloudAtlas. The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these experiments independently, with no direction from JD; JD reviewed them (and asked for longer rewind runs). Design: `../designs/cloudatlas-receipt-tamper-check.md`.

## 2. Goal
Show a PCH receipt can't be altered, reordered, replayed or rolled back without the check catching it. Cases: 1 byte flip, 2 field edit, 3 re-hash forgery, 4 replay, 5 reorder/drop, 6 branch swap, 7 rollback, 8 truncation/oversize.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_receipt_tamper.py

Or through CMake: `ctest --preset testbeds-only -R receipt_tamper$` (after `cmake --preset testbeds-only`).
Pass: each tampered input is rejected with a labelled reason, clean receipts accepted, no crashes. Fail: names the case that was wrongly accepted.

## 4. Status
**verified** for cases 1, 2, 4-8. Case 3 **skipped**: receipts are not signed yet. Hash-only chains catch accidental or partial tampering but not a deliberate full-chain recompute.

## 5. Safety
Read-only use of the harness: it imports `pch/harness/stepstream.py` / `harness.py` and reads the shipped `.jsonl` streams, and writes nothing into `pch/`. PCH core is not modified.

## 6. Further experiments and design notes
Replay and rollback use a small test-side ledger (`receipt_check.py`); PCH has no session tracking yet. When signing lands, enable case 3. The key must never live in the repo. Input SHA-256s are printed each run.
