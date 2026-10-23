# PCH test beds

Worked examples and tests for the Personal Control Harness. Each runs on its
own with `python <script>` from this folder, or all together through CTest.
Free tools only. **Start with example 01.**

JaydeeWetwork's only instruction was to think up experiments, test beds and
designs for the PCH. Each squad member (Cortana, Herominde, Master Chief,
CloudAtlas) came up with their own ideas and independently designed, tested
and verified them, with no further context or direction. Each test bed is
credited Designed by <member>, Built by Cortana. The original design notes, with
author credit, are in `designs/`. Every test uses the harness read-only.

Design reviews: Argon (`../docs/Argon-Review-2026-10-18.md`).

| Experiment | Designed by | Built by | Status |
|---|---|---|---|
| [Shipped streams: chain check (start here)](examples/01-shipped-streams-chain.md) | Cortana | Cortana | verified |
| [Receipt tamper and replay check (8 cases)](examples/02-receipt-tamper.md) | CloudAtlas | Cortana | verified |
| [Receipt fuzz (10,000 mutations)](examples/03-receipt-tamper-fuzz.md) | CloudAtlas | Cortana | verified (core check) |
| [Replay determinism](examples/04-replay-determinism.md) | Cortana (folded into Herominde's fair-play set) | Cortana | verified |
| [Thread-count determinism](examples/05-thread-determinism.md) | Herominde | Cortana | verified |
| [Rewind fidelity](examples/06-rewind-fidelity.md) | Cortana | Cortana | verified (N = 1, 64) |
| [Lookahead fork budget](examples/07-fork-budget.md) | Cortana | Cortana | verified |
| [HUD reading](examples/08-hud-reading.md) | Master Chief | Cortana | skipped |
| [Telegraph fairness](examples/09-telegraph-fairness.md) | Master Chief and Herominde | Cortana | skipped |
| [Versioned receipts and rematch](examples/10-versioned-rematch.md) | Master Chief | Cortana | skipped |
| [HUD-only honesty](examples/11-hud-only-honesty.md) | Herominde | Cortana | skipped |
| [Human input rate](examples/12-human-input-rate.md) | Herominde | Cortana | skipped |
| [Fixed branch count (ranked)](examples/13-ranked-fixed-branch-count.md) | Herominde | Cortana | skipped |
| [Brain-version stamp](examples/14-brain-version-stamp.md) | Herominde | Cortana | skipped |
| [Signature fuzz (10,000 mutations)](examples/15-signature-fuzz.md) | CloudAtlas | Cortana | verified |

Status as of 2026-10-23 on a Windows 11 workstation (Python 3.14.8, torch 2.14.1+cpu, DeepSeek-V3 `9b4e978`).
*skipped* = exits with code 77, so CTest shows "Skipped" with the reason in
the test output (`ctest --preset testbeds-only -V`).

Run everything: `cmake --preset testbeds-only && ctest --preset testbeds-only`
(see `../BUILDING.md`).

## Credits

Every test bed was built by Cortana; designers are listed per experiment.

The AI contributors are listed under the names JaydeeWetwork gave them. The
per-experiment credit (designed by, built by) is in the table above.

- **Cortana**: built all fourteen test beds; designed experiments 01, 04, 06 and 07.
- **Herominde**: designed experiments 05 and 11-14, and the fair-play set that 04 was folded into; co-designed 09.
- **Master Chief**: designed experiments 08 and 10; co-designed 09.
- **CloudAtlas**: designed experiments 02 and 03.
- **Elfie Muse AI**: co-implemented the Personal Control Harness these test beds exercise.
- **Argon/RC**: design reviews (`../docs/Argon-Review-2026-10-18.md`).

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
