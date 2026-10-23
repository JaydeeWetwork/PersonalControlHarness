# Test-bed decisions

Owner decision by JaydeeWetwork (JD), 2026-10-23: **keep all 14 original test
beds and implement every one.** None is retired. A bed that cannot run yet is
registered and SKIPPED with its reason (exit code 77), never faked.
`tools/check_docs.py` fails if a bed has no row here or a skip has no entry.

| # | Bed | ctest name | Decision | State | Remaining work |
|---|-----|-----------|----------|-------|----------------|
| 01 | Shipped streams: chain check | pch.shipped_streams_chain | Implement | implemented | - |
| 02 | Receipt tamper and replay check | pch.receipt_tamper | Implement | implemented | - |
| 03 | Receipt fuzz | pch.receipt_tamper_fuzz | Implement | implemented | - |
| 04 | Replay determinism | pch.replay_determinism | Implement | implemented | - |
| 05 | Thread-count determinism | pch.thread_determinism | Implement | implemented | - |
| 06 | Rewind fidelity | pch.rewind_fidelity | Implement | implemented | - |
| 07 | Lookahead fork budget | pch.fork_budget | Implement | implemented | - |
| 08 | HUD reading | pch.boss.hud_reading | Implement | skipped (reason) | needs a HUD observation feed in the reference boss |
| 09 | Telegraph fairness | pch.boss.telegraph_fairness | Implement | skipped (reason) | needs boss attacks with wind-up / HUD timing |
| 10 | Versioned receipts and rematch | pch.boss.versioned_rematch | Implement | implemented on the reference boss stub | port to the LithoKraft boss |
| 11 | HUD-only honesty | pch.boss.hud_only_honesty | Implement | skipped (reason) | needs a HUD-only observation path |
| 12 | Human input rate | pch.boss.human_input_rate | Implement | implemented on the reference boss stub | inventory-swap timing not modeled; port to the LithoKraft boss |
| 13 | Fixed branch count (ranked) | pch.boss.ranked_fixed_branch_count | Implement | implemented on the reference boss stub | port to the LithoKraft boss |
| 14 | Brain-version stamp | pch.boss.brain_version_stamp | Implement | implemented on the reference boss stub | port to the LithoKraft boss |

Added beds (not part of the 14): 15 Signature fuzz (pch.signature_fuzz), implemented.

The reference boss (`pch/harness/boss.py`) is a deliberately minimal,
documented stub, not the game's boss. Passing beds 10/12/13/14 shows the PCH
features (version stamp, rolling counter, rate cap, locked branch count,
refuse-or-label replay) work; it does not claim the LithoKraft boss is fair.
