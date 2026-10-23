# Signature fuzz (10,000 mutations)

## 1. Credits
Designed by: CloudAtlas (the bed 03 method, extended to signatures; proposed in DeepSeek's hardening plan, task S1.8).
Built by: Cortana.

## 2. Goal
Throw 10,000 seeded mutations at a signed receipt: sidecar values, types, added/removed fields, raw byte damage, 1-3 hex flips in the signature, trailer edits, and forgers who re-chain the trailer or re-close the whole stream and patch the sidecar. The offline verifier (`pch/harness/receipt_sig.py`, the code `pch-verify` runs) must reject every mutation that changes anything and never crash.

## 3. How to verify
    cd testbeds
    python tb_signature_fuzz.py            # 10000 mutations, seed 20261023
    python tb_signature_fuzz.py 10000 --seed 7

Or `ctest --preset testbeds-only -R signature_fuzz`.
Pass: `0 false accepts`, `0 crashes` (mutations that change nothing are counted as no-ops).

## 4. Status
**verified** 2026-10-23 (seed 20261023 and seed 7). On its first run it found a real bug (uppercase hex accepted as the same signature), fixed in the same task; see `../../docs/hardening/FAIL-BEFORE.md`.

## 5. Safety
Uses one seeded throwaway key held in memory only. Writes nothing to disk.
