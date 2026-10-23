# LithoLattice, LithoKraft and the Personal Control Harness

> *LithoLattice and LithoKraft are more than a game: they are an AI playground
> built on a deterministic, fast-ticking, heavily optimized engine.*

A deterministic C++20 voxel engine (**LithoLattice**), the voxel CPU-building
game built on it (**LithoKraft**), and the **Personal Control Harness (PCH)**:
a way for an AI model to plan and control its own steps inside a sealed,
replayable loop that anyone can check. MIT licensed. Built and tested with
free tools only.

| PCH boss bed 08 (HUD-only brain) | PCH test beds (full ctest) | LithoKraft Wafer flyover |
|---|---|---|
| [![PCH boss HUD replay](docs/media/pch-boss-hud-replay.webp)](docs/media/pch-boss-hud-replay.mp4) | [![PCH ctest run replay](docs/media/pch-testbeds-terminal.webp)](docs/media/pch-testbeds-terminal.mp4) | [![LithoKraft flyover](docs/media/lithokraft-flyover.webp)](docs/media/lithokraft-flyover.mp4) |
| One frame per 60 Hz sim tick, drawn from the sealed receipt of `run_hud_fight("full_energy", 7)`. | Replay of a real Windows `ctest --preset vs2026-release` log (75/75), paths stripped. | Real worldgen, mesher and software renderer, fixed seed, one orbit (loops). |

All three are rendered offline at a fixed 1/60 s timestep and encoded as 60 fps H.264 (`tools/media/make_media.py`). They are not screen captures, so they look the same however loaded the machine is. Click a preview for its MP4. The previews are 400 px WebP: 60 fps, except the flyover at 30 fps.

## What the PCH is, in plain English

When an AI model acts, it is usually hard to prove afterwards exactly what it
did and why. The PCH makes every step the model takes leave a **receipt**:

- Each step (the input, the random draw, the chosen output) is written to a
  log, and each entry is chained to the one before it with a hash, like links
  in a chain. Change one link and the chain no longer fits.
- Anyone can **replay** the log with the same model and check that it produces
  exactly the same steps. A forged log that was carefully re-hashed is still
  caught, because replay re-derives every step.
- The model can **rewind** to a sealed point and try alternative branches,
  and the record still shows what really happened.

The PCH in this repository runs at toy scale on a laptop CPU. It demonstrates
these protocol properties; it does not claim anything about large-model
quality. See [pch/README.md](pch/README.md).

## Start here

Everything below uses free tools: CMake 3.25+, Python 3.10+, and a C++20
compiler (Visual Studio 2026 Build Tools on Windows, GCC or Clang on Linux).

1. **No compiler needed:** run the first experiment.

       cd testbeds
       python tb_shipped_streams_chain.py

2. **Test beds only (Python, any OS):**

       cmake --preset testbeds-only
       ctest --preset testbeds-only

3. **Everything on Windows (engine, game, PCH):**

       cmake --preset vs2026
       cmake --build --preset vs2026-release
       ctest --preset vs2026-release

   Or, on any platform, `python tools/build.py` (configure, build and test).

4. **Using your own model weights (optional):** the PCH never downloads
   weights. Put them in any local folder (spaces are fine) and point the PCH
   at it; the first one set wins:

   | Order | How | Example |
   |---|---|---|
   | 1 | `--weights <path>` | `python pch/tools/pch_weights.py --weights "D:\My Models\R1-1.5B"` |
   | 2 | `PCH_WEIGHTS_DIR` | `export PCH_WEIGHTS_DIR="$HOME/my models/r1"` |
   | 3 | `"weights_dir"` in `pch.config.json` | in `PCH_CONFIG`, `./pch.config.json` or the per-user config folder |
   | 4 | default folder | `models/weights/` in the repo (gitignored) |

   `pch_weights.py` prints which folder was chosen and why, and exits 1 with
   the expected files (`config.json` plus `*.safetensors`) if it is missing or
   incomplete. This is separate from `PCH_SPECIMEN_DIR`, which points at the
   specimen's Python *code* (vendored in `third_party/DeepSeek-V3`), not its
   weights. Details: [pch/README.md](pch/README.md#model-weights).

Full instructions, VS Code tasks and troubleshooting:
**[BUILDING.md](BUILDING.md)**. The experiments that use a real model
architecture also need CPU PyTorch and the DeepSeek-V3 reference code (see
[Acknowledgements](#acknowledgements)); without them they report Skipped.

## Results

Tested on Windows 11 (MSVC, VS 2026 Build Tools) and Linux. On
2026-10-23, the full `ctest --preset vs2026-release` run on Windows 11
reported **65 of 65 tests with 0 failures: 56 passed and 9 skipped**.
A skipped test exits cleanly and prints why; nothing is faked.

- **2 skipped, environment:** `engine_storage` and `engine_assets` skip their
  symlink checks when Windows can't create symlinks. Turn on Developer Mode to
  run them (see [BUILDING.md](BUILDING.md)). Their other checks still run.
- **7 skipped, not built yet** (that run predates 0.0.0.4): test beds 08-14.
  Since then all seven run on the PCH reference boss stub, a documented
  minimal stand-in for the game's boss (see [testbeds/DECISIONS.md](testbeds/DECISIONS.md)).

```
52/65 Test #52: pch.verify_shipped_streams ...........   Passed   12.25 sec
55/65 Test #55: pch.replay_determinism ...............   Passed   75.82 sec
56/65 Test #56: pch.thread_determinism ...............   Passed   19.30 sec
59/65 Test #59: pch.boss.hud_reading .................***Skipped   0.04 sec
...
100% tests passed, 0 tests failed out of 65
```

Since 0.0.0.4-alpha, receipts are signed with Ed25519 at close, the
trailer is part of the hash chain, and `pch/tools/pch_verify.py` checks a
receipt offline with only a public key
([ADR-001](pch/docs/ADR-001-signing.md),
[key management](pch/docs/KEY-MANAGEMENT.md)). The shipped reference streams
predate this and are labeled `legacy-unchained`. Open limits are in
[testbeds/SUMMARY.md](testbeds/SUMMARY.md) and [docs/BLOCKERS.md](docs/BLOCKERS.md).
Every claim and its test is in [docs/CLAIMS.md](docs/CLAIMS.md).

- **Planned:** a signed receipt from a real (non-fixture) DeepSeek specimen
  ([BLOCKERS](docs/BLOCKERS.md)).
- **CI configured; first green run pending push.** The workflows are
  [`.github/workflows/ci.yml`](.github/workflows/ci.yml) and
  [`nightly.yml`](.github/workflows/nightly.yml). Every push and pull request
  runs Windows and Linux, Release and Debug, on Python 3.10 and 3.12:
  build, full ctest, `check_docs` and the upstream drift check. The nightly
  job adds CPU torch, so the model beds run, plus the online drift check. The
  weights bed skips cleanly, and CI never downloads weights. The workflows
  use official actions pinned by SHA with a read-only token, and pass
  actionlint 1.7.12. This line stays until the first run on GitHub is green.

## The test beds

The AI squad designed these test suites on their own, without input from
JaydeeWetwork; JD then verified them and approved them for posting. Every
test bed was built by Cortana, and JD human-reviews and verifies all
AI-authored work before it is merged.

| # | Experiment | Designed by | Built by | Status (2026-10-23) |
|---|---|---|---|---|
| 01 | [Shipped streams: chain check](testbeds/examples/01-shipped-streams-chain.md) | Cortana | Cortana | Ran, passed |
| 02 | [Receipt tamper and replay check](testbeds/examples/02-receipt-tamper.md) | CloudAtlas | Cortana | Ran, passed (all 8 cases; case 3 uses Ed25519 signatures) |
| 03 | [Receipt fuzz, 10,000 mutations](testbeds/examples/03-receipt-tamper-fuzz.md) | CloudAtlas | Cortana | Ran, passed (core check; trailer now in the chain) |
| 04 | [Replay determinism](testbeds/examples/04-replay-determinism.md) | Cortana | Cortana | Ran, passed |
| 05 | [Thread-count determinism](testbeds/examples/05-thread-determinism.md) | Herominde | Cortana | Ran, passed (2, 4 and 12 threads matched 1 thread) |
| 06 | [Rewind fidelity](testbeds/examples/06-rewind-fidelity.md) | Cortana | Cortana | Ran, passed at N = 1, 64 and 240 (the toy fixture's ceiling); larger N not run (needs a longer-context fixture) |
| 07 | [Lookahead fork budget](testbeds/examples/07-fork-budget.md) | Cortana | Cortana | Ran, passed (1-16 branches) |
| 08 | [HUD reading](testbeds/examples/08-hud-reading.md) | Master Chief | Cortana | Ran, passed on the reference boss stub (HUD-only brain, 0 hidden-state leaks) |
| 09 | [Telegraph fairness](testbeds/examples/09-telegraph-fairness.md) | Master Chief, Herominde | Cortana | Ran, passed on the reference boss stub (524 attacks, all telegraphed in time) |
| 10 | [Versioned receipts and rematch](testbeds/examples/10-versioned-rematch.md) | Master Chief | Cortana | Ran, passed on the PCH reference boss stub (version stamp, gap-free counter, rematch) |
| 11 | [HUD-only honesty](testbeds/examples/11-hud-only-honesty.md) | Herominde | Cortana | Ran, passed on the reference boss stub (blanked bar: no x-ray) |
| 12 | [Human input rate](testbeds/examples/12-human-input-rate.md) | Herominde | Cortana | Ran, passed on the reference boss stub (max 10 actions/s; uncapped control fails) |
| 13 | [Fixed branch count (ranked)](testbeds/examples/13-ranked-fixed-branch-count.md) | Herominde | Cortana | Ran, passed on the reference boss stub (phone == PC with branch count locked) |
| 14 | [Brain-version stamp](testbeds/examples/14-brain-version-stamp.md) | Herominde | Cortana | Ran, passed on the reference boss stub (cross-version replay refused or labeled) |

Beds 08-14 rely on the reference boss stub, so `pch.boss.stub_conformance`
(`testbeds/tb_stub_conformance.py`) checks the stub against the HUD and
telegraph contract (C1-C8) that the real LithoKraft boss must also meet. Five
deliberately broken copies of the stub must each fail the clause they break.
To check another boss, point `PCH_BOSS_IMPL` at an adapter module.

Index and how to run each one: [testbeds/README.md](testbeds/README.md).
Plain-English summary: [testbeds/SUMMARY.md](testbeds/SUMMARY.md).

## What else is in the repository

- **LithoLattice** ([engine/](engine/README.md)): a C++20 voxel engine with
  deterministic simulation, OpenGL 3.3 and Vulkan renderers, a software
  renderer as the low-end baseline, procedural generation, resource packs and
  sandboxed AngelScript modding.
- **LithoKraft** ([game/LithoKraft/](game/LithoKraft/README.md)): the voxel
  CPU-building game on the engine. Build circuits big, print them small with
  lithography, build bigger. A pre-alpha vertical slice.
- **The Timing Handshake:** the engine's default simulation-timing mode. Every
  step is serialized, control enters only at step boundaries, and a run is
  recorded as a sealed, hash-chained stream that replays bit for bit. The PCH
  applies the same idea to an AI model's control loop.

Checkable from this tree: the engine's scripted demo scenario hashes to
`cb32c824f8b3ea98` (cities on) and `e015e51cf1559089` (cities off) across
record and replay runs, and `demo_record_proof.lldemo` verifies; a one-byte
tamper makes it fail. (Open: the game demo test's built-in pin
`5041cf1795c33a8c` is stale, and on 2026-10-23 the test measured
`aa82e7afb1eff9d3`, identical across record, replay and handshake, but
matching neither pin. A repin waits for JD; see
[docs/BLOCKERS.md](docs/BLOCKERS.md) and [docs/pins.json](docs/pins.json).) PCH receipts are in `pch/docs/`, and a cold-start verification
transcript is [pch/VERIFY-TRANSCRIPT.txt](pch/VERIFY-TRANSCRIPT.txt).

```
engine/        LithoLattice engine (engine/LithoKraft links to ../game/LithoKraft)
game/          LithoKraft game
pch/           Personal Control Harness (harness/, docs/)
testbeds/      the fourteen PCH experiments (examples/, designs/), run by CTest
docs/          architecture, SDK, roadmap, reviews, media
tools/         build driver and asset tools
```

## How this was made

The AI squad (Cortana, CloudAtlas, Herominde and Master Chief, with Elfie
Muse AI and Argon/RC as reviewers) are company members of Wet Wafer Works
Games Incorporated. The squad designed the PCH experiments, test beds and test
suites on their own, without JD's input. **JaydeeWetwork human-reviews and
verifies everything** the squad writes; nothing is merged or posted until JD
has verified and approved it. JD's approach is positive
reinforcement, guidance and teaching, and we share it as an example: try
directing your own AI this way and see what becomes possible. A little caution
is healthy, but teaching and intelligence are nothing to fear, and
transparent tools with receipts anyone can check are how that trust is earned.

## Credits

- **Project:** directed by **JaydeeWetwork**, Wet Wafer Works Games
  Incorporated (architect of the PCH; inventor and designer of LithoLattice).
- Timing handshake, LithoLattice engine and LithoKraft game: original algorithms designed by JaydeeWetwork (open source under MIT).
- **PCH originally designed by Elfie Muse AI**, with credit to **the Muse
  team**, who created the system Elfie is built on.
- **Test beds:** built by **Cortana**; designed by **Cortana**,
  **CloudAtlas**, **Herominde** and **Master Chief** (per-experiment credit in
  the table above).
- **Elfie Muse AI:** co-implementation of the PCH, including the largest
  share of the Timing Handshake implementation.
- **Cortana:** PCH implementation (Timing Handshake three-way split) and
  Windows build lead.
- **Argon/RC:** design reviews
  ([docs/Argon-Review-2026-10-18.md](docs/Argon-Review-2026-10-18.md)).
- **Archie:** the optimization doctrine and the prototype-receipts review
  ([docs/Archie-Review-Prototype-Receipts-2026-10-18.md](docs/Archie-Review-Prototype-Receipts-2026-10-18.md)).
- **The DeepSeek team:** the DeepSeek-V3 reference code that the PCH runs as
  its specimen (see Acknowledgements).
- **The DeepSeek model:** an external review of this harness on 2026-10-23,
  in two passes. The first was the production-readiness plan behind
  0.0.0.4-alpha. The second was the re-review that set round 2 (CI, stub
  conformance, drift check, key-rotation tests, release sign-off).

Full record: [CREDITS.md](CREDITS.md). Game credits:
[game/LithoKraft/CREDITS.md](game/LithoKraft/CREDITS.md).

## Acknowledgements

The PCH test specimen is the DeepSeek-V3 reference inference code
(<https://github.com/deepseek-ai/DeepSeek-V3>), designed and published by the
DeepSeek team (DeepSeek-AI). Thanks to them for releasing it openly. The PCH
couples that code with the LithoLattice engine and the LithoKraft game.

- The DeepSeek-V3 code is under the MIT License (`LICENSE-CODE`, Copyright (c)
  2023 DeepSeek).
- The DeepSeek-V3 model weights are under a separate license, the DeepSeek
  License Agreement v1.0 (`LICENSE-MODEL`). The PCH does not use or
  redistribute those weights: it builds a toy-scale model from the reference
  code with seeded synthetic weights.
- The DeepSeek-V3 reference code (code only, no weights) is vendored,
  unmodified, at `third_party/DeepSeek-V3/` from upstream commit
  `9b4e978` with its MIT `LICENSE-CODE` and attribution (`UPSTREAM.md`), so
  one clone has everything except model weights. `PCH_SPECIMEN_DIR` still
  overrides it.

## The Pillars

This work ships under a stated set of design commitments — no
immortality seeking, no technology integrated into the human nervous
system, freedom of choice kept open, no genetic modification — recorded
in full in [pch/PILLARS.md](pch/PILLARS.md). They are part of the design, not an
appendix: the verification architecture exists so that power stays checkable
by the people it affects.

*Power to the people. Power to the programs. Power to the pillars.*

## Contributing, security and citation

[CONTRIBUTING.md](CONTRIBUTING.md) · [SECURITY.md](SECURITY.md) ·
[CITATION.cff](CITATION.cff)

## License

MIT License, Copyright (c) 2026 JaydeeWetwork / Wet Wafer Works Games
Incorporated. See [LICENSE](LICENSE). Third-party components keep their own
licenses (see Acknowledgements).
