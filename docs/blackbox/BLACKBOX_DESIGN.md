# Black Box Mystery Solved: design (draft 1, 2026-10-23)

A replayable, tamper-evident decision log for LithoLattice. The idea is the same as a flight recorder. When a robot, an NPC or an AI agent does something surprising, we can:
- prove what it saw and what it decided, tick by tick;
- prove the record wasn't edited afterwards;
- replay the run to get the same decisions again.

Status: **design and box prototype only.** Nothing has been wired into the engine; in PCH it runs as test bed 16. A working C prototype with self-tests is in `blackbox/` (in this repo) (see section 10).

## 1. Goals

1. **Record everything that drives a decision.** Every input sample, every sensor sample, and every AI decision is recorded, each stamped with its tick number. This covers NPC/mob AI (utility picks, behaviour-tree results, learned-scorer outputs) and AI agents (MCP / PCH-driven actions).
2. **Deterministic replay.** Load the log, feed the recorded inputs and sensor samples into the same build, and you get the same decisions tick by tick. The first tick that differs is reported.
3. **Tamper evidence.**
   - Entries are hash-chained, so any edit, insertion, deletion or reordering breaks the chain.
   - Truncation is detected.
   - Periodic signed checkpoints stop a forger who rewrites the whole chain.
4. **Never slows the tick.**
   - The tick thread only copies bytes into a ring buffer: no I/O, no locks, no allocation and no hashing.
   - A background flusher does the rest.
5. **Never silent.** If the ring overflows, the dropped records are counted and a GAP record is written into the chain. A gap is visible, never hidden.

## 2. Where it fits in the engine

**Style.** It follows the engine's existing style:
- deterministic;
- fast ticking;
- critical logs only in release;
- verbose logging only behind `LITHO_VERBOSE_LOG`.

**About `LITHO_VERBOSE_LOG`.** It does not exist in the tree yet. The nearest existing pattern is `LITHO_THREAD_LOG` (core/thread_debug.h), which compiles to `((void)0)` when its switch is off. The prototype mirrors that pattern:
- `BB_VLOG` compiles to nothing unless `LITHO_VERBOSE_LOG` is defined;
- `BB_CRIT` (overflow, I/O failure, signer failure) is always on.

**Likely recording points.** These are from the current headers and are proposals only; nothing is wired up:

| What | Where it comes from today | Entry type |
|---|---|---|
| Player / controller input | input pump → game input (InputPump / IInputBackend) | INPUT |
| Sensor samples (robotics) | future sensor seam; the vision/audio capture views are the nearest today | SENSOR |
| Seeds | world seed, per-system RNG seeds | SEED |
| NPC/mob decisions | `pickUtility()` result, behaviour-tree (`IBtNode`) result, `ILearnedScorer` output (ai/brain.h) | DECISION |
| AI agent actions | MCP `IPlayerController` calls, PCH-driven boss actions | DECISION |

**Ordering rule.** Records are appended at the deterministic commit point of each tick, in the same canonical order the deterministic scheduler (det_sched) already enforces. They are not appended from worker threads in completion order. This keeps the log byte-identical across runs and thread counts. The prototype test "two identical runs produce identical logs" checks this property.

**Struct padding.** Any C/C++ struct written as a payload must have zeroed padding. Uninitialised padding bytes get hashed, which makes logs differ between runs and makes replay comparisons fail. The prototype hit exactly this issue: `ts_decision` has 2 padding bytes. Rule: serialise fields explicitly, or `memset` the struct first.

## 3. File format (version 1, little-endian)

```
File header (64 bytes)
  0  magic        "LLBLKBX1"
  8  u16 version  = 1
 10  u16 hash_alg = 1 (SHA-256)   [2 = BLAKE3, reserved]
 12  u32 header_len = 64
 16  u64 session_id
 24  u64 start_tick
 32  32 bytes reserved, must be zero
genesis hash H0 = SHA-256(header)

Entry i
  0  u32 len        payload length (max 64 KiB)
  4  u16 type       0x01 INPUT, 0x02 SENSOR, 0x03 DECISION, 0x04 SEED,
                    0xF0 CHECKPOINT, 0xF1 GAP, 0xF2 END
  6  u16 flags      0 in v1
  8  u64 seq        0,1,2,... with no holes
 16  u64 tick
 24  payload[len]
 24+len  hash[32]   Hi = SHA-256(H(i-1) || header bytes 0..23 || payload)
```

**Per-entry overhead** is 56 bytes: a 24-byte header and a 32-byte hash.

**CHECKPOINT** (112-byte payload):
- `u64 covers_seq` (the last entry covered);
- `prev_hash[32]`;
- `key_id[8]`;
- `sig[64]`, a signature over `SHA-256("LLBB-CKPT1" || session_id || covers_seq || prev_hash)`.

**GAP** (24-byte payload):
- `u64 dropped_count`, `u64 first_tick`, `u64 last_tick`.
- It is written by the flusher whenever the producer had to drop records.

**END** (16-byte payload):
- `u64 seq_of_end`, `u64 total_dropped`.
- A log without END is either truncated or from a crashed run. The verifier reports TRUNCATED and gives the last good offset and the last signed sequence number, so a crash log can still be trusted up to its last signed checkpoint.

**Hash choice.**
- **SHA-256** is the default: standard, FIPS, hardware-accelerated (SHA-NI / ARMv8 SHA2), and already in the PCH's world.
- **BLAKE3** is an option (`hash_alg = 2`). It is about 3 to 10 times faster in software and natively tree-hashable, but it is not FIPS. The `hash_alg` field lets us switch without a format break.

## 4. Signed checkpoints (design only; no real keys touched)

- **When.** Every N entries (configurable; the prototype default in the tests is 64 to 256 entries) and at close. Later we can also add one per M ticks.
- **Proposed signature scheme: Ed25519**, the same as PCH receipts. The signer lives behind a callback (`bb_sign_fn`), so the log code never sees key material.
  - The key should be a per-device or per-session key held by the platform (an OS keystore or TPM on robots).
  - Its key id goes in each checkpoint.
- **What a signed checkpoint proves.** Everything up to `covers_seq` existed, in this order, when the device signed it.
  - A forger who edits entries and recomputes every hash still fails the next signature check. The prototype tests this.
  - Entries after the last checkpoint are protected only by the hash chain. That is the exposure window, so keep N small for safety-critical robots.
- **Prototype stand-in.** The prototype uses HMAC-SHA256 with a fixed, public, TEST-ONLY key (`bb_test_sign`) so the flow can be tested. It is clearly labelled and is not a real signature.
- **Out of scope here:** key provisioning, rotation and revocation (see open questions).

## 5. Write path: ring buffer plus a background flusher

```
tick thread                         flusher thread
-----------                         --------------
bb_append(tick,type,payload)        loop:
  if ring free < need:                drain ring -> for each record:
     dropped++, note ticks             Hi = SHA256(H(i-1)||hdr||payload)
     return BB_DROPPED (no wait)        fwrite(hdr,payload,Hi)  (buffered)
  memcpy hdr+payload into ring          every N: CHECKPOINT (+sign, fflush, optional fsync)
  publish head (release store)        if dropped changed: write GAP
                                      if ring empty: fflush, sleep 0.5 ms
```

- **Single-producer / single-consumer lock-free ring.** The producer does one acquire load, a memcpy and one release store. It never takes a lock, never calls the OS and never waits.
- **No wakeup signalling at all.** The flusher polls every 0.5 ms when idle. This is deliberate: the producer has nothing to signal, so there is no condition variable and no possible lost wakeup. The cost is up to 0.5 ms of extra latency to disk and about 2,000 tiny idle wakeups per second. (Lost wakeups are exactly the kind of issue suspected behind the `engine_sim` hang, PCH B8.)
- **Multiple producers.** Today the AI and the sim commit on the tick thread, so one ring is enough. If worker threads need to log directly, give each one its own SPSC ring, and have the flusher merge them in (tick, canonical order) at tick boundaries.
- **Overflow policy: drop and count, never block.** In a safety system, blocking the control loop is worse than losing log records. Every drop shows up as a GAP record and in the verifier report. The ring size should be several seconds of peak rate (the prototype default is 1 MiB, and it is configurable).
- **Durability.** Writes are buffered. `fflush` runs at checkpoints and when idle; `fsync` at checkpoints is optional (`fsync_on_checkpoint`). On a crash, at most the last checkpoint interval plus the ring contents is lost, and the verifier shows exactly where the log stops.
- **Release vs. debug.** The log is on in release; that is its whole purpose. Only verbose tracing is compiled out.

## 6. Verify and replay tool

`bbverify [--test-key] [--dump] FILE` (prototype CLI) checks:
1. The header: magic, version, hash algorithm, reserved bytes.
2. Every entry: the length is in range, the hash matches, and seq is consecutive with flags 0.
3. Every checkpoint: it covers the previous entry, its prev_hash matches, and its signature is valid when a verifier is supplied.
4. GAP and END bookkeeping, and no data after END.

The result is one of: OK, BAD_HEADER, HASH_MISMATCH, TRUNCATED, BAD_SEQ, BAD_CHECKPOINT or TRAILING_DATA. The report also gives:
- entries verified;
- signed checkpoints verified, and the sequence number it is trusted through;
- gaps and dropped records;
- the last good offset;
- the head hash.

The exit code is 0 only for OK.

**Replay.** `bb_verify_buffer()` takes a callback that receives each verified engine record in order. A replay harness:
1. seeds a fresh sim from SEED;
2. feeds INPUT and SENSOR samples per tick;
3. recomputes the decisions and compares them byte for byte with the recorded DECISION entries;
4. reports the first tick that differs.

Replay only uses entries that already passed the hash check. The prototype implements this against a toy mob-AI sim, and it catches a forged decision even after the forger recomputes the whole chain.

**Engine version.** For real replay, the log must also pin the engine build (a commit hash in a SEED/meta record) and the sim config, so we replay with the same code.

## 7. Performance budget

**Proposed targets for LithoLattice:**
- Tick-thread cost under 100 ns per record, and under 0.5% of a tick at 1,000 records per tick.
- Zero blocking calls on the tick thread.
- The flusher sustains at least 2 times the steady-state record rate, on one background core.

**Measured on the box prototype.** Box: 8 vCPU Intel Xeon VM, GCC -O2, portable C SHA-256 (no SHA-NI), 200,000 records sent in bursts of 1,000:

| Payload | append() mean | append() p99 | end-to-end per entry (hash + write) | file bytes per entry |
|---|---|---|---|---|
| 16 B | 28 ns | 41 ns | 0.90 µs | 72 |
| 64 B | 41 ns | 117 ns | 1.24 µs | 120 |
| 256 B | 28 ns | 92 ns | 0.65 µs* | 101* |

\*At 256 B the burst outran the flusher and 135,450 records were dropped, all counted in GAP records as designed. With no pacing, the producer is about 20 to 40 times faster than this flusher.

**Reading the numbers:**
- **The tick thread is well inside budget:** about 30 to 40 ns per record. At 1,000 records per tick that is about 40 µs per tick.
- **The flusher is the limit:** about 0.8 to 1.1 million entries per second. The portable SHA-256 here costs about 0.94 µs per 120-byte entry on this VM.
- **Ways to raise flusher throughput 5 to 10 times:** a SHA-NI / ARMv8 SHA-256 (OpenSSL / BoringSSL) or BLAKE3. Batching could raise it further.
- **Disk and file size:** 56 bytes of overhead per record, plus a 168-byte checkpoint every N records.
- **Signing:** the Ed25519 signature costs about 20 to 50 µs per checkpoint, on the flusher only.

These are prototype numbers on a VM. Re-measure on TheWorkstation and on the robot target.

## 8. Out of scope (for now)

- Key management: provisioning, rotation, revocation, TPM/keystore integration. Real keys are not touched.
- Encryption or privacy of the log contents (logs may contain player input; see the open questions).
- Remote or streaming upload and server-side storage.
- Compression.
- Rendering, audio and other non-decision state. The log records causes (inputs, sensors, seeds) and decisions, not frames.
- Making the whole engine deterministic. Replay assumes that the recorded systems are already deterministic. The log detects divergence but does not fix it.
- Windows port of the prototype. It uses pthreads and POSIX `fsync`; the engine version would use the engine's own threads.
- Integration into the engine tree. Another run is working there now.

## 9. Proposed rollout

1. Agree the format and the hook points (this doc).
2. Port the prototype into `engine/src/core` (or a new `blackbox` module) behind a CMake option. Use the engine's `NamedThread`/`ThreadUtil` for the flusher and a hardware SHA-256.
3. Hook the SEED, INPUT and DECISION points; add the replay harness to engine_tests.
4. Add the Ed25519 signer callback through the platform keystore.
5. Add the PCH test beds in PCH_EXPERIMENTS_PROPOSED.md.

## 10. Prototype (`blackbox/` (in this repo))

| File | What it is |
|---|---|
| blackbox.h / blackbox.c | the library: writer (ring + flusher + chain + checkpoints), verifier, test signer |
| sha256.h / sha256.c | portable SHA-256 + HMAC-SHA256 (FIPS 180-4 / RFC 4231 vectors in the tests) |
| bbverify.c | verify CLI |
| bbtest.c, toysim.h/.c | self-tests and the toy deterministic mob-AI sim |
| bbbench.c | the benchmark above |
| Makefile | `make test`, `make asan`, `make tsan`, `make` (all) |

**Self-test results:** 31 checks, all pass. They are also clean under AddressSanitizer + UndefinedBehaviorSanitizer and under ThreadSanitizer.
- SHA-256 and HMAC known-answer vectors.
- Append and chain: 1,001 records, 16 signed checkpoints and END all verify, and two identical runs produce byte-identical logs.
- Tamper: every byte of a 15,024-byte log was flipped (one bit, then 0xFF), giving 30,048 cases. All 30,048 were detected.
- Truncation: every length from 0 to 15,023 bytes was tried, and none verified as OK.
- Replay: 2,000 ticks and 16,000 decisions replayed with 0 mismatches.
  - A forged decision at tick 777 was flagged at tick 777, even after the forger recomputed the whole chain.
  - The signed-checkpoint check also rejected that forgery (BAD_CHECKPOINT).
- Wrong-seed replay diverges.
- Ring overflow: drops are reported exactly in GAP records.

## 11. Open questions for JD

1. Ed25519 per device, or per session? Where does the key live on robots (TPM, OS keystore)? How do rotation and revocation work?
2. Checkpoint interval: is the trade-off between entries and time acceptable (signing cost vs. the size of the unsigned tail)?
3. Overflow policy: is drop-and-GAP right for robots, or should some record classes (for example a safety stop) get a reserved, never-dropped lane?
4. SHA-256 (FIPS, hardware) or BLAKE3 (faster) as the shipped default?
5. Retention: how long are logs kept, at what maximum size? Should old segments roll over into a new file with a linking hash?
6. Privacy: player input in LithoKraft logs. Should logs be opt-in, local only, or encrypted at rest?
7. Should `LITHO_VERBOSE_LOG` become an engine-wide macro (it doesn't exist yet), alongside `LITHO_THREAD_LOG`?
8. Which decisions count as "AI decisions" for v1: just the final chosen action, or also the scores and candidates (much bigger)?
