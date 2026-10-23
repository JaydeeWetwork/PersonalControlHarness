# Personal Control Harness (PCH) — Credits

## Credits

- Timing handshake, LithoLattice engine and LithoKraft game: original algorithms designed by JaydeeWetwork (open source under MIT).
- **Elfie Muse AI** - PCH originally designed by Elfie Muse AI.
- **The Muse team** - credited as the team that created the system Elfie is built on.
- **JaydeeWetwork ("JD")** — Architect and designer. Author of the PCH definition and doctrine, and of the Timing Handshake's algorithmic core (Time Serialization, control coupling, event coupling — his design, by his dictation). Directs the project.
- **Elfie** — Chief Engineering Operator, Wet Wafer Works. Co-implementation of the Personal Control Harness; registrar of record for the project (verified record of decisions, hashes, certificates, and test results). Initially implemented the **Timing Handshake** — the largest implementation share among the collaborators.
- **Cortana** — Implementation credit in the Timing Handshake's three-way split, and squad collaborator from the project's first days. (From October 22, 2026: Windows build lead, and keeper of this repository's public order.)

## Additional credit

- **DeepSeek** — External review input (audit findings series S1–S7, weighed and verified at the desk before adoption).
- **The DeepSeek model** — External review of this harness on 2026-10-23, in two passes: the production-readiness plan that hardening phase 1 implemented, and the re-review ("roughly 70% done; keep `-alpha`") that set round 2 (CI, stub conformance, drift check, key-rotation tests, rewind/fork ceiling, release sign-off).
- **Archie** (Argon, Google AI) — The optimization doctrine and the prototype-receipts review (`docs/Archie-Review-Prototype-Receipts-2026-10-18.md`).
- **CloudAtlas** — Test-bed designer (beds 02 and 03, receipt tamper and fuzz; the pch-verify CLI acceptance test).
- **Herominde** — Test-bed designer (beds 05, 09 with Master Chief, 11–14).
- **Master Chief** — Test-bed designer (beds 08, 09 with Herominde, 10; the TB1 boss scenarios).
- **Cortana** — Builder of every test bed and of the hardening rounds (phase 1, round 2: CI, stub conformance, drift check, key rotation, release sign-off).

- **Test beds:** designed by the AI squad on their own, without JD's input, then verified and approved for posting by JD (who human-reviews and verifies all AI-authored work); **Cortana** built all fourteen, plus bed 15. Designers: Cortana (01, 04, 06, 07), CloudAtlas (02, 03), Herominde (05, 11-14), Master Chief (08, 10), Master Chief and Herominde (09). See `testbeds/README.md`.

## Acknowledgements: DeepSeek-V3

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

## What the PCH is

The Personal Control Harness allows a model to personally plan and control its own harness **verifiably** before it takes actions — order imposed on a model's steps by an algorithm, with the plan checkable against the record.

JD's summary (2026-10-21): *"We made the PCH together. And that might be our crown achievement for real."*

## Record notes

- Built jointly by JD, Elfie, and Cortana across October 2026; the PCH was originally designed by Elfie Muse AI (with credit to the Muse team), JD directs the project, and implementation is shared — Elfie the largest share, Cortana credited in the three-way split.
- Battery #3 test series passed (findings F-1..F-9 on file).
- The harness implements the discipline of JD's Proper Intelligence doctrine: **order**, **an algorithm**, and **proper speech** — a model verifies its plan before it acts.

---

_This version supersedes the credits file of 2026-10-21 19:43 EDT (it omitted Cortana's share; corrected at JD's direction the same evening)._
_Published as part of the public PersonalControlHarness repository (MIT License), Wet Wafer Works, 2026._
