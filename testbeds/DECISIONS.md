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
| 10 | Versioned receipts and rematch | pch.boss.versioned_rematch | Implement | skipped (reason) | being implemented on the PCH reference boss stub (T2.2) |
| 11 | HUD-only honesty | pch.boss.hud_only_honesty | Implement | skipped (reason) | needs a HUD-only observation path |
| 12 | Human input rate | pch.boss.human_input_rate | Implement | skipped (reason) | being implemented on the PCH reference boss stub (T2.2) |
| 13 | Fixed branch count (ranked) | pch.boss.ranked_fixed_branch_count | Implement | skipped (reason) | being implemented on the PCH reference boss stub (T2.2) |
| 14 | Brain-version stamp | pch.boss.brain_version_stamp | Implement | skipped (reason) | being implemented on the PCH reference boss stub (T2.2) |

Added beds (not part of the 14): 15 Signature fuzz (pch.signature_fuzz), implemented.

Beds 10, 12, 13 and 14 are scheduled first (they need only PCH-side features).
