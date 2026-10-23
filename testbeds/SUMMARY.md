# PCH and the squad test beds: a plain-English summary

## What the PCH is

The Personal Control Harness (PCH) is a "control harness" that wraps an AI
model while it generates output one step (token) at a time. Every step is
written into a **sealed step-stream**: a list of records where each record's
hash links to the one before it, and a trailer seals the last hash.

## What it does

- Each step's choice comes from a seeded draw, so it can be re-derived later.
- A verifier can **replay** a run and get the same result bit for bit
  (Bit-Exact tier), or check each recorded probability within a small
  tolerance on different hardware (Witnessed tier).
- If someone edits a token, the hash chain breaks, or the replay catches it.

Today it is a toy-scale prototype: it drives the DeepSeek-V3 reference code
shrunk to CPU size with synthetic weights. It proves the harness's
properties, not anything about a full-size model.

## Why it matters for LithoKraft's in-game AI

LithoKraft's in-game AI (for example a boss) can plan and act through the
PCH, and each fight leaves a receipt. Receipts let anyone check that the AI
played by the rules: same seed, same moves, no hidden edits. That is the
basis for the fair-play test beds below (HUD-only play, human input rates,
visible attack wind-ups, versioned rematches).

## Current limits (be honest about these)

- **Receipts are hashed only, not signed.** Hashes catch accidental or
  partial tampering, but not someone who deliberately recomputes the whole
  chain. No signing key exists yet.
- **The hash doesn't cover the trailer (known issue).** Fuzzing found this;
  the test-side checker requires exactly the trailer's three fields as a
  stopgap, so the fuzz result holds for the test-bed verifier, not the PCH
  core verifier. Fixing it is a PCH core change and is pending JD's go.
- **No session tracking.** Replay and rollback checks use a small test-side
  ledger (`receipt_check.py`) instead.
- **Model tests need extra free downloads:** CPU PyTorch plus the DeepSeek-V3
  reference code (`inference/`), pinned at commit `9b4e978`. Without them
  those tests skip (on 2026-10-23 they were installed and 04-07 passed). See `../pch/README.md`.

## How to get started

1. Install Python 3.10+ and CMake (both free).
2. From the repo root: `cd testbeds` then `python tb_shipped_streams_chain.py`
   (example 01). You should see `ALL PASS`.
3. Run everything: `cmake --preset testbeds-only && ctest --preset testbeds-only`.
   Add `-V` to see why a test skipped. Full build steps: `../BUILDING.md`.
4. Optional: install CPU torch and the pinned DeepSeek-V3 code to un-skip the
   model tests.

## The experiments

JaydeeWetwork's only instruction was to think up experiments, test beds and
designs for the PCH. Each squad member (Cortana, Herominde, Master Chief,
CloudAtlas) came up with their own ideas and independently designed, tested
and verified them, with no further context or direction. Each test bed is
credited Designed by <member>, Built by Cortana.
Status is as of 2026-10-23
(Python 3.14.8, torch 2.14.1+cpu, DeepSeek-V3 `9b4e978`). "Skipped" means the test exits cleanly with a reason;
nothing is faked.

| # | Experiment | Designed by | Built by | Goal | Status | Next step |
|---|---|---|---|---|---|---|
| 01 | Shipped streams: chain check | Cortana | Cortana | Prove the two shipped sealed runs are intact (chain, seal, no step gaps) | **Passed** (both streams, 16 steps each) | Edit a digit in a copy and watch it fail |
| 02 | Receipt tamper and replay check | CloudAtlas | Cortana | Catch altered, reordered, replayed or rolled-back receipts (8 cases) | **Passed** cases 1, 2, 4-8; case 3 skipped (receipts aren't signed) | Enable case 3 once signing lands |
| 03 | Receipt fuzz (10,000 mutations) | CloudAtlas | Cortana | Zero false accepts and zero crashes under random edits, measured against the test-bed verifier | **Passed** against the test-bed verifier (seed 20261023: 0 false accepts, 0 crashes, 92 no-ops) | Put the trailer inside the hash (needs JD's go) |
| 04 | Replay determinism | Cortana (folded into Herominde's fair-play set) | Cortana | 16 identical seeded runs match at every 64-step checkpoint | **Passed** (16/16 runs match, ~70 s) | Longer runs |
| 05 | Thread-count determinism | Herominde | Cortana | Same final hash with 1, 2, 4 and all threads | **Passed** (2, 4, 12 threads == 1 thread) | Try other CPUs; a mismatch is useful evidence |
| 06 | Rewind fidelity | Cortana | Cortana | Snapshot, run, rewind, re-run must match for N = 1 to 65536 | **Passed** N = 1 and 64; N = 1024+ not run (toy model context is 248 steps) | Longer-context fixture or engine tick simulation |
| 07 | Lookahead fork budget | Cortana | Cortana | Measure time, memory and score for 1-16 branches | **Passed** 1-16 branches (0.12-1.83 s, peak RSS 205-208 MiB) | Compare a laptop and a desktop |
| 08 | HUD reading | Master Chief | Cortana | Boss acts only on what's on screen: no hidden-state leaks, 90%+ match rate | Skipped: no boss brain or HUD feed yet | Build the missing feature, then the real check |
| 09 | Telegraph fairness | Master Chief and Herominde | Cortana | Every attack shows a readable wind-up before it lands | Skipped: no boss attacks with wind-up/HUD timing yet | Same as 08 |
| 10 | Versioned receipts and rematch | Master Chief | Cortana | Rematches replay the same and receipts carry a version stamp and gap-free counter | Skipped: no version stamps or rolling counter yet | Same as 08 |
| 11 | HUD-only honesty | Herominde | Cortana | Blank the on-screen bar; boss must not "x-ray" the real value | Skipped: no HUD-only observation path yet | Same as 08 |
| 12 | Human input rate | Herominde | Cortana | No burst of boss actions exceeds a human rate cap | Skipped: no action-rate cap yet | Same as 08 |
| 13 | Fixed branch count (ranked) | Herominde | Cortana | "Phone" and "PC" budgets give identical decisions | Skipped: no configurable branch budget yet | Same as 08 |
| 14 | Brain-version stamp | Herominde | Cortana | Old receipts on a new build are refused or labeled, not silently replayed | Skipped: no brain-version field yet | Same as 08 |

Every test bed uses the harness read-only and doesn't change PCH core.
Details per experiment are in `examples/`, and the original designs with
credit are in `designs/`.
## Shipped streams across platforms (fixed 2026-10-23)

Platform-tagged reference streams (core fix approved by JD 2026-10-23):
the prefill cache digest hashes raw float output, which differs between OS,
CPU arch and torch builds. Stream headers now record `platform` (os, arch,
torch, python) inside the hashed chain, so the tag can't be swapped quietly.
`pch/harness/stream_*.jsonl` stay as the original Linux reference set
(untagged; treated as Linux x86_64, torch 2.14.1+cpu). Windows reference
streams recorded on the dev workstation are in
`pch/harness/reference_streams/windows-amd64-torch2.14.1-cpu/`.
`verify_streams.py` picks the set matching this machine (OS, arch, torch)
and reports SKIPPED, not FAILED, when none matches. `disc_test_llm.py`
now writes fresh streams to a temp folder (or `PCH_OUT_DIR`) and never
overwrites the committed ones. To add a reference set for a new platform:
`PCH_OUT_DIR=reference_streams/<name> python disc_test_llm.py` from `pch/harness/`.
