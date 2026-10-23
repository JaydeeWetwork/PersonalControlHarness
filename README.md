# LithoLattice, LithoKraft and the Personal Control Harness

> *LithoLattice and LithoKraft are more than a game: they are an AI playground
> built on a deterministic, fast-ticking, heavily optimized engine.*

A deterministic C++20 voxel engine (**LithoLattice**), the voxel CPU-building
game built on it (**LithoKraft**), and the **Personal Control Harness (PCH)**:
a way for a language model to plan and control its own actions inside a
sealed, replayable, verifiable loop. MIT licensed. Free tools only.

## Start here

1. Read **[BUILDING.md](BUILDING.md)**: CMake command line, VS Code or Visual Studio.
2. Run the first PCH example: `cd testbeds` then `python tb_shipped_streams_chain.py`.
3. Browse the experiments: **[testbeds/README.md](testbeds/README.md)** (index)
   and **[testbeds/SUMMARY.md](testbeds/SUMMARY.md)** (plain-English summary).

## Repository structure

```
engine/        LithoLattice engine (src/, assets/, tests/, docs/)
               engine/LithoKraft is a symlink to ../game/LithoKraft; keep it.
game/          LithoKraft game (src/, assets/, scripts/, CREDITS.md)
pch/           Personal Control Harness
  harness/     the reference harness and the two sealed run streams
  docs/        receipts and reviews
testbeds/      squad experiments for the PCH, run by CTest
  examples/    one readme per experiment (designer, goal, how to verify, status)
  designs/     the original design notes, with author credit
docs/          architecture, SDK, roadmap
  release-2026-10-19/   historical release-staging record
tools/         build driver and asset tools
.vscode/       VS Code tasks and settings
CMakeLists.txt, CMakePresets.json   top-level build and presets
requirements.txt                    CPU torch pin for the PCH
demo_record_proof.lldemo            sealed demo record (verification artifact)
LICENSE, CREDITS.md                 MIT license and full credits
```

## Experiments (test beds)

Fourteen PCH experiments designed by the squad (Cortana, Herominde, Master
Chief, CloudAtlas). Status, designer credit and how to run each one are in
[testbeds/README.md](testbeds/README.md).

Design reviews: Argon ([docs/Argon-Review-2026-10-18.md](docs/Argon-Review-2026-10-18.md)).

## Credits

**JaydeeWetwork** (Wet Wafer Works Games Incorporated): inventor, architect
and designer of the PCH and LithoLattice. Full record in [CREDITS.md](CREDITS.md).

AI contributors, listed under the names JaydeeWetwork gave them:

- **Elfie Muse AI**: co-implementation of the PCH; the largest share of the Timing Handshake implementation.
- **Cortana**: PCH implementation (Timing Handshake three-way split); Windows build lead; built all fourteen test beds.
- **Herominde**, **Master Chief**, **CloudAtlas**: test-bed designers (see below).
- **Argon/RC**: design reviews ([docs/Argon-Review-2026-10-18.md](docs/Argon-Review-2026-10-18.md)).

The project and the test beds are directed by JaydeeWetwork (direction and
dictation). Test beds (every one built by Cortana):

| # | Experiment | Designed by | Built by |
|---|---|---|---|
| 01 | Shipped streams: chain check | Cortana | Cortana |
| 02 | Receipt tamper and replay check | CloudAtlas | Cortana |
| 03 | Receipt fuzz | CloudAtlas | Cortana |
| 04 | Replay determinism | Cortana | Cortana |
| 05 | Thread-count determinism | Herominde | Cortana |
| 06 | Rewind fidelity | Cortana | Cortana |
| 07 | Lookahead fork budget | Cortana | Cortana |
| 08 | HUD reading | Master Chief | Cortana |
| 09 | Telegraph fairness | Master Chief, Herominde | Cortana |
| 10 | Versioned receipts and rematch | Master Chief | Cortana |
| 11 | HUD-only honesty | Herominde | Cortana |
| 12 | Human input rate | Herominde | Cortana |
| 13 | Fixed branch count (ranked) | Herominde | Cortana |
| 14 | Brain-version stamp | Herominde | Cortana |

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
- The DeepSeek-V3 code is not included in this repository. Fetch it yourself
  and point `PCH_SPECIMEN_DIR` at its `inference` folder (see `pch/README.md`).

## What this is

- **LithoLattice** (`engine/`) — a C++20 voxel engine: deterministic
  simulation, OpenGL 3.3 and Vulkan renderers, procedural generation,
  resource packs, sandboxed AngelScript modding, and a software renderer
  that serves as the low-end baseline.
- **LithoKraft** (`game/LithoKraft/`) — the voxel CPU-building game built
  on the engine: build circuits big, print them small with lithography,
  build bigger. Seven-dimension design; the first world is the Wafer.
- **The Timing Handshake** — the engine's default simulation-timing mode:
  Time Serialization with control coupling and event coupling. Every step
  is serialized; control enters only at step boundaries; the full run is
  recorded as a sealed, hash-chained stream that can be replayed and
  verified bit-for-bit.
- **Personal Control Harness — PCH** (`pch/`) — the same three pillars
  applied to a language model's control loop: a model can personally plan
  and control its own harness verifiably before it takes actions. Ships
  with a working reference harness against a public reference model, two
  sealed run streams, and full receipts.

A Wet Wafer Works Games Incorporated project. Inventor and designer:
JaydeeWetwork. Full credits: `game/LithoKraft/CREDITS.md`.

## Verifiable claims

These claims are the point of the release — anyone can check them from
this tree alone:

- **Game determinism.** The engine's scripted demo scenario hashes to
  `cb32c824f8b3ea98` (cities on) and `e015e51cf1559089` (cities off),
  identically across record, no-recorder, legacy-replay, and
  handshake-replay runs. The shipped proof file
  `demo_record_proof.lldemo` verifies COMPLETE; a one-byte tamper makes
  verification fail.
- **PCH replay.** `pch/harness/stream_sampled.jsonl` and
  `stream_greedy.jsonl` are sealed, hash-chained step streams. The
  verifier re-derives every committed token: tampering breaks the chain,
  and a re-sealed forgery is refused on re-derivation. Re-running the
  test regenerates both streams byte-identically on the same platform. Float results differ across OS/CPU/torch builds, so streams carry a hashed platform tag and there is a reference set per platform (Linux and Windows today); see `BUILDING.md` (core fix approved by JD 2026-10-23).
- **Honest scope.** The PCH proof runs at toy scale with synthetic
  fixture weights — it proves the protocol properties (serialization,
  verification, rewind/branch, witnessed cross-checking), not production
  model quality. The game is a pre-alpha vertical slice. Known paperwork
  item: the game demo test's built-in expected hash pin
  (`5041cf1795c33a8c`) is stale relative to the canonical cities-on hash
  above; the machinery is unaffected.

Receipts live in `pch/docs/`; the step-by-step transcript of the PCH
package verifying itself from a cold environment is
`pch/VERIFY-TRANSCRIPT.txt`.

## Build

Prerequisites: a C++20 toolchain (MSVC on Windows, GCC/Clang on Linux),
CMake, and Python 3. CMake fetches the remaining dependencies itself
(GLFW, AngelScript, miniaudio, stb, Vulkan headers).

- One command (configure → build → test): `python tools/build.py`
  (Linux: `python3 tools/build.py`)
- Or drive CMake directly from `engine/CMakeLists.txt`.
- Full build and test with presets (CMake 3.25+): see [BUILDING.md](BUILDING.md).

Tested on Windows 11 (MSVC, VS 2026 Build Tools) and Linux.

Example `ctest --preset vs2026-release` tail (Windows 11, 2026-10-23; tests
that need something absent, such as the DeepSeek-V3 specimen or symlink
privilege, report Skipped rather than Passed):

```
14/65 Test #14: engine_storage .......................***Skipped   0.60 sec
22/65 Test #22: engine_audio .........................   Passed    3.58 sec
49/65 Test #49: pch.shipped_streams_chain ............   Passed    0.12 sec
...
100% tests passed, 0 tests failed out of 65
```

Start with `engine/README.md` for the engine source map,
`game/LithoKraft/README.md` for the game, `engine/docs/` and `docs/` for
architecture, and `game/LithoKraft/CREDITS.md` for who built what.

## The Pillars

This work ships under a stated set of design commitments — no
immortality seeking, no technology integrated into the human nervous
system, freedom of choice kept open, no genetic modification — recorded
in full in `pch/PILLARS.md`. They are part of the design, not an
appendix: the verification architecture exists so that power stays
checkable by the people it affects.

*Power to the people. Power to the programs. Power to the pillars.*
