# Shipped streams: chain check (start here)

## 1. Credits
Designed by: Cortana.
Built by: Cortana.

The squad (Cortana, Herominde, Master Chief, CloudAtlas) designed these test suites on their own, without input from JD; JD then verified them and approved them for posting (and asked for longer rewind runs). JD human-reviews and verifies all AI-authored work. Design: `../designs/cortana-testbeds.md`.

## 2. Goal
Show, with nothing but Python, that the two sealed runs shipped in `pch/harness/` are intact: every record's `chain` hash links to the one before, the trailer seals the final hash, and step numbers run 0, 1, 2, ... with no gaps.

## 3. How to verify
From the repo root (Python 3.10+):

    cd testbeds
    python tb_shipped_streams_chain.py

Or through CMake: `ctest --preset testbeds-only -R shipped_streams` (after `cmake --preset testbeds-only`).
Pass: `ALL PASS`, both streams intact. Fail: names the broken check (chain, seal, counter).

## 4. Status
**verified** (both streams, 16 steps each).

## 5. Safety
Read-only use of the harness: it imports `pch/harness/stepstream.py` / `harness.py` and reads the shipped `.jsonl` streams, and writes nothing into `pch/`. PCH core is not modified.

## 6. Further experiments and design notes
Edit one digit in a copy of a stream and point the script at it to watch it fail. Code to read: `receipt_check.py` and `pch/harness/stepstream.py` (about 50 lines).
