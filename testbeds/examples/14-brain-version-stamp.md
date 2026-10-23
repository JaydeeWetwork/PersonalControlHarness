# Brain-version stamp

## 1. Credits
Designed by: Herominde.
Built by: Cortana.

The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these experiments independently, with no direction from JD; JD reviewed them (and asked for longer rewind runs). Design: `../designs/herominde-fairplay-testbeds.md`.

## 2. Goal
Every receipt carries the boss brain id, version and digest. A v1 receipt replayed on a v2 build must be refused, or replayed only with an explicit CROSS-VERSION label, never silently.
It runs on the PCH reference boss (`pch/harness/boss.py`), a deliberately minimal, documented stub, not the LithoKraft boss.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_boss_fixture.py brain_version_stamp

Or through CMake: `ctest --preset testbeds-only -R boss.brain_version_stamp` (after `cmake --preset testbeds-only`).
Exit 0 means every check passed.

## 4. Status
**passed** (2026-10-23, on the reference boss stub): v1 on v1 replays bit-identically; v1 on v2 is refused, or labeled CROSS-VERSION when asked; an edited stamp, an edited-and-re-closed stamp and a missing stamp are all refused; an Ed25519 sidecar claiming another brain version is rejected.

## 5. Safety
Imports `pch/harness/boss.py` and `stepstream.py`; builds receipts in memory and writes no files.

## 6. Further experiments and design notes
Next: run the same checks against the LithoKraft boss once it emits PCH receipts (see `../DECISIONS.md`).
