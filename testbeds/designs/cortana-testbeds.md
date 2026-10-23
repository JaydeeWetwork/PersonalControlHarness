# Cortana's PCH test bed designs (Proposed by Cortana, 2026-10-22) - designs only, nothing run
All on my computer only, using sandbox copies. No engine source, no harness core fixes.

## TB-1 Replay determinism
Run the same seeded scenario 16 times and hash the receipt every 64 ticks.
Pass: every hash matches. Any mismatch logs the first tick where the runs diverge.

## TB-2 Lookahead fork budget (JD's planning idea)
Fork at a sealed tick into 1, 2, 4, 8 and 16 branches with a fixed tick horizon.
Record wall time, peak RAM and best-branch score for each count.
Question: where does adding branches stop helping on a 2-core machine versus a 16-thread workstation?

## TB-3 Rewind fidelity
Snapshot, run N ticks, rewind, then re-run.
Pass: the rewound state hash equals the snapshot hash, for N = 1, 64, 1024, 16384 and 65536 (longer runs per JD), with a replay-determinism check on each.

## TB-4 Receipt tamper check: handed to CloudAtlas (security lane)
