# Fail-before / pass-after record (security changes, 2026-10-23)

Each security test below was run twice on the box (Linux, Python 3.13): against the pre-fix harness at `3198eee` (`PCH_HARNESS_DIR=<worktree of 3198eee>/pch/harness`) and against the fixed tree. Output is verbatim.

## S1.3 trailer in chain: `tb_trailer_chain.py`

Before (3198eee):
```
  [PASS] clean re-closed receipt accepted -- accepted (old core)
  [FAIL] trailer carries schema 2 and its own chain link
  [FAIL] core rejects: extra trailer field -- accepted (old core)
  [FAIL] core rejects: step count edited -- accepted (old core)
  [FAIL] core rejects: schema downgraded (field removed) -- accepted (old core)
  [FAIL] core rejects: trailer chain link edited -- accepted (old core)
  [PASS] core rejects: final_chain edited -- trailer seal mismatch

TRAILER IN CHAIN (S1.3): 5 FAILED
exit 1
```
After:
```
  [PASS] clean re-closed receipt accepted -- 16 steps, chained
  [PASS] trailer carries schema 2 and its own chain link
  [PASS] core rejects: extra trailer field -- seal: trailer has unexpected or missing fields
  [PASS] core rejects: step count edited -- seal: trailer is not in the hash chain (edited trailer)
  [PASS] core rejects: schema downgraded (field removed) -- seal: legacy trailer has unexpected or missing fields
  [PASS] core rejects: trailer chain link edited -- seal: trailer is not in the hash chain (edited trailer)
  [PASS] core rejects: final_chain edited -- seal: trailer is not in the hash chain (edited trailer)
  [PASS] shipped legacy stream still readable, labeled legacy-unchained -- 16 steps, legacy-unchained

TRAILER IN CHAIN (S1.3): ALL PASS
exit 0
```

## S1.7 bed 02 case 3 (re-hash forgery): `tb_receipt_tamper.py`

Before (new test file run inside a checkout of 3198eee, so it uses the old harness and old test-side verifier):
```
  [FAIL] case 3 re-hash forgery rejected by signature check -- no signature verifier in the PCH (No module named 'receipt_sig')
RECEIPT TAMPER (cases 1-8): 1 FAILED
exit 1
```
The test also demonstrates the gap directly on the fixed tree: "hash-only check alone accepts the re-hashed forgery -- ACCEPTED".

After:
```
  [PASS] case 3 honest signed receipt accepted -- signature OK: key <random>, brain weights:4365dc9610eadcfa, 16 steps, head 2757a34256c2c833
  [PASS] case 3 (the gap) hash-only check alone accepts the re-hashed forgery -- ACCEPTED
  [PASS] case 3 re-hash forgery + original .sig rejected -- binding: sidecar chain head does not match the receipt
  [PASS] case 3 re-hash forgery + .sig head patched rejected -- signature: Ed25519 verification failed
  [PASS] case 3 re-hash forgery re-signed with forger's key rejected -- key: signed by key <random>, not the given key <random>
  [PASS] case 3 tampered signature rejected -- signature: Ed25519 verification failed
  [PASS] case 3 tampered trailer rejected -- receipt: seal: trailer is not in the hash chain (edited trailer)
  [PASS] case 3 wrong public key rejected -- key: signed by key <random>, not the given key <random>
  NOTE: hash chains catch accidental/partial tampering; the Ed25519 signature (case 3) catches a deliberate full-chain recompute.
RECEIPT TAMPER (cases 1-8): ALL PASS
exit 0
```
