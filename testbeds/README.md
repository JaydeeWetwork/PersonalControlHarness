# PCH test beds

Worked examples and tests for the Personal Control Harness. Each runs on its
own with `python <script>` from this folder, or all together through CTest.
Free tools only. **Start with example 01.**

The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these
independently, with no direction from JD. The original design notes, with
author credit, are in `designs/`. Every test uses the harness read-only.

| Experiment | Designer | Status |
|---|---|---|
| [Shipped streams: chain check (start here)](examples/01-shipped-streams-chain.md) | Cortana | verified |
| [Receipt tamper and replay check (8 cases)](examples/02-receipt-tamper.md) | CloudAtlas | verified |
| [Receipt fuzz (10,000 mutations)](examples/03-receipt-tamper-fuzz.md) | CloudAtlas | verified |
| [Replay determinism](examples/04-replay-determinism.md) | Cortana (folded into Herominde's fair-play set) | skipped |
| [Thread-count determinism](examples/05-thread-determinism.md) | Herominde | skipped |
| [Rewind fidelity](examples/06-rewind-fidelity.md) | Cortana | skipped |
| [Lookahead fork budget](examples/07-fork-budget.md) | Cortana | skipped |
| [HUD reading](examples/08-hud-reading.md) | Master Chief | skipped |
| [Telegraph fairness](examples/09-telegraph-fairness.md) | Master Chief and Herominde | skipped |
| [Versioned receipts and rematch](examples/10-versioned-rematch.md) | Master Chief | skipped |
| [HUD-only honesty](examples/11-hud-only-honesty.md) | Herominde | skipped |
| [Human input rate](examples/12-human-input-rate.md) | Herominde | skipped |
| [Fixed branch count (ranked)](examples/13-ranked-fixed-branch-count.md) | Herominde | skipped |
| [Brain-version stamp](examples/14-brain-version-stamp.md) | Herominde | skipped |

Status as of 2026-10-23 on a Windows 11 workstation (Python 3.14, no torch).
*skipped* = exits with code 77, so CTest shows "Skipped" with the reason in
the test output (`ctest --preset testbeds-only -V`).

Run everything: `cmake --preset testbeds-only && ctest --preset testbeds-only`
(see `../BUILDING.md`).
