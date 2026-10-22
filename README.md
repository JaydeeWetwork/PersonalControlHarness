# LithoLattice · LithoKraft · Personal Control Harness

> **Initial public release staging — published by the author via GitHub.**
> License chosen by JD on 2026-10-19: **MIT** (see `LICENSE`; decision
> record in `LICENSE-OPTIONS.md`).

*The Wafer, the way forward.*

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
David Rittenhouse. Full credits: `game/LithoKraft/CREDITS.md`.

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
  test regenerates both streams byte-identically.
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

## Layout

```
engine/     LithoLattice engine (src/, assets/, tests/, docs/, branding/)
            engine/LithoKraft is a symlink to ../../game/LithoKraft —
            keep it intact; the build reaches the game through it.
game/       LithoKraft game (src/, assets/, scripts/, CREDITS.md)
pch/        Personal Control Harness reference package (self-contained)
docs/       Project documentation (architecture, SDK, roadmap)
tools/      Build driver and asset tools
demo_record_proof.lldemo   Sealed demo record (verification artifact)
MANIFEST.md                Exactly what this staging contains, and what
                           was deliberately left out, and why
LICENSE-OPTIONS.md         DRAFT — the license decision, still open
```

## Build

Prerequisites: a C++20 toolchain (MSVC on Windows, GCC/Clang on Linux),
CMake, and Python 3. CMake fetches the remaining dependencies itself
(GLFW, AngelScript, miniaudio, stb, Vulkan headers).

- One command (configure → build → test): `python tools/build.py`
  (Linux: `python3 tools/build.py`)
- Or drive CMake directly from `engine/CMakeLists.txt`.

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
