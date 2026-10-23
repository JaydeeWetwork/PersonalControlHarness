# PCH code review, 2026-10-23

Directed by JaydeeWetwork. Built by Cortana. Credit to the DeepSeek team for the specimen inference code.

## Scope
- **Python, read in full:** `pch/harness/stepstream.py`, `receipt_sig.py`, `harness.py`, `boss.py`, `weights.py`, and `pch/tools/*`.
- **C++, read:** the `engine/src/sim/demo_record.cpp` parser and salvage path.
- **C++, wider check:** the whole tree (about 144k lines) was built on Linux with GCC (`-Wall -Wextra`) and the warnings were triaged. It was not read line by line.
- **CMake:** a configure and build on Linux; the Windows build uses the existing VS 2026 preset.
- **Docs:** checked by `tools/check_docs.py`.

## Findings fixed
Every fix is in commit `dc4de67`. The regression tests are in `testbeds/tb_review_fixes.py` (ctest `pch.review_regressions`). The output from before the fixes is recorded in `docs/hardening/FAIL-BEFORE.md`.

| ID | Area | Bug | Fix | Fail-before |
|---|---|---|---|---|
| R1 | canonicalization | `canonical()` hashed NaN/Infinity. `loads_strict` rejects these, so a receipt could be sealed and signed but then never verified. | `allow_nan=False` | yes |
| R2 | robustness | Deeply nested JSON raised an uncaught `RecursionError` in `check_sealed`. | Catch it and report the record as malformed. | yes |
| R3 | security/replay | `verify_replay` trusted the `step` numbers. A re-hashed forgery with duplicated or skipped steps was accepted. A missing prefill crashed with `StopIteration`. | Added `check_step_sequence()` (steps must run exactly 0..n-1); require exactly one prefill and a prompt list. | yes |
| R4 | portability | `save_stream`/`load_stream` used the platform encoding and newlines, so a stream saved on Windows had different bytes (CRLF, cp1252). | UTF-8 and `newline="\n"` | Windows only; Linux passed before |
| R5 | security | Revocation entries with an uppercase, padded or short `key_id` were accepted and then never matched, so the "revoked" key stayed trusted. | Require 16 lowercase hex chars; otherwise raise `SigError`. | yes |
| R6 | UX | `pch_sign` printed errors to stdout and had no help text. `pch_verify` reported 0-based line numbers. | Errors go to stderr, help text added, line numbers are 1-based. | n/a |
| R7 | C++ UB | `scout_selftest.cpp:232` bound a reference to an element of the temporary returned by `waferMapPalette()`, so it dangled (GCC `-Wdangling-reference`, `-Wmaybe-uninitialized`). | Copy the palette into a local first. | compiler diagnostic |

## Reviewed, no bug found
- **`demo_record.cpp` `parseDemo`/`salvageSegment`:** every read is length-checked before use, and segment counts are bounded by the remaining bytes.
- **Demo checksum:** it is a non-cryptographic hash. It detects accidental corruption only. Authenticity comes from the Ed25519 receipt signature, not from this checksum.
- **Weights resolver:** shard names that try to escape the weights folder are rejected. Paths containing spaces are covered by `tb_weights_path.py`.

## Notes / deferred
- **`key_id` is 64 bits** (a truncated hash of the public key). That is fine as an identifier, but not as an authenticator. Verification always uses the full public key.
- **Bed 08 match rate (100%) is near-tautological.** The reference HUD brain and the expectation rule share one policy. The beds with teeth are `hud_only_honesty` (the x-ray control is caught) and `telegraph_fairness`.
- **GCC warnings left as they are:** 546 `-Wunused-parameter` and 58 `-Wmissing-field-initializers`. They are cosmetic, and changing them would mean touching a lot of the engine for no behaviour change.
- **Still open for JD, and deliberately not touched:** the demo-hash repin (BLOCKERS B6) and the Qwen2 loader and weights (B4).
