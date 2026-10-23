# Proposed PCH experiments 16–20 (design only, 2026-10-23)

These are new test bed designs for the Personal Control Harness. **Design only.** The PCH repo has not been changed. The numbering continues from the 15 existing beds; renumber freely. Designer credit is JD's call.

Each bed follows the existing pattern: a deterministic fixture, a pass/fail check that a script can run, and no faked results. A bed that can't run (for example, missing weights) skips and says why.

---

## 16. Black Box replay and tamper bed

**Question:** Does a Black Box log of a boss fight replay to exactly the same decisions? And is every kind of tampering caught?

**Fixture:**
- The reference boss stub (the same one as beds 08–14) plays a scripted 2,000-tick fight.
- Its inputs, sensor samples (HUD-visible state), seed and every decision are logged with the Black Box writer, with signed checkpoints every 128 entries (test key).

**Cases:**
1. **Clean replay.** Verify the log, replay it into a fresh boss, and compare every decision byte for byte. Pass: 0 mismatches and verifier OK.
2. **Single-byte flips.** Flip every byte (or 10,000 random bytes, the same as bed 03). Pass: 100% detected, with the detected offset reported at or before the flipped entry.
3. **Truncation.** Cut the log at every entry boundary and at 1,000 random lengths. Pass: never OK, and TRUNCATED gives the right last good offset.
4. **Re-chained forgery.** Change one decision and recompute every hash. Pass: replay flags exactly that tick, and the checkpoint signature check fails.
5. **Reorder / delete / duplicate one entry.** Pass: BAD_SEQ or HASH_MISMATCH.
6. **Cross-link to PCH receipts.** The PCH receipt for the same fight records the Black Box head hash at close. Pass: the receipt and the log agree; changing either one breaks the check.

**Output:** a table of cases with detected/total counts; the replay mismatch list (expected to be empty).

---

## 17. Lost-wakeup regression bed

**Question:** Can any producer/consumer handoff in the engine or the PCH lose a wakeup and hang? (This is the class of issue suspected behind the `engine_sim` intermittent hang, BLOCKERS B8.)

**Fixture:**
- A stress harness runs each handoff primitive under test in a tight loop:
  - the job system's queues and wait/notify points;
  - the sim thread tick handoff;
  - the input pump;
  - the Black Box flusher.
- Every wait has a watchdog timeout. A run "hangs" if any wait exceeds 100 times its expected latency.

**Cases:**
1. **Repeat until failure.** 100,000 rounds each at 1, 2, 4, 8 and 16 threads, with random `sched_yield` and sleep jitter (0–50 µs) injected between "check condition" and "wait". This is the classic lost-wakeup window.
2. **Spurious wakeups.** Inject extra notify calls with no state change. Pass: no extra work is done and nothing double-runs.
3. **Shutdown race.** Stop requested while a producer is mid-push. Pass: the joins finish, and nothing is left hanging or detached.
4. **Optional, under ThreadSanitizer:** the same harness. Pass: no data race reports.
5. **Regression pin.** Once the B8 root cause is found, the exact interleaving becomes a deterministic test (forced with a test hook or a seeded scheduler) that failed before the fix and passes after it.

**Pass:**
- 0 hangs in every configuration;
- the watchdog report lists each primitive with its max observed wait;
- this runs on Windows and Linux (B8 shows on Windows Release).

---

## 18. Replay across thread counts and machines

**Question:** Does the same seed and the same inputs give byte-identical Black Box logs, and identical decisions, at 1, 4 and 8 worker threads, and on Windows vs. Linux?

**Cases:**
1. Record the same scripted session at each thread count on the same machine. Pass: identical head hash.
2. Record on the Windows workstation and on the Linux box. Pass: identical head hash.
   - If they differ, the first differing tick and record type is the report. That is how floating-point or ordering issues show up.
3. Cross-replay (a Windows log replayed on Linux, and the reverse). Pass: 0 decision mismatches.

This pairs with the PCH Bit-Exact vs. Witnessed tiers. It tells us which tier the in-game AI can honestly claim.

---

## 19. Overflow honesty bed (the log never lies by omission)

**Question:** Under overload, does the Black Box drop records visibly and never silently? And does the tick stay on time?

**Cases:**
1. Flood the writer at 10, 50 and 100 times the steady-state rate with a small ring. Pass: every dropped record is counted in GAP records (append drops = GAP total = writer counter), and the verifier is OK.
2. Measure tick duration with and without logging under the flood. Pass: the tick's p99 rises by less than the budget (proposed: less than 1%), and no tick waits on I/O.
3. Slow disk: throttle the output file (for example a FUSE or slow-write shim). Pass: same as 1 and 2; the flusher falls behind but the tick doesn't.
4. *(If JD wants a reserved lane: see the design's open question 3.)* Safety-stop records are never dropped, even under flood.

---

## 20. Checkpoint signing and crash recovery bed

**Question:** After a crash, how much of the log can still be trusted? Do the signatures hold?

**Cases:**
1. Kill the process (SIGKILL / TerminateProcess) at random points during a 10,000-tick run, 200 times. Pass:
   - every log verifies as TRUNCATED (never as corrupt in the middle);
   - "trusted through seq" is at most one checkpoint interval behind the last entry the writer accepted.
2. Use the wrong key id or the wrong key. Pass: BAD_CHECKPOINT.
3. Splice in a checkpoint from another session (same key, different session_id). Pass: BAD_CHECKPOINT, because the session id is inside the signed message.
4. With `fsync_on_checkpoint` on, use power-loss simulation (a VM snapshot or dm-flakey on Linux). Pass: nothing before the last fsynced checkpoint is lost.

Uses the TEST signer only. Real Ed25519 keys come in only when JD approves the key plan.

---

## Shared notes

- Beds 16, 18 and 19 can run on the box prototype today. Beds 17 and 20 need small engine or OS hooks.
- None of these needs model weights, so they won't add to the `pch.weights_local` skips.
- "Issues" found by these beds go into BLOCKERS/CHANGES the usual way.
