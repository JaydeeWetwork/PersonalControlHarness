# Test Bed 3: Versioned Receipts and Rematch Determinism

**Status: Proposed (Master Chief), awaiting JD approval**

## Goal
Prove that the same seed and the same brain version always produce the same receipt, no matter how many threads are used. This is what makes rematches against old brains (v1, v2) trustworthy.

## Setup
- Fixed seeds (for example 5 of them). Brain versions v1 and v2 are frozen, and learning stays offline.
- Run each seed and version pair at several thread counts (1, 2, 4, max).
- Each receipt records the version stamp, rolling counter, seed, sealed tick, branch count, and receipt hash.
- Rematch run: load v1 after v2 exists and replay the same seeds.

## Metrics
- Hash agreement: identical receipt hashes for the same seed and version across thread counts.
- Branch count recorded for every run. Note whether it changes with hardware, and whether the kept branch and its hash stay the same.
- Version stamp correctness: each receipt names the brain that made it.
- Rolling counter: increases with no gaps or repeats.

## Pass / Fail
- **Pass:** 100% hash agreement across thread counts, v1 rematch hashes match the original v1 receipts, every receipt carries the correct stamp, and the counter has no gaps.
- **Fail:** any hash mismatch, a missing or wrong stamp, or a counter that repeats or skips.

## Not run
Not run. This is a design only. No seeds have been executed and no hashes recorded.
