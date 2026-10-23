# PCH Test Bed: Receipt Tamper and Replay Check
Proposed by CloudAtlas, 2026-10-22. Design only, not run. Sandbox copies only: no engine source.

## Goal
Show that a PCH receipt (sealed tick, branch, result) can't be altered, reordered, replayed or forged without the check catching it.

## Cases
1. Bit flip: change one byte in the payload. Expect a hash mismatch, and the receipt is rejected.
2. Field edit: change the score or result but keep the old hash. Expect rejection.
3. Re-hash forgery: edit the payload and recompute a plain hash. SKIPPED: receipts not signed yet. Hash-only chains can't stop this, so a forger who recomputes the whole chain gets through. Turn this on when signing lands.
4. Replay: submit a valid receipt twice, or for another session. Expect rejection by nonce, session id and tick.
5. Reorder or drop: swap or remove receipts in a chain. Expect rejection, because each receipt carries the previous receipt's hash.
6. Branch swap: attach branch B's result to branch A's id. Expect rejection.
7. Rollback: present an older valid chain head. Expect rejection, because the counter only moves forward.
8. Truncation and oversize: send cut-off or huge receipts. Expect a clean reject with a size cap and no crash.

## Pass criteria
Every tampered case is rejected with a labeled reason, every clean receipt passes, there are zero crashes, and a fuzz pass of 10k random mutations gets no false accepts. Report SHA-256s of the inputs, and label anything not run as "not run".

## Status (Cortana, 2026-10-23)
PCH receipts are SHA-256 hash chains today, not signed, and no key exists yet. Cases 1, 2 and 4 to 8 run against the hashes now. Case 3 stays skipped until signing lands, and when it does, JD decides where the key lives (never in the repo).
