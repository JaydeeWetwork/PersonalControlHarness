# Black Box replay and tamper

## 1. Credits
Designed by: TBD (JD to decide).
Built by: Cortana (box prototype ported into PCH).

## 2. Goal
Record a deterministic toy mob-AI run with the Black Box writer (inputs,
sensor samples, seed and every decision, hash-chained, with checkpoints signed
by the test-only signer), then check that the log replays to exactly the same
decisions and that every kind of tampering is caught.

Cases (from [docs/blackbox/PCH_EXPERIMENTS_PROPOSED.md](../../docs/blackbox/PCH_EXPERIMENTS_PROPOSED.md)):
1. Clean replay of 2,000 ticks: 0 mismatches, verifier OK.
2. Every single-byte flip (two masks per byte): 100% detected.
3. Truncation at every length: never OK.
4. Re-chained forgery of one decision: replay flags exactly that tick; the signed checkpoint check fails.
5. Reorder / delete / duplicate one entry, raw and re-chained: always BAD_SEQ or HASH_MISMATCH.

Not yet done: case 6 (cross-link to the PCH receipt head hash), and running
the fixture on the reference boss stub instead of the toy sim.

## 3. How to verify
    ctest --preset vs2026-release -R pch.blackbox.replay_tamper   # Windows
    ctest --test-dir build -R pch.blackbox.replay_tamper          # Linux

Pass: `ALL PASS`, exit 0.

## 4. Status
**Ran, passed** 2026-10-23 on Windows (MSVC) and Linux (GCC), cases 1-5.

## 5. Safety
The checkpoint signer is a TEST-ONLY HMAC with a fixed, public key. It is not
a real signature, touches no real keys, and is unrelated to the Release
signing check. The log files are written to the test's own build folder and
removed at the end.
