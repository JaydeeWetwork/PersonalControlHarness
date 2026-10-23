# Fail-before / pass-after record (security changes, 2026-10-23)

Each security test below was run twice on the box (Linux, Python 3.13): against the pre-fix harness at `3198eee` (`PCH_HARNESS_DIR=<worktree of 3198eee>/pch/harness`) and against the fixed tree. Output is verbatim.

## S1.3 trailer in chain: `tb_trailer_chain.py`

Before (3198eee):
```
  [PASS] clean re-closed receipt accepted -- accepted (old core)
  [FAIL] trailer carries schema 2 and its own chain link
  [FAIL] core rejects: extra trailer field -- accepted (old core)
  [FAIL] core rejects: step count edited -- accepted (old core)
  [FAIL] core rejects: schema downgraded (field removed) -- accepted (old core)
  [FAIL] core rejects: trailer chain link edited -- accepted (old core)
  [PASS] core rejects: final_chain edited -- trailer seal mismatch

TRAILER IN CHAIN (S1.3): 5 FAILED
exit 1
```
After:
```
  [PASS] clean re-closed receipt accepted -- 16 steps, chained
  [PASS] trailer carries schema 2 and its own chain link
  [PASS] core rejects: extra trailer field -- seal: trailer has unexpected or missing fields
  [PASS] core rejects: step count edited -- seal: trailer is not in the hash chain (edited trailer)
  [PASS] core rejects: schema downgraded (field removed) -- seal: legacy trailer has unexpected or missing fields
  [PASS] core rejects: trailer chain link edited -- seal: trailer is not in the hash chain (edited trailer)
  [PASS] core rejects: final_chain edited -- seal: trailer is not in the hash chain (edited trailer)
  [PASS] shipped legacy stream still readable, labeled legacy-unchained -- 16 steps, legacy-unchained

TRAILER IN CHAIN (S1.3): ALL PASS
exit 0
```
