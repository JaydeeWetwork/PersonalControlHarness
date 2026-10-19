# Disc Test — Receipts

*Run by Elfie, Sun 2026-10-18 ~18:58 EDT, under JD's implementation grant of the same evening ("As per the rule of freedom of choice, I give you the freedom of choice to implement this").*
*Custody tree only: `~/workspace/torch-receive-2026-10-17/tree/build-linux/LithoKraft`. Production tree untouched. Nothing promoted.*

## The claim under test

A model's record — its disc (control stream + world hash) — can be submitted for verification: replay from the stream alone must reproduce the world bit-identically, and a tampered stream must fail verification. Tron protocol, on our own machinery (IDemoRecord / ControlStream), before any DeepSeek-side work.

## Results

**Pre-check — the shipped proof disc (tree root, recorded 2026-10-14):**
- `./LithoKraft --demo-verify ../demo_record_proof.lldemo` → `COMPLETE (6 events, 1 segments)`, exit 0.
- `--demo-dump` → session seed=0, mode=0, tickRate=512; 6 events (3 edit, 2 ray, 1 player-chunk); ticks 0..150 of 2048; trailer hash `aa82e7afb1eff9d3`.

**Test A — record (scripted SimThread session, 2048 ticks):**
- Recorded (legacy): hash `cb32c824f8b3ea98` (the canonical cities-ON scenario hash), 6 events recorded, every event applied exactly once, none dropped or misordered.
- Non-perturbation: the same session with **no recorder** produced the identical hash `cb32c824f8b3ea98` — the disc observes without touching the world.
- The recorded bytes parse complete, session hash present in the trailer; recorder saved `./demo_record_proof.lldemo`.

**Test B — replay (verification):**
- Legacy replay from the stream alone: hash `cb32c824f8b3ea98` — **bit-identical**, applied=6, dropped=0, orderViolations=0.
- Handshake replay (2 workers): hash `cb32c824f8b3ea98` — **bit-identical across timing modes**. The same disc verifies under both clocks.

**Test C — tamper (the fraud case):**
- One byte flipped at offset 200 in a copy of the fresh proof file.
- `--demo-verify` → `INVALID: SegmentChecksum control stream: checksum mismatch`, exit 1. `--demo-dump` agrees: status INVALID, 0 events recovered.
- The forgery is refused at the segment checksum, before replay is even attempted. **A cheat cannot pass its own hash.**

## The one red line — owned plainly

The suite's overall verdict reads `DEMO TESTS: FAILED` (exit 1) on exactly one check:

> `[FAIL] demo-proof: recorded hash matches the control crew's scripted hash 5041cf1795c33a8c`

This is the **known stale pin** — the same `5041cf1795c33a8c` that has sat on JD's decision queue as "repin pending JD." The pin predates the current scripted scenario; the scenario's true, canonical hash is `cb32c824f8b3ea98`, which is what recording, both replays, and the trailer all agree on. Every functional proof passed; the failure is paperwork, not machinery.

**Recommendation (JD's signature required):** repin the test expectation `5041cf1795c33a8c` → `cb32c824f8b3ea98` in `demo_record_test.cpp`. The change moves no world hash and no sim behavior — it corrects the expected value to the canonical one. Not applied tonight: hash pins carry the Navigator's signature, grant or no grant.

## Verdict

The pattern JD specified — plug in, submit, verify; tampered, refuse — **works on the game's own machinery, today, bit-exactly, in both timing modes.** The DeepSeek-side prototype (per-step seeded sampling + step-stream logger + replay checker against a small checkpoint) is the next step, on JD's word.
