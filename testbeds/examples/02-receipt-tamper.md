# Receipt tamper and replay check (8 cases)

## 1. Credits
Designed by: CloudAtlas.
Built by: Cortana.

The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these test suites on their own, without input from JD; JD then verified them and approved them for posting (and asked for longer rewind runs). JD human-reviews and verifies all AI-authored work. Design: `../designs/cloudatlas-receipt-tamper-check.md`.

## 2. Goal
Show a PCH receipt can't be altered, reordered, replayed or rolled back without the check catching it. Cases: 1 byte flip, 2 field edit, 3 re-hash forgery, 4 replay, 5 reorder/drop, 6 branch swap, 7 rollback, 8 truncation/oversize.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_receipt_tamper.py

Or through CMake: `ctest --preset testbeds-only -R receipt_tamper$` (after `cmake --preset testbeds-only`).
Pass: each tampered input is rejected with a labelled reason, clean receipts accepted, no crashes. Fail: names the case that was wrongly accepted.

## 4. Status
**verified**, all 8 cases (case 3 un-skipped 2026-10-23, task S1.7). Case 3 first shows the gap (the hash-only check accepts a fully re-hashed forgery), then requires the Ed25519 signature check to reject: the forgery with the original `.sig`, with the `.sig` head patched, re-signed by a forger's key, a tampered signature, a tampered trailer and a wrong public key. Before the fix this case FAILS ("no signature verifier in the PCH"); see `../../docs/hardening/FAIL-BEFORE.md`.

## 5. Safety
Read-only use of the harness: it imports `pch/harness/stepstream.py` / `harness.py` and reads the shipped `.jsonl` streams, and writes nothing into `pch/`. Case 3 uses the PCH core signing module `pch/harness/receipt_sig.py`.

## 6. Further experiments and design notes
Replay and rollback use a small test-side ledger (`receipt_check.py`); PCH has no session tracking yet. Case 3 uses throwaway keys generated in memory; no key is ever written to the repo (see `../../pch/docs/KEY-MANAGEMENT.md`). Input SHA-256s are printed each run.
