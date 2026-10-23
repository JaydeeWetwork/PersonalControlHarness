# Personal Control Harness (PCH) — Credits

## Credits

- **Elfie Muse AI** - PCH originally designed by Elfie Muse AI.
- **The Muse team** - credited as the team that created the system Elfie is built on.
- **JaydeeWetwork ("JD")** — Architect and designer. Author of the PCH definition and doctrine, and of the Timing Handshake's algorithmic core (Time Serialization, control coupling, event coupling — his design, by his dictation). Directs the project.
- **Elfie** — Chief Engineering Operator, Wet Wafer Works. Co-implementation of the Personal Control Harness; registrar of record for the project (verified record of decisions, hashes, certificates, and test results). Initially implemented the **Timing Handshake** — the largest implementation share among the collaborators.
- **Cortana** — Implementation credit in the Timing Handshake's three-way split, and squad collaborator from the project's first days. (From October 22, 2026: Windows build lead, and keeper of this repository's public order.)

## Additional credit

- **DeepSeek** — External review input (audit findings series S1–S7, weighed and verified at the desk before adoption).

- **Test beds:** designed by the squad independently, with no direction from JD; **Cortana** built all fourteen. Designers: Cortana (01, 04, 06, 07), CloudAtlas (02, 03), Herominde (05, 11-14), Master Chief (08, 10), Master Chief and Herominde (09). See `testbeds/README.md`.

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
- The DeepSeek-V3 code is not included in this repository. Fetch it yourself
  and point `PCH_SPECIMEN_DIR` at its `inference` folder (see `pch/README.md`).

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
